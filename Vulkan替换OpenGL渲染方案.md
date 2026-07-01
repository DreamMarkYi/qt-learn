# 用 Vulkan 替换 OpenGL 渲染合成方案

> 目标：把 `GlCompositor`（OpenGL 3.3 离屏合成 + 回读）替换为 Vulkan 实现，
> 保持对外接口不变（两个 `QVideoSink` 进、`frameReady(QImage)` 出），
> 上层 `DesktopCapturer` / `FFmpegStreamer` / UI 完全不动。

## 0. 先说结论：值不值得换

当前 OpenGL 方案的真正瓶颈**不是绘制**，而是 `glFinish()` + `m_fbo->toImage()`
这一步**同步 GPU→CPU 回读**（GlCompositor.cpp:140-143）。换 Vulkan 不会让回读
本身变快——它是 PCIe 带宽和"GPU 画完才能读"的物理限制决定的。

Vulkan 真正能带来的收益是：

| 收益点 | 说明 |
|--------|------|
| 异步回读流水线 | 用多个 frame-in-flight + fence，让"GPU 画第 N+1 帧"与"CPU 拷第 N 帧"重叠，吞吐翻倍 |
| 显式控制内存 | host-visible 的 readback buffer 可持久映射(persistent mapping)，省一次拷贝 |
| 多队列 | 图形队列与传输队列分离，回读走专用 transfer queue 不抢绘制 |
| 与推流解耦 | 配合前面的"渲染独立线程"方案，可做成无锁三缓冲 |

代价：Vulkan 代码量是 OpenGL 的 3~5 倍，所有同步要手写。
**建议路径**：先做"渲染独立线程"(已有文档)解决卡顿，若仍需更高吞吐再上 Vulkan。

## 1. 接口契约（保持不变）

替换后的新类 `VkCompositor` 必须提供与 `GlCompositor` 完全一致的对外 API，
这样 `DesktopCapturer.h` 一行不用改（只改 `#include` 和类名 typedef）：

```cpp
class VkCompositor : public QObject {
    Q_OBJECT
public:
    QVideoSink* screenSink() const;
    QVideoSink* cameraSink() const;
    QSize       outputSize() const;
    void        setOverlay(bool on);
    bool        overlay() const;
signals:
    void frameReady(const QImage &frame);   // 出口契约不变
};
```

最省事的接法：在 `DesktopCapturer.h` 顶部
```cpp
#include "VkCompositor.h"
using GlCompositor = VkCompositor;   // 名字别名，其余代码不动
```

## 2. 技术选型：三条路线

Qt 里用 Vulkan 做离屏合成有三种做法，按"工作量 / 可控性"排序：

### 路线 A — Qt RHI（推荐，工作量最小）
Qt 6 内部的渲染硬件接口 `QRhi`，写一份代码可后端切换
（Vulkan / D3D11 / Metal / OpenGL）。离屏渲染用 `QRhiTextureRenderTarget`，
回读用 `QRhiReadbackResult`，天然支持 frame-in-flight。

- 优点：API 比裸 Vulkan 简洁一个数量级；跨平台；Qt 已封装同步细节。
- 缺点：`QRhi` 在 Qt 6.x 是「半公开」API（头文件在 `<rhi/qrhi.h>`，
  接口可能随版本微调）；需要链接 `Qt6::GuiPrivate`。
- **结论：本方案主推路线 A**，下文代码以 QRhi 为主。

### 路线 B — 裸 Vulkan + QVulkanInstance
用 `QVulkanInstance` 创建实例，自己管 device / queue / command buffer /
pipeline / 同步。Qt 只帮你建 instance 和（如需）窗口表面。

- 优点：完全可控，能用 transfer queue、timeline semaphore 等高级特性。
- 缺点：代码量巨大（建管线、描述符、内存分配都要手写），易错。
- 适用：路线 A 的吞吐仍不够，需要榨干硬件时。

### 路线 C — 外部库（Vulkan-Hpp / VMA）
裸 Vulkan + `vulkan.hpp`(C++ 封装) + VMA(显存分配器)。
脱离 Qt 的 Vulkan 封装，最灵活但与 Qt 集成最麻烦，本方案不展开。

## 3. 依赖与构建改动（CMakeLists.txt）

