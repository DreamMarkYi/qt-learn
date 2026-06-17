# Live2D 模型加载类设计（Live2DModel）

> 目标：新增一个 `Live2DModel` 类，负责 **读取并解析一个 Live2D 模型目录**（`*.model3.json` 及其引用的 `moc3` / 贴图 / 动作 / 物理 / 表情等），把它加载进 Cubism Core，并用 Cubism Framework 的 OpenGL 渲染器把模型画到当前的 GL 上下文 / FBO 里。
>
> 工程已在 `CMakeLists.txt` 里配好了 Cubism Core（预编译 lib/dll）、Framework（源码编译）、GLEW、stb_image。本类是把这些 SDK 拼起来、对接现有 `GlCompositor` 渲染线程的 **胶水层**。

---

## 1. 背景与原理

### 1.1 一个 Live2D 模型在磁盘上是什么

一个模型目录（以 Cubism 官方示例 Hiyori 为例）大致长这样：

```
Hiyori/
├── Hiyori.model3.json      # 入口清单：声明下面所有文件的相对路径
├── Hiyori.moc3             # 二进制网格 + 变形数据（核心）
├── Hiyori.physics3.json    # 物理摆动（头发/裙摆等）
├── Hiyori.pose3.json       # 部件显隐分组（可选）
├── Hiyori.cdi3.json        # 参数显示名（编辑器用，可选）
├── Hiyori.2048/            # 贴图图集
│   └── texture_00.png
├── motions/                # 动作 *.motion3.json，按 group 组织
│   ├── Idle.motion3.json
│   └── ...
└── expressions/            # 表情 *.exp3.json（可选）
```

`*.model3.json` 是**唯一入口**：它本身不含几何数据，只是一份清单，用相对路径告诉你 moc3、贴图、动作分别在哪。所以加载流程永远是「先读 model3.json → 再按里面的路径去读其它文件」。

### 1.2 Cubism SDK 的三层结构

| 层 | 来源 | 职责 | 本类如何用 |
|----|------|------|-----------|
| **Core** | 预编译 `Live2DCubismCore`（C 库） | 解析 `moc3`、持有参数/部件、计算顶点 | 不直接调，被 Framework 包着 |
| **Framework** | 源码 `Framework` 目标 | C++ 封装：`CubismUserModel`、`CubismModelSettingJson`、`CubismRenderer_OpenGLES2`、动作/物理/表情管理器 | **继承 `CubismUserModel`** |
| **胶水层** | 我们写的 `Live2DModel` | 文件读取、纹理上传(stb+GL)、生命周期、对接 Qt 线程 | 本文档 |

关键认识：**Framework 不碰文件系统、不碰图片解码**。它只接受「内存里的字节缓冲」。所以胶水层要负责：

1. 把 `moc3` / `motion3.json` / `physics3.json` 等读成 `csmByte*` 缓冲喂给 Framework；
2. 用 stb_image 解码 PNG，再用 GL 建纹理，调 `renderer->BindTexture(i, texId)` 绑上去。

### 1.3 渲染原理（为什么能塞进现有 GlCompositor）

Cubism 的 OpenGL 渲染器 `CubismRenderer_OpenGLES2` 干的事：

- 每帧 `model->Update()` 让 Core 重算顶点；
- `renderer->SetMvpMatrix(&mvp)` 设投影；
- `renderer->DrawModel()` 用模型自带 shader 把所有 ArtMesh 画到**当前绑定的 framebuffer**。

它不创建上下文、不开窗口，只往「当前 current 的 GL 上下文 + 当前 FBO」里画。这正好和 `GlCompositor` 的离屏渲染模型一致——`GlCompositor::onScreenFrame` 里 `m_fbo->bind()` 之后、`glFinish()` 之前，插一句 `m_live2d->draw(mvp)`，Live2D 就叠加到合成画面上了。

> ⚠️ 线程：所有 `Live2DModel` 的 GL 相关调用（`loadAssets` 里的建纹理、`draw`、`release`）**必须在 GlCompositor 的渲染线程、且上下文 current 时**执行。构造/纯数据解析可以在任意线程，但保险起见统一在渲染线程做。

---

## 2. 类设计

### 2.1 职责划分

