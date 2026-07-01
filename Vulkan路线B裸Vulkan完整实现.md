# 路线 B：裸 Vulkan + QVulkanInstance 完整实现

> 本文给出**路线 B（裸 Vulkan）** 的完整代码：用 `QVulkanInstance` 建实例，
> 自己管理 device / queue / command buffer / pipeline / 同步，做离屏合成 + 异步回读。
> 对外接口与 `GlCompositor` 一致（两路 `QVideoSink` 进、`frameReady(QImage)` 出），
> 上层 `DesktopCapturer` 用 `using GlCompositor = VkCompositor;` 即可零改动切换。
>
> ⚠️ 这是 OpenGL 版 3~5 倍的代码量，所有同步手写。建议先读完《Vulkan替换OpenGL渲染方案.md》
> 的路线对比，确认确实需要裸 Vulkan 的异步吞吐再动手。

## 0. 整体设计

```
                         ┌─────────────── 渲染线程（配合独立线程方案）───────────────┐
QScreenCapture ─sink─►   onScreenFrame                                              │
QCamera        ─sink─►   m_lastCamera = f                                           │
                         │                                                          │
                         │  环形使用 N=2 帧并发(frame-in-flight)：                    │
                         │  ① 取一个空闲 frame slot（fence 已 signaled）             │
                         │  ② 录制 cmd：上传纹理 → renderpass 画桌面+相机 → blit到    │
                         │     host-visible readback buffer                          │
                         │  ③ submit + fence                                         │
                         │  ④ 回收"上上帧"已完成的 slot：map 内存 → QImage → emit     │
                         └──────────────────────────────────────────────────────────┘
```

关键点：**回读落后 1 帧**。提交第 N 帧后不等它，而是去取第 N-1 帧（其 fence 此时
多半已 signaled）的 readback buffer 拷出来。CPU 几乎不空等，这是裸 Vulkan 相对
QRhi 离屏模式的核心优势。

## 1. 依赖与构建（CMakeLists.txt）

```cmake
find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets Gui)
find_package(Vulkan REQUIRED)        # 需安装 LunarG Vulkan SDK，设好 VULKAN_SDK

target_sources(qt-learn PRIVATE VkCompositor.h VkCompositor.cpp)

target_link_libraries(qt-learn PRIVATE
    Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets Qt6::Gui
    Vulkan::Vulkan)
```

着色器预编译为 SPIR-V（用第 4 节《Vulkan替换OpenGL渲染方案.md》给出的 450 版源码）：
```bash
glslangValidator -V composite.vert -o composite.vert.spv
glslangValidator -V composite.frag -o composite.frag.spv
```
把两个 `.spv` 放进 `shader.qrc`：
```xml
<qresource prefix="/Shader">
    <file>composite.vert.spv</file>
    <file>composite.frag.spv</file>
</qresource>
```

## 2. VkCompositor.h（完整）