### 路线 A（QRhi）所需：
```cmake
find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets Gui)

target_link_libraries(qt-learn PRIVATE
    Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets
    Qt6::GuiPrivate                 # ← QRhi 在 GuiPrivate 里
)
```
不再需要 `Qt6::OpenGL` 和 `opengl32`（除非保留 GL 后备）。
QRhi 选 Vulkan 后端时，运行期需要系统装有 Vulkan loader（`vulkan-1.dll`，
Windows 上一般随显卡驱动；CI/无 GPU 环境需 SwiftShader 软件实现）。

### 路线 B（裸 Vulkan）额外需要：
```cmake
find_package(Vulkan REQUIRED)        # 需安装 Vulkan SDK (LunarG)
target_link_libraries(qt-learn PRIVATE Vulkan::Vulkan)
```
并设置环境 `VULKAN_SDK` 指向 SDK；着色器要用 `glslangValidator` 预编译成 SPIR-V。

## 4. 着色器移植（GLSL 330 → Vulkan GLSL → SPIR-V）

Vulkan 不吃 GLSL 源码，只吃 **SPIR-V 字节码**。现有两个着色器要改写并预编译。
主要差异：① uniform 必须放进 **uniform block + descriptor set/binding**；
② 顶点输入用 `location`；③ Vulkan 的 NDC **Y 轴向下、深度 [0,1]**（与 GL 相反）。

### 现有 GL 着色器（参考）
```glsl
// composite.vert (330)：vUv 翻转 V，aPos 映射到 uRect 矩形
// composite.frag (330)：采样 uTex，按 uSwizzleBGRA 决定是否交换 R/B
```

### Vulkan 版顶点着色器 `composite.vert`
```glsl
#version 450
layout(location = 0) in vec2 aPos;          // 单位四边形 [0,1]^2
layout(location = 0) out vec2 vUv;

layout(std140, binding = 0) uniform Ubo {
    vec4 uRect;        // x,y,w,h (NDC)
    int  uSwizzleBGRA; // 传给 frag 用
} ubo;

void main() {
    // Vulkan NDC 的 Y 向下；纹理原点在左上。
    // GL 版做了 1.0 - aPos.y 翻 V，Vulkan 下通常不需要翻（取决于上传方向），
    // 这里先保持采样坐标直取，若上下颠倒再调此处或上传时翻。
    vUv = aPos;
    vec2 ndc = ubo.uRect.xy + aPos * ubo.uRect.zw;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
```

### Vulkan 版片元着色器 `composite.frag`
```glsl
#version 450
layout(location = 0) in  vec2 vUv;
layout(location = 0) out vec4 FragColor;

layout(binding = 1) uniform sampler2D uTex;
layout(std140, binding = 0) uniform Ubo {
    vec4 uRect;
    int  uSwizzleBGRA;
} ubo;

void main() {
    vec4 c = texture(uTex, vUv);
    FragColor = (ubo.uSwizzleBGRA == 1) ? c.bgra : c;
}
```

### 预编译为 SPIR-V（构建步骤）
```bash
glslangValidator -V composite.vert -o composite.vert.spv
glslangValidator -V composite.frag -o composite.frag.spv
```
把 `.spv` 放进 `qrc` 资源（替换原来的 `.vert/.frag`），运行期读字节码建 shader module。

> 用 QRhi（路线 A）时可省心：QRhi 自带 `QShader` + `qsb` 工具，
> 用 `.qsb` 容器一次打包多后端字节码，运行期自动选。命令：
> `qsb --glsl 450 --spirv composite.vert -o composite.vert.qsb`

## 5. 路线 A 实现骨架（QRhi + Vulkan 后端）

### 5.1 VkCompositor.h（结构）
```cpp
#ifndef VKCOMPOSITOR_H
#define VKCOMPOSITOR_H

#include <QObject>
#include <QVideoSink>
#include <QVideoFrame>
#include <QImage>
#include <QSize>
#include <rhi/qrhi.h>          // 半公开 API，需 Qt6::GuiPrivate
#include <QVulkanInstance>
#include <memory>

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
    void init();       // 在渲染线程里建 QRhi（沿用独立线程方案）
    void cleanup();

signals:
    void frameReady(const QImage &frame);

private:
    void onScreenFrame(const QVideoFrame &f);
    bool uploadFrame(std::unique_ptr<QRhiTexture> &tex, QSize &cached,
                     const QVideoFrame &f, QRhiResourceUpdateBatch *u);
    void buildPipeline();
    void renderAndReadback();

    // ---- Qt 媒体侧 ----
    QVideoSink* m_screenSink = nullptr;
    QVideoSink* m_cameraSink = nullptr;
    QVideoFrame m_lastCamera;

    // ---- Vulkan / RHI ----
    QVulkanInstance m_vkInstance;
    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<QRhiTexture>            m_fboTex;     // 离屏目标
    std::unique_ptr<QRhiTextureRenderTarget> m_rt;
    std::unique_ptr<QRhiRenderPassDescriptor> m_rp;
    std::unique_ptr<QRhiBuffer>  m_vbuf, m_ibuf, m_ubuf;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline>  m_pipeline;
    std::unique_ptr<QRhiTexture> m_texScreen, m_texCamera;
    QSize m_texScreenSize, m_texCameraSize;

    bool        m_overlay = true;
    const QSize m_outputSize{1920, 1080};
    const QSize m_camSize{480, 360};
    const int   m_margin = 20;
};
#endif
```