```
┌─────────────────────────────────────────────────────────┐
│ CubismFramework (SDK 全局静态)                            │
│   StartUp(allocator) / Initialize() —— 整个进程一次       │
└─────────────────────────────────────────────────────────┘
            ▲ 依赖
┌───────────────────────┐     ┌──────────────────────────┐
│ Live2DAllocator       │     │ Live2DModel               │
│  : ICubismAllocator   │     │  : CubismUserModel        │
│  实现 malloc/free      │     │  - loadAssets(dir, name)  │
│  (Framework 启动时传入)│     │  - update(deltaSec)       │
└───────────────────────┘     │  - draw(CubismMatrix44&)  │
                              │  - release()              │
                              │  - startMotion / setExpr  │
                              └──────────────────────────┘
```

- **`Live2DAllocator`**：实现 `ICubismAllocator`（4 个虚函数），Framework 启动时必须传一个。最简实现就是转发到 `malloc/free` + 对齐分配。
- **`Live2DModel`**：继承 `CubismUserModel`（它已自带 `_model`、`_modelMatrix`、各种 Manager、`CreateRenderer/GetRenderer`），我们只补「读文件 + 建纹理 + 每帧更新/绘制」。

### 2.2 公开接口一览

| 方法 | 线程 | 说明 |
|------|------|------|
| `static bool initFramework()` | 任意，一次 | 启动 Cubism Framework 全局 |
| `static void disposeFramework()` | 任意，退出前 | 释放全局 |
| `bool loadAssets(dir, fileName)` | 渲染线程(GL current) | 读 model3.json 并加载全部资源、建纹理 |
| `void update(float dt)` | 渲染线程 | 推进动作/物理/呼吸/眨眼，重算顶点 |
| `void draw(CubismMatrix44 mvp)` | 渲染线程(GL current) | 把模型画到当前 FBO |
| `void release()` | 渲染线程(GL current) | 删纹理、删渲染器 |
| `CubismMotionQueueEntryHandle startMotion(group, no, priority)` | 渲染线程 | 播放动作 |
| `void setExpression(const csmChar* id)` | 渲染线程 | 切换表情 |

> 下文给出 **完整可编译代码**：`Live2DAllocator.hpp`、`Live2DModel.hpp`、`Live2DModel.cpp`，以及与 `GlCompositor` / `CMakeLists.txt` 的对接片段。

---

## 3. 完整代码

### 3.1 `Live2DAllocator.hpp` —— Framework 内存分配器

Framework 不直接 `new`，所有内存都走你传入的分配器。这里用**分级 free-list 内存池**实现：小块按 size class 切分回收复用，大块与超对齐请求回退到 `malloc`。