```cpp
#ifndef VKCOMPOSITOR_H
#define VKCOMPOSITOR_H

#include <QObject>
#include <QVideoSink>
#include <QVideoFrame>
#include <QImage>
#include <QSize>
#include <QVulkanInstance>
#include <QVulkanFunctions>
#include <vulkan/vulkan.h>
#include <array>

class VkCompositor : public QObject
{
    Q_OBJECT
public:
    explicit VkCompositor(QObject *parent = nullptr);
    ~VkCompositor();

    QVideoSink* screenSink() const { return m_screenSink; }
    QVideoSink* cameraSink() const { return m_cameraSink; }
    QSize       outputSize() const { return m_outputSize; }
    void        setOverlay(bool on) { m_overlay = on; }
    bool        overlay() const { return m_overlay; }

public slots:
    void init();      // 渲染线程内调用：建 instance/device/pipeline/资源
    void cleanup();   // 渲染线程内调用：vkDeviceWaitIdle + 销毁全部对象

signals:
    void frameReady(const QImage &frame);

private:
    static constexpr int kFramesInFlight = 2;   // 并发帧数

    // 一张被采样的纹理（桌面/相机各一份）
    struct Tex {
        VkImage        image  = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView    view   = VK_NULL_HANDLE;
        QSize          size;
    };

    // 每个并发帧自带的命令缓冲、同步原语、回读缓冲
    struct Frame {
        VkCommandBuffer cmd        = VK_NULL_HANDLE;
        VkFence         fence      = VK_NULL_HANDLE;  // 提交后 signal
        VkBuffer        readback   = VK_NULL_HANDLE;  // host-visible
        VkDeviceMemory  readbackMem= VK_NULL_HANDLE;
        void*           mapped     = nullptr;          // 持久映射指针
        bool            pending    = false;            // 是否有未回收的结果
    };

    // ---- Qt 媒体侧 ----
    QVideoSink* m_screenSink = nullptr;
    QVideoSink* m_cameraSink = nullptr;
    QVideoFrame m_lastCamera;

    void onScreenFrame(const QVideoFrame &f);

    // ---- Vulkan 句柄 ----
    QVulkanInstance     m_inst;
    VkPhysicalDevice    m_phys   = VK_NULL_HANDLE;
    VkDevice            m_dev    = VK_NULL_HANDLE;
    uint32_t            m_qfam   = 0;            // 图形+传输队列族
    VkQueue             m_queue  = VK_NULL_HANDLE;
    VkCommandPool       m_pool   = VK_NULL_HANDLE;

    // 离屏目标
    VkImage         m_color    = VK_NULL_HANDLE;
    VkDeviceMemory  m_colorMem = VK_NULL_HANDLE;
    VkImageView     m_colorView= VK_NULL_HANDLE;
    VkRenderPass    m_rp       = VK_NULL_HANDLE;
    VkFramebuffer   m_fb       = VK_NULL_HANDLE;

    // 管线
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipeLayout= VK_NULL_HANDLE;
    VkPipeline            m_pipe      = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool  = VK_NULL_HANDLE;
    VkSampler             m_sampler   = VK_NULL_HANDLE;

    VkBuffer        m_vbuf=VK_NULL_HANDLE, m_ibuf=VK_NULL_HANDLE;
    VkDeviceMemory  m_vmem=VK_NULL_HANDLE, m_imem=VK_NULL_HANDLE;

    Tex m_texScreen, m_texCamera;
    std::array<Frame, kFramesInFlight> m_frames;
    int m_curFrame = 0;

    bool        m_overlay = true;
    const QSize m_outputSize{1920, 1080};
    const QSize m_camSize{480, 360};
    const int   m_margin = 20;

    QVulkanDeviceFunctions* m_df = nullptr;   // Qt 提供的 device 级函数表

    // ---- 内部辅助（实现见后文）----
    uint32_t findMemType(uint32_t bits, VkMemoryPropertyFlags props);
    void createBuffer(VkDeviceSize sz, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags props,
                      VkBuffer &buf, VkDeviceMemory &mem);
    void createOffscreenTarget();
    void createPipeline();
    void createFrames();
    void uploadTexture(Tex &t, const QVideoFrame &f, VkCommandBuffer cmd);
    void recordDraw(VkCommandBuffer cmd, bool hasScreen, bool hasCamera);
    void recycleFrame(Frame &fr);
    VkShaderModule loadSpv(const QString &path);
};
#endif // VKCOMPOSITOR_H
```

## 3. VkCompositor.cpp — 构造 / init / 设备选择