### 5.2 init()：创建 Vulkan 后端的 QRhi
```cpp
void VkCompositor::init()
{
    // 1. Vulkan 实例（QRhi 的 Vulkan 后端要求先有 QVulkanInstance）
    m_vkInstance.setApiVersion(QVersionNumber(1, 1));
#ifndef QT_NO_DEBUG
    m_vkInstance.setLayers({ "VK_LAYER_KHRONOS_validation" }); // 校验层(调试)
#endif
    if (!m_vkInstance.create())
        qFatal("无法创建 QVulkanInstance: %d", m_vkInstance.errorCode());

    // 2. 建 QRhi（Vulkan 后端，离屏用，无需 swapchain/window）
    QRhiVulkanInitParams params;
    params.inst = &m_vkInstance;
    m_rhi.reset(QRhi::create(QRhi::Vulkan, &params));
    if (!m_rhi)
        qFatal("无法创建 Vulkan 后端的 QRhi");

    qDebug() << "QRhi 后端:" << m_rhi->backendName()
             << " 设备:" << m_rhi->driverInfo().deviceName;

    buildPipeline();
}
```

### 5.3 buildPipeline()：离屏目标 + 管线 + 资源绑定
```cpp
void VkCompositor::buildPipeline()
{
    // ---- 离屏渲染目标：一张颜色纹理当 FBO ----
    m_fboTex.reset(m_rhi->newTexture(QRhiTexture::RGBA8, m_outputSize, 1,
                   QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    m_fboTex->create();

    QRhiTextureRenderTargetDescription rtDesc({ m_fboTex.get() });
    m_rt.reset(m_rhi->newTextureRenderTarget(rtDesc));
    m_rp.reset(m_rt->newCompatibleRenderPassDescriptor());
    m_rt->setRenderPassDescriptor(m_rp.get());
    m_rt->create();

    // ---- 顶点/索引缓冲：单位四边形（与 GL 版同样的 4 点 6 索引）----
    static const float quad[] = { 0,0, 1,0, 1,1, 0,1 };
    static const quint16 idx[] = { 0,1,2, 0,2,3 };
    m_vbuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable,
                 QRhiBuffer::VertexBuffer, sizeof(quad))); m_vbuf->create();
    m_ibuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable,
                 QRhiBuffer::IndexBuffer, sizeof(idx)));  m_ibuf->create();

    // ---- uniform buffer：uRect(vec4) + uSwizzleBGRA(int)，按 std140 对齐 ----
    m_ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic,
                 QRhiBuffer::UniformBuffer, 32)); m_ubuf->create();

    // ---- 采样器 ----
    m_sampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                    QRhiSampler::None, QRhiSampler::ClampToEdge,
                    QRhiSampler::ClampToEdge)); m_sampler->create();

    // ---- 加载 .qsb 着色器 ----
    auto loadShader = [](const QString &path){
        QFile f(path); f.open(QIODevice::ReadOnly);
        return QShader::fromSerialized(f.readAll());
    };
    QShader vs = loadShader(":/Shader/composite.vert.qsb");
    QShader fs = loadShader(":/Shader/composite.frag.qsb");

    // ---- 资源绑定（set=0）：b0=UBO(顶点+片元), b1=sampler(片元) ----
    m_srb.reset(m_rhi->newShaderResourceBindings());
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0,
            QRhiShaderResourceBinding::VertexStage |
            QRhiShaderResourceBinding::FragmentStage, m_ubuf.get()),
        QRhiShaderResourceBinding::sampledTexture(1,
            QRhiShaderResourceBinding::FragmentStage,
            m_texScreen ? m_texScreen.get() : m_fboTex.get(), m_sampler.get()),
    });
    m_srb->create();

    // ---- 图形管线 ----
    m_pipeline.reset(m_rhi->newGraphicsPipeline());
    m_pipeline->setShaderStages({ {QRhiShaderStage::Vertex, vs},
                                  {QRhiShaderStage::Fragment, fs} });
    QRhiVertexInputLayout layout;
    layout.setBindings({ { 2 * sizeof(float) } });
    layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float2, 0 } });
    m_pipeline->setVertexInputLayout(layout);
    m_pipeline->setShaderResourceBindings(m_srb.get());
    m_pipeline->setRenderPassDescriptor(m_rp.get());
    // 画中画需要混合时开启 alpha blend
    QRhiGraphicsPipeline::TargetBlend blend; blend.enable = true;
    m_pipeline->setTargetBlends({ blend });
    m_pipeline->create();

    // 上传静态顶点/索引（一次）
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    u->uploadStaticBuffer(m_vbuf.get(), quad);
    u->uploadStaticBuffer(m_ibuf.get(), idx);
    // 注意：此 batch 需在下次 beginPass 时提交
}
```