```cpp
#ifndef LIVE2DALLOCATOR_HPP
#define LIVE2DALLOCATOR_HPP

#include <ICubismAllocator.hpp>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

// 基于内存池的 Framework 分配器。
//
// 为什么用「分级 free-list 池」而不是「定长块池」：
// Cubism 的分配几乎全集中在加载期（解析 json / moc3 / 建顶点缓冲），运行时
// 每帧基本不分配；且请求大小跨度大、释放顺序任意。定长池套不进来，arena
// （只整体回收）又会在反复加载/卸载模型时无限增长。所以折中用分级 free-list：
//   - 小块(<= kMaxPooled)按 size class 切分、回收复用，省掉反复向 CRT 申请；
//   - 大块、或对齐要求超过 kAlign 的请求，直接走（对齐）malloc；
//   - 每个返回指针前藏 16 字节 Header，记录它从哪条路径来，释放时照此归位。
//
// 线程：加了一把 mutex 保证任意线程安全。若确认只在渲染线程分配，可去掉。
class Live2DAllocator : public Live2D::Cubism::Framework::ICubismAllocator
{
public:
    Live2DAllocator() = default;
    ~Live2DAllocator() override
    {
        // 进程退出时归还所有 slab；大块由 Framework Dispose 时各自 free。
        for (void* slab : _slabs) std::free(slab);
    }

    void* Allocate(const Live2D::Cubism::Framework::csmSizeType size) override
    {
        return allocate(size, kAlign);          // 统一按 kAlign 对齐，过度对齐无害
    }
    void Deallocate(void* memory) override { deallocate(memory); }

    void* AllocateAligned(const Live2D::Cubism::Framework::csmSizeType size,
                          const Live2D::Cubism::Framework::csmUint32 alignment) override
    {
        return allocate(size, alignment < kAlign ? kAlign : alignment);
    }
    void DeallocateAligned(void* alignedMemory) override { deallocate(alignedMemory); }

private:
    static constexpr std::size_t kAlign     = 16;        // 基础对齐（够 SIMD 顶点用）
    static constexpr std::size_t kHeader    = 16;        // Header 占位，且是 kAlign 倍数
    static constexpr std::size_t kSlabSize  = 64 * 1024; // 每次向 CRT 批发的大块
    static constexpr std::size_t kClasses[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
    static constexpr std::size_t kClassCount = sizeof(kClasses) / sizeof(kClasses[0]);
    static constexpr std::size_t kMaxPooled = 4096;

    enum Kind : std::uint32_t { POOL = 0, MALLOC = 1 };
    struct Header {                 // 藏在每个返回指针前 16 字节里
        std::uint32_t kind;
        std::uint32_t cls;          // POOL: size class 下标
        void*         origin;       // POOL: 块首；MALLOC: malloc 原始指针
    };

    static int classFor(std::size_t need)   // need 向上取到最近一档，超档返回 -1
    {
        for (int i = 0; i < (int)kClassCount; ++i)
            if (need <= kClasses[i]) return i;
        return -1;
    }

    void* allocate(std::size_t size, std::size_t alignment)
    {
        std::lock_guard<std::mutex> lk(_mtx);

        // —— 小块且只需基础对齐：走池 ——
        if (alignment <= kAlign) {
            int cls = classFor(size + kHeader);
            if (cls >= 0) {
                void* block = popOrCarve(cls);          // block 必为 16 对齐
                if (!block) return nullptr;
                Header* h = static_cast<Header*>(block);
                h->kind = POOL; h->cls = (std::uint32_t)cls; h->origin = block;
                return static_cast<std::uint8_t*>(block) + kHeader;
            }
        }

        // —— 大块 / 超对齐：对齐 malloc，Header 紧贴用户指针之前 ——
        std::size_t raw = size + alignment + kHeader;
        void* p = std::malloc(raw);
        if (!p) return nullptr;
        std::uintptr_t base    = reinterpret_cast<std::uintptr_t>(p) + kHeader;
        std::uintptr_t aligned = (base + alignment - 1) & ~(std::uintptr_t)(alignment - 1);
        Header* h = reinterpret_cast<Header*>(aligned - kHeader);
        h->kind = MALLOC; h->origin = p;
        return reinterpret_cast<void*>(aligned);
    }

    void deallocate(void* memory)
    {
        if (!memory) return;
        std::lock_guard<std::mutex> lk(_mtx);
        Header* h = reinterpret_cast<Header*>(
            static_cast<std::uint8_t*>(memory) - kHeader);
        if (h->kind == POOL) pushFree(h->cls, h->origin);  // 回收进对应 free list
        else                 std::free(h->origin);         // 还原始指针给 CRT
    }

    // free list 为侵入式单链：空闲块头 8 字节存 next 指针。
    void* popOrCarve(int cls)
    {
        if (!_free[cls]) carveSlab(cls);                   // 没了就开新 slab 切分
        void* block = _free[cls];
        if (!block) return nullptr;                        // OOM
        std::memcpy(&_free[cls], block, sizeof(void*));
        return block;
    }

    void carveSlab(int cls)
    {
        std::size_t blk = kClasses[cls];
        void* slab = std::malloc(kSlabSize);
        if (!slab) return;
        _slabs.push_back(slab);
        // slab 首地址向上对齐到 kAlign，保证切出的每块都 16 对齐
        std::uintptr_t p   = reinterpret_cast<std::uintptr_t>(slab);
        std::uintptr_t cur = (p + kAlign - 1) & ~(std::uintptr_t)(kAlign - 1);
        std::uintptr_t end = p + kSlabSize;
        while (cur + blk <= end) {                         // 逐块串进 free list
            std::memcpy(reinterpret_cast<void*>(cur), &_free[cls], sizeof(void*));
            _free[cls] = reinterpret_cast<void*>(cur);
            cur += blk;
        }
    }

    void pushFree(std::uint32_t cls, void* block)
    {
        std::memcpy(block, &_free[cls], sizeof(void*));
        _free[cls] = block;
    }

    std::mutex         _mtx;
    void*              _free[kClassCount] = {};
    std::vector<void*> _slabs;
};

#endif // LIVE2DALLOCATOR_HPP
```