```cpp
#include "VkCompositor.h"
#include <QFile>
#include <QDebug>
#include <vector>

#define VK_CHECK(x) do { VkResult _r=(x); if(_r!=VK_SUCCESS) \
    qFatal("Vulkan 调用失败 %s = %d", #x, _r); } while(0)

VkCompositor::VkCompositor(QObject *parent) : QObject(parent)
{
    m_screenSink = new QVideoSink(this);
    m_cameraSink = new QVideoSink(this);
    // AutoConnection：媒体线程发、渲染线程收，自动队列投递
    connect(m_screenSink, &QVideoSink::videoFrameChanged,
            this, &VkCompositor::onScreenFrame);
    connect(m_cameraSink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &f){ m_lastCamera = f; });
}

VkCompositor::~VkCompositor() { /* 真正释放在 cleanup() */ }

void VkCompositor::init()
{
    // ---- 1. 实例 ----
    m_inst.setApiVersion(QVersionNumber(1, 1));
#ifndef QT_NO_DEBUG
    m_inst.setLayers({ "VK_LAYER_KHRONOS_validation" });
#endif
    if (!m_inst.create())
        qFatal("QVulkanInstance 创建失败: %d", m_inst.errorCode());

    VkInstance inst = m_inst.vkInstance();
    QVulkanFunctions *f = m_inst.functions();

    // ---- 2. 选物理设备 + 队列族（要支持 graphics）----
    uint32_t n = 0;
    f->vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    f->vkEnumeratePhysicalDevices(inst, &n, devs.data());
    if (n == 0) qFatal("没有可用的 Vulkan 物理设备");

    m_phys = devs[0];                       // 简单起见取第一个；可挑独显
    uint32_t qn = 0;
    f->vkGetPhysicalDeviceQueueFamilyProperties(m_phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qn);
    f->vkGetPhysicalDeviceQueueFamilyProperties(m_phys, &qn, qprops.data());
    bool found = false;
    for (uint32_t i = 0; i < qn; ++i)
        if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { m_qfam=i; found=true; break; }
    if (!found) qFatal("找不到图形队列族");

    // ---- 3. 逻辑设备 + 队列 ----
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = m_qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    VK_CHECK(f->vkCreateDevice(m_phys, &dci, nullptr, &m_dev));
    m_df = m_inst.deviceFunctions(m_dev);   // 之后所有 device 级调用走 m_df
    m_df->vkGetDeviceQueue(m_dev, m_qfam, 0, &m_queue);

    // ---- 4. 命令池 ----
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_qfam;
    VK_CHECK(m_df->vkCreateCommandPool(m_dev, &pci, nullptr, &m_pool));

    // ---- 5. 资源 ----
    createOffscreenTarget();
    createPipeline();
    createFrames();

    qDebug() << "✅ [Vulkan] 初始化完成，frames-in-flight =" << kFramesInFlight;
}

// 选满足属性的内存类型下标
uint32_t VkCompositor::findMemType(uint32_t bits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    m_inst.functions()->vkGetPhysicalDeviceMemoryProperties(m_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u<<i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    qFatal("找不到合适的内存类型");
    return 0;
}

void VkCompositor::createBuffer(VkDeviceSize sz, VkBufferUsageFlags usage,
                                VkMemoryPropertyFlags props,
                                VkBuffer &buf, VkDeviceMemory &mem)
{
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = sz; bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(m_df->vkCreateBuffer(m_dev, &bi, nullptr, &buf));

    VkMemoryRequirements req;
    m_df->vkGetBufferMemoryRequirements(m_dev, buf, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemType(req.memoryTypeBits, props);
    VK_CHECK(m_df->vkAllocateMemory(m_dev, &ai, nullptr, &mem));
    m_df->vkBindBufferMemory(m_dev, buf, mem, 0);
}
```

## 4. createOffscreenTarget() — 离屏颜色附件 + RenderPass + Framebuffer

```cpp
void VkCompositor::createOffscreenTarget()
{
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    const uint32_t W = m_outputSize.width(), H = m_outputSize.height();

    // ---- 颜色 image（既是渲染目标，又作为 transfer src 给 blit 回读）----
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = { W, H, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(m_df->vkCreateImage(m_dev, &ici, nullptr, &m_color));

    VkMemoryRequirements req;
    m_df->vkGetImageMemoryRequirements(m_dev, m_color, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemType(req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(m_df->vkAllocateMemory(m_dev, &ai, nullptr, &m_colorMem));
    m_df->vkBindImageMemory(m_dev, m_color, m_colorMem, 0);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = m_color; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1 };
    VK_CHECK(m_df->vkCreateImageView(m_dev, &vi, nullptr, &m_colorView));

    // ---- RenderPass：单颜色附件，结束后转成 TRANSFER_SRC 方便 blit ----
    VkAttachmentDescription att{};
    att.format = fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    // 外部依赖：确保渲染写完后再做 transfer 读
    VkSubpassDependency dep{};
    dep.srcSubpass = 0;
    dep.dstSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1; rpci.pAttachments = &att;
    rpci.subpassCount = 1;    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1; rpci.pDependencies = &dep;
    VK_CHECK(m_df->vkCreateRenderPass(m_dev, &rpci, nullptr, &m_rp));

    // ---- Framebuffer ----
    VkFramebufferCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass = m_rp;
    fci.attachmentCount = 1; fci.pAttachments = &m_colorView;
    fci.width = W; fci.height = H; fci.layers = 1;
    VK_CHECK(m_df->vkCreateFramebuffer(m_dev, &fci, nullptr, &m_fb));
}
```