### 5.4 每帧：上传纹理 → 离屏绘制 → 异步回读（替换 glFinish+toImage）
```cpp
void VkCompositor::onScreenFrame(const QVideoFrame &screenFrame)
{
    // QRhi 离屏渲染：用 beginOffscreenFrame / endOffscreenFrame
    QRhiCommandBuffer *cb = nullptr;
    if (m_rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) return;

    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();

    // 1. 上传两路纹理（首帧创建，之后只更新像素）
    bool hasScreen = uploadFrame(m_texScreen, m_texScreenSize, screenFrame, u);
    bool hasCamera = (m_overlay && m_lastCamera.isValid())
                   && uploadFrame(m_texCamera, m_texCameraSize, m_lastCamera, u);

    // 2. 开 render pass，清屏为黑
    const QColor clear(0, 0, 0, 255);
    cb->beginPass(m_rt.get(), clear, { 1.0f, 0 }, u);
    cb->setGraphicsPipeline(m_pipeline.get());
    cb->setViewport({ 0, 0, float(m_outputSize.width()),
                            float(m_outputSize.height()) });

    // 3. 桌面铺满（uRect 覆盖整个 NDC：-1,-1,2,2）
    if (hasScreen) drawQuadRhi(cb, m_texScreen.get(), -1,-1, 2,2, /*swizzle*/0);

    // 4. 相机小窗右下角（像素→NDC，同 GL 版算法）
    if (hasCamera) {
        float W=m_outputSize.width(), H=m_outputSize.height();
        float w=m_camSize.width(), h=m_camSize.height();
        float px=W-w-m_margin, py=m_margin;     // Vulkan Y 向下，按需调 py
        float ndcX=px/W*2-1, ndcY=py/H*2-1, ndcW=w/W*2, ndcH=h/H*2;
        drawQuadRhi(cb, m_texCamera.get(), ndcX,ndcY, ndcW,ndcH, 0);
    }
    cb->endPass();

    // 5. ⭐ 异步回读：登记一个 readback，GPU 完成后回调里拿到像素
    QRhiReadbackResult rb;
    rb.completed = [this, &rb]() {
        // pixelSize / format / data 已填好（BGRA 或 RGBA 视后端而定）
        QImage img(reinterpret_cast<const uchar*>(rb.data.constData()),
                   rb.pixelSize.width(), rb.pixelSize.height(),
                   QImage::Format_RGBA8888);
        if (!img.isNull())
            emit frameReady(img.copy().convertToFormat(QImage::Format_ARGB32));
    };
    QRhiResourceUpdateBatch *ru = m_rhi->nextResourceUpdateBatch();
    ru->readBackTexture(QRhiReadbackDescription(m_fboTex.get()), &rb);

    // 6. 结束帧。endOffscreenFrame 会等 GPU 完成并触发上面的 completed 回调
    m_rhi->endOffscreenFrame();
}
```

> 关于 `drawQuadRhi`：每次绘制前更新 UBO（uRect + uSwizzle），重建/切换 srb 的
> 纹理绑定，再 `cb->setShaderResources(); setVertexInput(); drawIndexed(6)`。
> 由于 QRhi 一个 pass 内切换纹理需要不同 srb，实战中常给桌面、相机各建一个 srb，
> 或用 dynamic offset。细节略，按 Qt RHI 示例 `offscreen` / `texuploads` 套用。