**原理**：

- **分级回收**：把「用户大小 + 16 字节 Header」向上取到最近的 size class（32…4096），同档的释放块串成侵入式 free list 复用。第一次某档没货时，向 CRT 批发一整块 64KB `slab` 切分填满，之后的分配/释放就在 free list 上进出，不再碰 `malloc`。
- **Header 定位**：每个返回给 Framework 的指针前 16 字节藏一个 `Header`，记录它来自池（哪一档）还是 `malloc`（原始指针）。`Deallocate` 时直接 `ptr - 16` 取回，按来路归位——这样池块和大块能走同一个释放入口。
- **对齐**：slab 首地址对齐到 16、块大小是 16 的倍数，故池块天然 16 对齐，覆盖 Core 放 SIMD 顶点缓冲的常见需求。若请求对齐超过 16，回退到「多分配 `alignment-1` 字节、向上取整」的对齐 `malloc`，仍把 Header 塞在对齐指针之前。
- **回收时机**：池块释放只是回 free list（不还给系统），slab 直到分配器析构（进程退出）才整体 `free`；大块每次 `Deallocate` 即时还给 CRT。所以池的常驻内存约等于「加载期小块用量的峰值」，反复加载/卸载不会无限涨。

### 3.2 `Live2DModel.hpp`

```cpp
#ifndef LIVE2DMODEL_HPP
#define LIVE2DMODEL_HPP

#include <CubismFramework.hpp>
#include <Model/CubismUserModel.hpp>
#include <ICubismModelSetting.hpp>
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>
#include <Type/csmVector.hpp>
#include <string>

// 用 Live2D Cubism Framework 加载并渲染一个 model3.json 模型。
// 继承 CubismUserModel：父类已持有 _model / _modelMatrix / 各 Manager，
// 我们补足「读文件 + 建纹理 + 每帧更新/绘制」这一层胶水。
//
// 线程约定：loadAssets / update / draw / release 必须在持有 GL 上下文
// 的渲染线程中调用（与 GlCompositor 同线程、上下文 current）。
class Live2DModel : public Live2D::Cubism::Framework::CubismUserModel
{
public:
    Live2DModel();
    ~Live2DModel() override;

    // 进程级：启动 / 释放 Framework 全局。只需各调一次。
    static bool initFramework();
    static void disposeFramework();

    // 读取 <dir>/<fileName>（fileName 形如 "Hiyori.model3.json"），
    // 加载 moc3 / 物理 / 姿态 / 表情 / 动作，并把贴图上传成 GL 纹理。
    // 必须在 GL 上下文 current 时调用。成功返回 true。
    bool loadAssets(const std::string& dir, const std::string& fileName);

    // 推进时间：动作 → 表情 → 呼吸 → 眨眼 → 物理 → 姿态，最后 _model->Update()。
    void update(float deltaSeconds);

    // 用给定的 MVP 矩阵把模型绘制到当前绑定的 framebuffer。
    void draw(Live2D::Cubism::Framework::CubismMatrix44& mvpMatrix);

    // 删除所有 GL 纹理与渲染器。必须在 GL 上下文 current 时调用。
    void release();

    // 播放动作：group 如 "Idle"，no 为组内序号，priority 为优先级。
    Live2D::Cubism::Framework::CubismMotionQueueEntryHandle
    startMotion(const char* group, int no, int priority);

    // 切换表情，id 对应 model3.json 中 expressions 的 Name。
    void setExpression(const char* expressionId);

    bool isLoaded() const { return _loaded; }

private:
    // 读整个文件到 Framework 缓冲（csmByte*），调用方用完 ReleaseBytes 释放。
    static Live2D::Cubism::Framework::csmByte*
    loadFileAsBytes(const std::string& path, Live2D::Cubism::Framework::csmSizeInt* outSize);
    static void releaseBytes(Live2D::Cubism::Framework::csmByte* buffer);

    // 用 stb_image 解码 png 并建立 GL 纹理，返回纹理 id（0 表示失败）。
    unsigned int createTextureFromPng(const std::string& path);

    void setupModel();        // loadAssets 内部：按 setting 逐项加载
    void setupTextures();     // loadAssets 内部：建纹理并 BindTexture
    void preloadMotionGroup(const char* group);  // 预读某动作组

    Live2D::Cubism::Framework::ICubismModelSetting* _modelSetting = nullptr;
    std::string _modelDir;                         // 资源目录（含尾部斜杠）
    Live2D::Cubism::Framework::csmVector<unsigned int> _textures; // 已建纹理 id
    bool _loaded = false;
};

#endif // LIVE2DMODEL_HPP
```