## 5. createPipeline() — 着色器 / 描述符 / 图形管线

UBO 布局对应着色器里的 `Ubo { vec4 uRect; int uSwizzleBGRA; }`，按 push constant
传更省事（避免每帧多个 descriptor set），这里用 **push constant** 传 uRect+swizzle，
descriptor set 只放 sampler2D。

```cpp
VkShaderModule VkCompositor::loadSpv(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) qFatal("打不开着色器 %s", qPrintable(path));
    QByteArray code = f.readAll();
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.constData());
    VkShaderModule m;
    VK_CHECK(m_df->vkCreateShaderModule(m_dev, &ci, nullptr, &m));
    return m;
}

void VkCompositor::createPipeline()
{
    // ---- 描述符布局：binding0 = combined image sampler（片元）----
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dl{};
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 1; dl.pBindings = &b;
    VK_CHECK(m_df->vkCreateDescriptorSetLayout(m_dev,&dl,nullptr,&m_setLayout));

    // ---- push constant：vec4 uRect + int swizzle = 20 字节，凑齐 32 ----
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0; pcr.size = 32;

    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1; pl.pSetLayouts = &m_setLayout;
    pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
    VK_CHECK(m_df->vkCreatePipelineLayout(m_dev,&pl,nullptr,&m_pipeLayout));

    // ---- 采样器 ----
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(m_df->vkCreateSampler(m_dev,&si,nullptr,&m_sampler));

    // ---- 描述符池：桌面+相机两个 set ----
    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
    VkDescriptorPoolCreateInfo dp{};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = 2; dp.poolSizeCount = 1; dp.pPoolSizes = &ps;
    VK_CHECK(m_df->vkCreateDescriptorPool(m_dev,&dp,nullptr,&m_descPool));

    // ---- 着色器阶段 ----
    VkShaderModule vert = loadSpv(":/Shader/composite.vert.spv");
    VkShaderModule frag = loadSpv(":/Shader/composite.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag; stages[1].pName = "main";
```

接着固定功能状态与管线创建（紧接上面的函数体）：

```cpp
    // ---- 顶点输入：location0 = vec2 aPos，stride=8 ----
    VkVertexInputBindingDescription vib{ 0, 2*sizeof(float),
                                         VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription via{ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    VkPipelineVertexInputStateCreateInfo vis{};
    vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vis.vertexBindingDescriptionCount = 1; vis.pVertexBindingDescriptions = &vib;
    vis.vertexAttributeDescriptionCount = 1; vis.pVertexAttributeDescriptions = &via;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{ 0,0, float(m_outputSize.width()),
                        float(m_outputSize.height()), 0.f, 1.f };
    VkRect2D sc{ {0,0}, { uint32_t(m_outputSize.width()),
                          uint32_t(m_outputSize.height()) } };
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.pViewports = &vp;
    vps.scissorCount = 1;  vps.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.f;

    VkPipelineMultisampleStateCreateInfo ms;
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // 画中画叠加：开 alpha blend
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vis;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.layout = m_pipeLayout;
    gp.renderPass = m_rp;
    gp.subpass = 0;
    VK_CHECK(m_df->vkCreateGraphicsPipelines(m_dev, VK_NULL_HANDLE, 1, &gp,
                                             nullptr, &m_pipe));

    m_df->vkDestroyShaderModule(m_dev, vert, nullptr);
    m_df->vkDestroyShaderModule(m_dev, frag, nullptr);

    // ---- 顶点/索引缓冲（host-visible，简单上传一次）----
    static const float quad[] = { 0,0, 1,0, 1,1, 0,1 };
    static const uint16_t idx[] = { 0,1,2, 0,2,3 };
    auto hostFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    createBuffer(sizeof(quad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 hostFlags, m_vbuf, m_vmem);
    createBuffer(sizeof(idx),  VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                 hostFlags, m_ibuf, m_imem);
    void *p;
    m_df->vkMapMemory(m_dev, m_vmem, 0, sizeof(quad), 0, &p);
    memcpy(p, quad, sizeof(quad)); m_df->vkUnmapMemory(m_dev, m_vmem);
    m_df->vkMapMemory(m_dev, m_imem, 0, sizeof(idx), 0, &p);
    memcpy(p, idx, sizeof(idx));   m_df->vkUnmapMemory(m_dev, m_imem);
}
```