### 5.5 同步 vs 异步回读的关键区别
- **GL 旧版**：`glFinish()` 死等 GPU + `toImage()` 阻塞拷贝，全程卡调用线程。
- **QRhi offscreen**：`endOffscreenFrame()` 仍是同步点，但配合**渲染独立线程**
  （见另一份文档），这个等待发生在工作线程，不卡 UI。
- **真正的异步流水线**需要路线 B 手写 N 帧 in-flight + fence 轮询，
  让 readback buffer 落后 1~2 帧，CPU 永不空等。QRhi 离屏模式做不到完全异步，
  这是 QRhi 的取舍。若吞吐是硬指标，才值得上裸 Vulkan。

### 5.6 uploadFrame()：QVideoFrame → QRhiTexture
```cpp
bool VkCompositor::uploadFrame(std::unique_ptr<QRhiTexture> &tex, QSize &cached,
                               const QVideoFrame &f, QRhiResourceUpdateBatch *u)
{
    QImage img = f.toImage();                 // 必要时含 GPU 回读
    if (img.isNull()) return false;
    img = img.convertToFormat(QImage::Format_RGBA8888);

    if (!tex || img.size() != cached) {       // 首帧或尺寸变化：重建纹理
        tex.reset(m_rhi->newTexture(QRhiTexture::RGBA8, img.size()));
        tex->create();
        cached = img.size();
    }
    // 上传整张（QRhi 内部处理行对齐，无需手动逐行）
    QRhiTextureSubresourceUploadDescription sub(img.constBits(),
                                                img.sizeInBytes());
    QRhiTextureUploadDescription desc({ 0, 0, sub });
    u->uploadTexture(tex.get(), desc);
    return true;
}
```
注意：纹理重建后，引用它的 `m_srb` 需要 `m_srb->create()` 重新生成绑定。

## 6. 迁移步骤（建议顺序）

1. **先做"渲染独立线程"改造**（另一份文档），把 GL 移出主线程——
   这是低成本高收益的第一步，多数卡顿问题到此即可解决。
2. 引入 QRhi：CMakeLists 加 `Qt6::GuiPrivate`，用 `qsb` 把着色器编成 `.qsb`，
   更新 `shader.qrc`。
3. 新建 `VkCompositor.h/.cpp`，按第 5 节实现，**先跑通桌面单层**（不画相机）。
4. 在 `DesktopCapturer.h` 用 `using GlCompositor = VkCompositor;` 切换，
   验证预览与推流链路不变。
5. 加回画中画（相机层 + alpha blend），调 Y 翻转/坐标。
6. 压测吞吐；若仍不满足，再评估路线 B（裸 Vulkan 异步流水线）。

## 7. 风险与注意事项

| 风险 | 说明 / 对策 |
|------|------|
| QRhi 是半公开 API | `<rhi/qrhi.h>` 接口可能随 Qt 小版本变；锁定 Qt 6.11.x，升级时回归测试 |
| 运行期需 Vulkan loader | Windows 随显卡驱动；旧机器/虚拟机/CI 需装驱动或用 SwiftShader 软渲染 |
| Y 轴翻转 | Vulkan NDC Y 向下、纹理原点左上，与 GL 相反；回读图像可能上下颠倒，在顶点/上传/`mirrored()` 三处之一统一处理 |
| 颜色通道顺序 | 回读格式可能是 BGRA；推流端（FFmpegStreamer）当前按 BGRA 配置 ffmpeg，需对齐避免红蓝互换 |
| 回读仍是带宽瓶颈 | 换 Vulkan 不改变 PCIe 回读速度；真正提速靠"异步重叠"，QRhi 离屏模式有限 |
| CI 无法跑 | 与 GL 版一样，需 GPU/Vulkan 环境，编译可进 CI、运行不行 |

## 8. 一句话总结

- **想解决卡顿** → 先做"渲染独立线程"，不必换 Vulkan。
- **想要可控的现代渲染、跨后端、且不想写裸 Vulkan** → 用 **QRhi（路线 A）**，
  本文第 5 节即可直接落地。
- **想榨干吞吐、做真正异步回读流水线** → 才上**裸 Vulkan（路线 B）**，
  代价是 3~5 倍代码量和全手写同步。