### 3.3 `Live2DModel.cpp`

先是全局启动、文件读取、纹理工具：

```cpp
#include "Live2DModel.hpp"
#include "Live2DAllocator.hpp"

#include <GL/glew.h>          // CubismRenderer_OpenGLES2 内部依赖 glew
#include <CubismModelSettingJson.hpp>
#include <Motion/CubismMotion.hpp>
#include <Id/CubismIdManager.hpp>
#include <Utils/CubismString.hpp>

#define STB_IMAGE_IMPLEMENTATION   // 整个工程仅此一处定义实现
#include <stb_image.h>

#include <fstream>
#include <vector>

using namespace Live2D::Cubism::Framework;
using namespace Live2D::Cubism::Framework::Rendering;

namespace {
    // 全局只需一份分配器与启动选项，进程退出前一直存活。
    Live2DAllocator     g_allocator;
    CubismFramework::Option g_option;
}

// ── 进程级 Framework 启停 ───────────────────────────────────────────
bool Live2DModel::initFramework()
{
    g_option.LogFunction  = [](const char* msg){ /* 可接到 qDebug */ };
    g_option.LoggingLevel = CubismFramework::Option::LogLevel_Warning;

    CubismFramework::StartUp(&g_allocator, &g_option);
    CubismFramework::Initialize();
    return CubismFramework::IsInitialized();
}

void Live2DModel::disposeFramework()
{
    CubismFramework::Dispose();
}

// ── 文件 → 字节缓冲 ─────────────────────────────────────────────────
csmByte* Live2DModel::loadFileAsBytes(const std::string& path, csmSizeInt* outSize)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { *outSize = 0; return nullptr; }
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);

    // 用 Framework 的分配器分配，方便统一用 csmFree/DeallocateBytes 风格释放；
    // 这里简单用 new[]，配套 releaseBytes 用 delete[]。
    csmByte* buf = new csmByte[sz];
    if (!f.read(reinterpret_cast<char*>(buf), sz)) {
        delete[] buf; *outSize = 0; return nullptr;
    }
    *outSize = static_cast<csmSizeInt>(sz);
    return buf;
}

void Live2DModel::releaseBytes(csmByte* buffer)
{
    delete[] buffer;
}

// ── PNG → GL 纹理（stb_image 解码 + 生成 mipmap）────────────────────
unsigned int Live2DModel::createTextureFromPng(const std::string& path)
{
    int w, h, ch;
    // 强制 4 通道 RGBA，与 Cubism 渲染器预期一致
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
    if (!pixels) return 0;

    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    stbi_image_free(pixels);
    return tex;
}
```

接着是构造/析构与 `loadAssets` 主流程：