## 6. createFrames() — 每帧的命令缓冲 / fence / 持久映射回读缓冲

```cpp
void VkCompositor::createFrames()
{
    const VkDeviceSize rbSize =
        VkDeviceSize(m_outputSize.width()) * m_outputSize.height() * 4;

    VkCommandBufferAllocateInfo cba{};
    cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cba.commandPool = m_pool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;

    for (auto &fr : m_frames) {
        VK_CHECK(m_df->vkAllocateCommandBuffers(m_dev, &cba, &fr.cmd));

        // fence 初始 signaled，第一帧才能立刻取用
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(m_df->vkCreateFence(m_dev, &fci, nullptr, &fr.fence));

        // host-visible 回读缓冲 + 持久映射（避免每帧 map/unmap）
        createBuffer(rbSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     fr.readback, fr.readbackMem);
        m_df->vkMapMemory(m_dev, fr.readbackMem, 0, rbSize, 0, &fr.mapped);
        fr.pending = false;
    }
}
```

## 7. onScreenFrame() — 核心：取槽 → 录制 → 提交 → 回收上一帧

```cpp
void VkCompositor::onScreenFrame(const QVideoFrame &screenFrame)
{
    Frame &fr = m_frames[m_curFrame];

    // 1. 等这个槽上一轮的 GPU 工作完成（通常早已完成，几乎不阻塞）
    m_df->vkWaitForFences(m_dev, 1, &fr.fence, VK_TRUE, UINT64_MAX);

    // 2. 先把这个槽“上一次”的回读结果取出来发出去（异步回读的落地点）
    if (fr.pending) recycleFrame(fr);
    m_df->vkResetFences(m_dev, 1, &fr.fence);

    // 3. 录制命令
    VkCommandBufferBeginInfo bi;
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    m_df->vkBeginCommandBuffer(fr.cmd, &bi);

    // 3a. 上传两路纹理（含 layout 转换，详见第 8 节 uploadTexture）
    bool hasScreen = (uploadTexture(m_texScreen, screenFrame, fr.cmd), true);
    bool hasCamera = m_overlay && m_lastCamera.isValid();
    if (hasCamera) uploadTexture(m_texCamera, m_lastCamera, fr.cmd);

    // 3b. renderpass 绘制
    recordDraw(fr.cmd, hasScreen, hasCamera);

    // 3c. 把离屏 color image（已是 TRANSFER_SRC）拷进回读 buffer
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { uint32_t(m_outputSize.width()),
                           uint32_t(m_outputSize.height()), 1 };
    m_df->vkCmdCopyImageToBuffer(fr.cmd, m_color,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fr.readback, 1, &region);

    m_df->vkEndCommandBuffer(fr.cmd);

    // 4. 提交（不等待，fence 之后在“下一轮回到这个槽”时才检查）
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &fr.cmd;
    VK_CHECK(m_df->vkQueueSubmit(m_queue, 1, &si, fr.fence));
    fr.pending = true;

    // 5. 轮到下一个槽
    m_curFrame = (m_curFrame + 1) % kFramesInFlight;
}

// 把已完成帧的回读内存包成 QImage 发出去
void VkCompositor::recycleFrame(Frame &fr)
{
    // mapped 指向 host-coherent 内存，渲染结果就在里面（RGBA8）
    QImage img(reinterpret_cast<const uchar*>(fr.mapped),
               m_outputSize.width(), m_outputSize.height(),
               m_outputSize.width() * 4, QImage::Format_RGBA8888);
    if (!img.isNull())
        emit frameReady(img.copy().convertToFormat(QImage::Format_ARGB32));
    fr.pending = false;
}
```

> 注意第 2 步的设计：回读**落后一整轮**（kFramesInFlight 帧）。提交第 N 帧后立即返回，
> 等下次轮回到同一个槽时，它的 GPU 工作早已完成，取结果几乎零等待——这就是相对
> QRhi 离屏模式的吞吐优势。代价是端到端延迟增加约 1 帧。

## 8. uploadTexture() — QVideoFrame → VkImage（含 layout 转换 + staging）

简化起见，纹理用每帧重建的 staging buffer 上传。生产环境应复用 staging。

```cpp
// 小工具：image layout barrier
static void imageBarrier(QVulkanDeviceFunctions *df, VkCommandBuffer cmd,
    VkImage img, VkImageLayout oldL, VkImageLayout newL,
    VkAccessFlags srcA, VkAccessFlags dstA,
    VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
{
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldL; b.newLayout = newL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1 };
    b.srcAccessMask = srcA; b.dstAccessMask = dstA;
    df->vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0,nullptr, 0,nullptr, 1,&b);
}

void VkCompositor::uploadTexture(Tex &t, const QVideoFrame &f, VkCommandBuffer cmd)
{
    QImage img = f.toImage();
    if (img.isNull()) return;
    img = img.convertToFormat(QImage::Format_RGBA8888);
    const uint32_t W = img.width(), H = img.height();

    // 尺寸变化或首帧：重建 image + view + descriptor
    if (t.image == VK_NULL_HANDLE || t.size != img.size()) {
        if (t.view)   m_df->vkDestroyImageView(m_dev, t.view, nullptr);
        if (t.image)  m_df->vkDestroyImage(m_dev, t.image, nullptr);
        if (t.memory) m_df->vkFreeMemory(m_dev, t.memory, nullptr);

        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = { W, H, 1 };
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(m_df->vkCreateImage(m_dev, &ici, nullptr, &t.image));

        VkMemoryRequirements req;
        m_df->vkGetImageMemoryRequirements(m_dev, t.image, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemType(req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(m_df->vkAllocateMemory(m_dev, &ai, nullptr, &t.memory));
        m_df->vkBindImageMemory(m_dev, t.image, t.memory, 0);

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = t.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1 };
        VK_CHECK(m_df->vkCreateImageView(m_dev, &vi, nullptr, &t.view));
        t.size = img.size();
    }

    // staging buffer（host-visible）→ 拷数据
    VkDeviceSize sz = VkDeviceSize(W) * H * 4;
    VkBuffer staging; VkDeviceMemory stagingMem;
    createBuffer(sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, stagingMem);
    void *p; m_df->vkMapMemory(m_dev, stagingMem, 0, sz, 0, &p);
    memcpy(p, img.constBits(), sz);
    m_df->vkUnmapMemory(m_dev, stagingMem);

    // UNDEFINED → TRANSFER_DST
    imageBarrier(m_df, cmd, t.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy rg{};
    rg.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0,0,1 };
    rg.imageExtent = { W, H, 1 };
    m_df->vkCmdCopyBufferToImage(cmd, staging, t.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);

    // TRANSFER_DST → SHADER_READ_ONLY
    imageBarrier(m_df, cmd, t.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // ⚠️ staging 在本帧 GPU 用完前不能销毁：实战应挂到该 frame 的延迟删除队列，
    //    这里为示意省略；可改用预分配的每帧 staging 复用。
}
```

## 9. recordDraw() — renderpass + 两次绘制（push constant 传矩形）

push constant 内存布局须与着色器 `Ubo` 一致：偏移 0 起 `vec4 uRect`(16B)，
偏移 16 起 `int uSwizzleBGRA`(4B)。