```cpp
Live2DModel::Live2DModel() = default;

Live2DModel::~Live2DModel()
{
    release();
    delete _modelSetting;
}

// 读 model3.json → setupModel() 逐项加载 → CreateRenderer → setupTextures()
bool Live2DModel::loadAssets(const std::string& dir, const std::string& fileName)
{
    _modelDir = dir;
    if (!_modelDir.empty() && _modelDir.back() != '/') _modelDir += '/';

    csmSizeInt size = 0;
    csmByte* buf = loadFileAsBytes(_modelDir + fileName, &size);
    if (!buf) return false;

    _modelSetting = new CubismModelSettingJson(buf, size);
    releaseBytes(buf);

    setupModel();          // moc3 / 物理 / 姿态 / 表情 / 动作 / 眨眼 / 呼吸

    if (_model == nullptr) return false;

    // 用模型画布尺寸创建渲染器（也可传 FBO 尺寸）
    CreateRenderer(static_cast<csmUint32>(_model->GetCanvasWidth()),
                   static_cast<csmUint32>(_model->GetCanvasHeight()));

    setupTextures();       // 把贴图建成 GL 纹理并 BindTexture

    _loaded = true;
    return true;
}

// 按 model3.json 清单逐项把字节缓冲喂给父类的 Load* 方法
void Live2DModel::setupModel()
{
    csmSizeInt size = 0;
    csmByte* buf = nullptr;

    // 1) moc3：模型几何与变形（必需）
    if (strcmp(_modelSetting->GetModelFileName(), "") != 0) {
        buf = loadFileAsBytes(_modelDir + _modelSetting->GetModelFileName(), &size);
        LoadModel(buf, size);
        releaseBytes(buf);
    }

    // 2) 表情（可选，多个）
    for (csmInt32 i = 0; i < _modelSetting->GetExpressionCount(); ++i) {
        const csmChar* name = _modelSetting->GetExpressionName(i);
        const csmChar* file = _modelSetting->GetExpressionFileName(i);
        buf = loadFileAsBytes(_modelDir + file, &size);
        ACubismMotion* exp = LoadExpression(buf, size, name);
        // CubismUserModel 内部 _expressions 容器持有；此处简化省略缓存
        releaseBytes(buf);
    }

    // 3) 物理（头发/裙摆摆动，可选）
    if (strcmp(_modelSetting->GetPhysicsFileName(), "") != 0) {
        buf = loadFileAsBytes(_modelDir + _modelSetting->GetPhysicsFileName(), &size);
        LoadPhysics(buf, size);
        releaseBytes(buf);
    }

    // 4) 姿态（部件显隐分组，可选）
    if (strcmp(_modelSetting->GetPoseFileName(), "") != 0) {
        buf = loadFileAsBytes(_modelDir + _modelSetting->GetPoseFileName(), &size);
        LoadPose(buf, size);
        releaseBytes(buf);
    }

    // 5) 眨眼 / 呼吸：用 SDK 内置效果器，参数 id 由默认参数表给出
    _eyeBlink = CubismEyeBlink::Create(_modelSetting);
    _breath   = CubismBreath::Create();
    csmVector<CubismBreath::BreathParameterData> breaths;
    const CubismId* idAngleX = CubismFramework::GetIdManager()->GetId("ParamAngleX");
    breaths.PushBack(CubismBreath::BreathParameterData(idAngleX, 0.0f, 15.0f, 6.5345f, 0.5f));
    _breath->SetParameters(breaths);

    // 6) 动作分组预读（把每组动作文件读入 _motions，供 startMotion 用）
    for (csmInt32 i = 0; i < _modelSetting->GetMotionGroupCount(); ++i)
        preloadMotionGroup(_modelSetting->GetMotionGroupName(i));

    _motionManager->StopAllMotions();
}

// 预读一个动作组里的所有 motion3.json
void Live2DModel::preloadMotionGroup(const char* group)
{
    csmInt32 count = _modelSetting->GetMotionCount(group);
    for (csmInt32 i = 0; i < count; ++i) {
        csmString name = csmString(group) + "_" + csmString::IntToString(i);
        csmSizeInt size = 0;
        csmByte* buf = loadFileAsBytes(
            _modelDir + _modelSetting->GetMotionFileName(group, i), &size);
        if (!buf) continue;
        CubismMotion* m = static_cast<CubismMotion*>(
            LoadMotion(buf, size, name.GetRawString()));
        releaseBytes(buf);
        // m 已由父类容器管理；实际工程可建 map<name, ACubismMotion*> 便于检索
    }
}
```

最后是纹理绑定、每帧 `update`、`draw`、`release` 与动作/表情：

```cpp
// 把 model3.json 里声明的每张贴图建成 GL 纹理，并按索引绑到渲染器
void Live2DModel::setupTextures()
{
    auto* renderer = GetRenderer<CubismRenderer_OpenGLES2>();
    // 预乘 alpha：官方贴图按预乘处理，开启后混合正确
    renderer->IsPremultipliedAlpha(true);

    csmInt32 texCount = _modelSetting->GetTextureCount();
    for (csmInt32 i = 0; i < texCount; ++i) {
        const csmChar* file = _modelSetting->GetTextureFileName(i);
        if (strcmp(file, "") == 0) continue;
        unsigned int tex = createTextureFromPng(_modelDir + file);
        _textures.PushBack(tex);
        renderer->BindTexture(i, tex);   // 第 i 张贴图 → GL 纹理 id
    }
}

// 每帧推进：动作 → 表情 → 呼吸 → 眨眼 → 物理 → 姿态 → 顶点重算
void Live2DModel::update(float dt)
{
    if (!_loaded) return;

    _model->LoadParameters();          // 取上一帧参数为基准
    bool motionUpdated = _motionManager->UpdateMotion(_model, dt);
    _model->SaveParameters();          // 存基准，供眨眼/呼吸叠加

    // 没有动作在播时才让眨眼生效，避免和动作打架
    if (!motionUpdated && _eyeBlink)
        _eyeBlink->UpdateParameters(_model, dt);

    if (_expressionManager) _expressionManager->UpdateMotion(_model, dt);
    if (_breath)   _breath->UpdateParameters(_model, dt);
    if (_physics)  _physics->Evaluate(_model, dt);
    if (_pose)     _pose->UpdateParameters(_model, dt);

    _model->Update();                  // Core 据当前参数重算所有顶点
}

// 把模型画到当前绑定的 framebuffer（外部已 bind 好 FBO + 设好 viewport）
void Live2DModel::draw(CubismMatrix44& mvp)
{
    if (!_loaded) return;
    auto* renderer = GetRenderer<CubismRenderer_OpenGLES2>();
    renderer->SetMvpMatrix(&mvp);
    renderer->DrawModel();             // 用模型自带 shader 绘制全部 ArtMesh
}

void Live2DModel::release()
{
    for (csmUint32 i = 0; i < _textures.GetSize(); ++i)
        if (_textures[i]) glDeleteTextures(1, &_textures[i]);
    _textures.Clear();
    DeleteRenderer();
    _loaded = false;
}

// 播放动作
CubismMotionQueueEntryHandle Live2DModel::startMotion(const char* group, int no, int priority)
{
    csmString name = csmString(group) + "_" + csmString::IntToString(no);
    // 实际工程从预读 map 取出 ACubismMotion*；此处示意从文件即时加载
    csmSizeInt size = 0;
    csmByte* buf = loadFileAsBytes(_modelDir + _modelSetting->GetMotionFileName(group, no), &size);
    CubismMotion* m = static_cast<CubismMotion*>(LoadMotion(buf, size, name.GetRawString()));
    releaseBytes(buf);
    return _motionManager->StartMotionPriority(m, /*autoDelete*/ false, priority);
}

// 切换表情（需在 setupModel 时把表情缓存进 map；此处给出接口形态）
void Live2DModel::setExpression(const char* /*expressionId*/)
{
    // _expressionManager->StartMotionPriority(_expressions[id], false, Priority);
}
```

---

## 4. 与现有工程对接

### 4.1 CMakeLists.txt

源文件加进可执行目标（链接 / include 已配好，无需再动）：

```cmake
qt_add_executable(qt-learn
    MANUAL_FINALIZATION
    ${PROJECT_SOURCES}
    ...
    GlCompositor.h
    GlCompositor.cpp
    Live2DModel.hpp        # 新增
    Live2DModel.cpp        # 新增
    Live2DAllocator.hpp    # 新增（纯头文件）
    ...
)
```

> `Framework`（含 Core、GLEW）、`stb` include 路径已在 `target_link_libraries` / `target_include_directories` 里。`STB_IMAGE_IMPLEMENTATION` 在 `Live2DModel.cpp` 里定义一次即可。

### 4.2 GlCompositor 接入点

`GlCompositor` 持有一个 `Live2DModel* m_live2d`，在三处插桩：

**① `init()` 里，`buildGl()` 之后**（上下文已 current）：

```cpp
m_ctx->makeCurrent(m_surface);
initializeOpenGLFunctions();
buildGl();

// —— Live2D：进程级 Framework 启动 + 加载模型 ——
glewExperimental = GL_TRUE;
glewInit();                                // Cubism 渲染器依赖 glew
Live2DModel::initFramework();
m_live2d = new Live2DModel();
m_live2d->loadAssets("models/Hiyori/", "Hiyori.model3.json");

m_elapsed.start();                         // QElapsedTimer，用于算 dt
m_ctx->doneCurrent();
```

**② `onScreenFrame()` 里，桌面/相机画完之后、`glFinish()` 之前**：