```cpp
// 每帧临时分配描述符 set，把某张纹理绑定上去
static VkDescriptorSet allocBindTex(QVulkanDeviceFunctions *df, VkDevice dev,
    VkDescriptorPool pool, VkDescriptorSetLayout layout,
    VkImageView view, VkSampler samp)
{
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
    VkDescriptorSet set;
    df->vkAllocateDescriptorSets(dev, &ai, &set);

    VkDescriptorImageInfo ii{ samp, view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    df->vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    return set;
}

void VkCompositor::recordDraw(VkCommandBuffer cmd, bool hasScreen, bool hasCamera)
{
    // 每帧重置描述符池（上一轮的 set 随之失效，与 frame fence 对齐保证安全）
    m_df->vkResetDescriptorPool(m_dev, m_descPool, 0);

    VkClearValue clear{}; clear.color = { {0.f,0.f,0.f,1.f} };
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = m_rp; rbi.framebuffer = m_fb;
    rbi.renderArea = { {0,0}, { uint32_t(m_outputSize.width()),
                                uint32_t(m_outputSize.height()) } };
    rbi.clearValueCount = 1; rbi.pClearValues = &clear;
    m_df->vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    m_df->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipe);
    VkDeviceSize off = 0;
    m_df->vkCmdBindVertexBuffers(cmd, 0, 1, &m_vbuf, &off);
    m_df->vkCmdBindIndexBuffer(cmd, m_ibuf, 0, VK_INDEX_TYPE_UINT16);

    struct PC { float rect[4]; int swizzle; } pc;

    auto draw = [&](VkImageView view, float x,float y,float w,float h,int sw){
        VkDescriptorSet set = allocBindTex(m_df, m_dev, m_descPool,
                                           m_setLayout, view, m_sampler);
        m_df->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_pipeLayout, 0, 1, &set, 0, nullptr);
        pc.rect[0]=x; pc.rect[1]=y; pc.rect[2]=w; pc.rect[3]=h; pc.swizzle=sw;
        m_df->vkCmdPushConstants(cmd, m_pipeLayout,
            VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(pc), &pc);
        m_df->vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
    };

    // 1) 桌面铺满 NDC
    if (hasScreen) draw(m_texScreen.view, -1,-1, 2,2, /*swizzle*/0);

    // 2) 相机右下角小窗（与 GL 版同算法；Vulkan Y 向下，按需调 py）
    if (hasCamera) {
        float W=m_outputSize.width(), H=m_outputSize.height();
        float w=m_camSize.width(), h=m_camSize.height();
        float px=W-w-m_margin, py=m_margin;
        float ndcX=px/W*2-1, ndcY=py/H*2-1, ndcW=w/W*2, ndcH=h/H*2;
        draw(m_texCamera.view, ndcX,ndcY, ndcW,ndcH, 0);
    }

    m_df->vkCmdEndRenderPass(cmd);
    // renderpass 的 finalLayout 已把 m_color 转成 TRANSFER_SRC，外层可直接 copy
}
```

## 10. cleanup() — 销毁全部 Vulkan 对象

必须先 `vkDeviceWaitIdle` 等 GPU 全部停下，再逆序销毁；
**在渲染线程里调用**（配合独立线程方案，用 `BlockingQueuedConnection`）。