```cpp
if (hasScreen) drawQuad(m_texScreen, -1.f, -1.f, 2.f, 2.f);
if (hasCamera) { /* 相机小窗 ... */ }

// —— Live2D：叠加到合成画面 ——
if (m_live2d && m_live2d->isLoaded()) {
    float dt = m_elapsed.restart() / 1000.0f;
    m_live2d->update(dt);

    // MVP：把模型归一化坐标映射到输出。按宽高比缩放，避免拉伸。
    CubismMatrix44 mvp;
    float aspect = float(m_outputSize.width()) / m_outputSize.height();
    mvp.Scale(1.0f / aspect, 1.0f);        // 竖向铺满，横向按比例
    m_live2d->draw(mvp);
}

glFinish();
```

> 注意：Cubism 渲染器会改 GL 状态（混合、深度、program、VAO 绑定）。它内部用 `SaveProfile/RestoreProfile` 保存恢复一部分，但 `drawQuad` 用的是自己的 `m_prog`/`m_vao`，每帧都重新 `bind`，所以顺序上让 Live2D 最后画即可，不互相污染。若发现合成 quad 异常，在 `drawQuad` 开头补一次完整的状态设置。

**③ `cleanup()` 里，删 GL 资源时**（上下文 current）：

```cpp
m_ctx->makeCurrent(m_surface);

if (m_live2d) { m_live2d->release(); delete m_live2d; m_live2d = nullptr; }
Live2DModel::disposeFramework();

if (m_texScreen) { glDeleteTextures(1, &m_texScreen); m_texScreen = 0; }
// ... 其余原有清理 ...
```

`GlCompositor.h` 需新增成员：

```cpp
#include "Live2DModel.hpp"
#include <QElapsedTimer>
// ...
private:
    Live2DModel*  m_live2d = nullptr;
    QElapsedTimer m_elapsed;          // 帧间隔 dt
```

---

## 5. 加载流程全景

```
loadAssets(dir, "Hiyori.model3.json")
   │
   ├─ loadFileAsBytes(model3.json) ──► new CubismModelSettingJson(buf,size)
   │                                       （解析出清单：moc3/贴图/动作…路径）
   ├─ setupModel()
   │     ├─ LoadModel(moc3 字节)        → Core 解析几何，父类持有 _model
   │     ├─ LoadExpression × N          → 表情动作
   │     ├─ LoadPhysics(physics3.json)  → _physics
   │     ├─ LoadPose(pose3.json)        → _pose
   │     ├─ CubismEyeBlink::Create      → _eyeBlink
   │     ├─ CubismBreath::Create        → _breath
   │     └─ preloadMotionGroup × 组数    → 动作缓存
   │
   ├─ CreateRenderer(canvasW, canvasH)  → new CubismRenderer_OpenGLES2
   │
   └─ setupTextures()
         └─ 每张 PNG: stbi_load → glTexImage2D → renderer->BindTexture(i, tex)

每帧：
   update(dt): 动作→表情→呼吸→眨眼→物理→姿态→ _model->Update()
   draw(mvp) : renderer->SetMvpMatrix → DrawModel  （画到当前 FBO）
```

---

## 6. 易踩的坑

1. **线程**：`loadAssets`/`draw`/`release` 必须在 GL 上下文 current 的渲染线程。放到主线程或上下文没 current 时调，建纹理/绘制会静默失败或崩。
2. **glewInit 时机**：必须在上下文 current *之后* 调一次，否则 Cubism 渲染器里的 GL 函数指针是空的。
3. **预乘 alpha**：官方贴图是预乘的，渲染器要 `IsPremultipliedAlpha(true)`，且合成 shader 的混合方程要配套（预乘用 `GL_ONE, GL_ONE_MINUS_SRC_ALPHA`）。透明边缘发黑通常就是这里没对上。
4. **DLL**：`Live2DCubismCore.dll` 已由 CMake 的 POST_BUILD 拷到 exe 旁，运行时找不到 DLL 时先查这一步。
5. **路径**：`model3.json` 里全是相对路径，`loadAssets` 的 `dir` 要是它所在目录；模型目录建议随 exe 部署（如 `models/` 加到拷贝步骤）。
6. **状态污染**：Cubism 改了混合/VAO 等全局状态，让它在每帧最后画，自己的 `drawQuad` 每帧重新绑定 program/VAO，避免相互影响。