```cpp
void VkCompositor::cleanup()
{
    if (m_dev == VK_NULL_HANDLE) return;
    m_df->vkDeviceWaitIdle(m_dev);

    auto destroyTex = [&](Tex &t){
        if (t.view)   m_df->vkDestroyImageView(m_dev, t.view, nullptr);
        if (t.image)  m_df->vkDestroyImage(m_dev, t.image, nullptr);
        if (t.memory) m_df->vkFreeMemory(m_dev, t.memory, nullptr);
        t = {};
    };
    destroyTex(m_texScreen);
    destroyTex(m_texCamera);

    for (auto &fr : m_frames) {
        if (fr.mapped)      m_df->vkUnmapMemory(m_dev, fr.readbackMem);
        if (fr.readback)    m_df->vkDestroyBuffer(m_dev, fr.readback, nullptr);
        if (fr.readbackMem) m_df->vkFreeMemory(m_dev, fr.readbackMem, nullptr);
        if (fr.fence)       m_df->vkDestroyFence(m_dev, fr.fence, nullptr);
    }

    m_df->vkDestroyBuffer(m_dev, m_vbuf, nullptr);
    m_df->vkFreeMemory(m_dev, m_vmem, nullptr);
    m_df->vkDestroyBuffer(m_dev, m_ibuf, nullptr);
    m_df->vkFreeMemory(m_dev, m_imem, nullptr);

    m_df->vkDestroySampler(m_dev, m_sampler, nullptr);
    m_df->vkDestroyDescriptorPool(m_dev, m_descPool, nullptr);
    m_df->vkDestroyPipeline(m_dev, m_pipe, nullptr);
    m_df->vkDestroyPipelineLayout(m_dev, m_pipeLayout, nullptr);
    m_df->vkDestroyDescriptorSetLayout(m_dev, m_setLayout, nullptr);

    m_df->vkDestroyFramebuffer(m_dev, m_fb, nullptr);
    m_df->vkDestroyRenderPass(m_dev, m_rp, nullptr);
    m_df->vkDestroyImageView(m_dev, m_colorView, nullptr);
    m_df->vkDestroyImage(m_dev, m_color, nullptr);
    m_df->vkFreeMemory(m_dev, m_colorMem, nullptr);

    m_df->vkDestroyCommandPool(m_dev, m_pool, nullptr);
    m_df->vkDestroyDevice(m_dev, nullptr);
    m_dev = VK_NULL_HANDLE;
    m_inst.destroy();
    qDebug() << "🧹 [Vulkan] 资源全部释放";
}
```

## 11. 接入 DesktopCapturer（与独立线程方案合并）

```cpp
// DesktopCapturer.h 顶部
#include "VkCompositor.h"
using GlCompositor = VkCompositor;   // 类型别名，其余代码不动

// 构造里（同独立线程方案）：
m_compositor = new VkCompositor();            // 无 parent
m_renderThread = new QThread(this);
m_compositor->moveToThread(m_renderThread);
connect(m_renderThread, &QThread::started, m_compositor, &VkCompositor::init);
m_renderThread->start();
// 析构：BlockingQueuedConnection 调 cleanup → quit → wait → delete
```

## 12. 已知简化点与生产环境改进

本文为「可读完整」做了取舍，落地前需补：

| 简化点 | 生产做法 |
|--------|---------|
| `uploadTexture` 每帧新建 staging 且未延迟销毁 | 每个 frame slot 预分配固定 staging，随 fence 回收；或用 `vkCmdUpdateBuffer` 小数据 |
| 描述符池每帧 reset+realloc | 预分配 per-frame 固定 set，仅 `vkUpdateDescriptorSets` 改绑定 |
| 单队列做图形+传输 | 拆出独立 transfer queue，回读 copy 不抢图形队列 |
| `f.toImage()` 仍走 CPU | 若 QVideoFrame 是 GPU 纹理(`QVideoFrameFormat` 硬件帧)，可用 Vulkan 互操作零拷贝导入，省掉回读再上传 |
| 错误处理用 `qFatal` | 改成可恢复的错误返回 + 日志 |
| 校验层仅 debug 开 | 上线关闭校验层；用 `vkconfig` 离线验证无 error |

## 13. 验证清单

1. 开 `VK_LAYER_KHRONOS_validation`，运行**全程无 validation error**。
2. 预览画面正确：桌面铺满、相机在右下角；**确认无上下颠倒**（否则调 Y 翻转）。
3. **确认无红蓝互换**（否则切 swizzle 或改回读 `Format_RGBA8888` ↔ 通道顺序）。
4. 推流端 FFmpegStreamer 收到的字节序与其 `bgra` 参数一致。
5. 连续运行 10 分钟，显存/内存不增长（无泄漏，验证 staging/descriptor 回收正确）。
6. 退出无崩溃、无 "device lost"、cleanup 日志正常打印。

## 14. 总结

裸 Vulkan（路线 B）相比 QRhi（路线 A）多写约 600 行，换来：
- **真正的异步回读流水线**（回读落后 N 帧，CPU 几乎零等待）；
- 独立 transfer queue、零拷贝硬件帧导入等高级优化空间。

但只有当 **QRhi 离屏 + 渲染独立线程仍达不到目标吞吐**时才值得。
绝大多数场景，前面两份文档（独立线程 / QRhi）已足够。
