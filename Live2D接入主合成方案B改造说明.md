# Live2D 接入主合成（方案 B）改造说明

> 把 Live2D 渲染流程接入 `GlCompositor` 主合成。采用**方案 B**：Live2D 先渲染到
> 一张独立离屏 FBO/纹理，再当作普通 quad 贴到主输出 FBO 上。
>
> **所有新代码都在主文件夹重新编写，未调用 `Live2D/` 目录下任何文件。**

---

## 一、核心结论：为什么不需要新增线程 / 第二个 GL 上下文

| 维度 | 判断 | 原因 |
|---|---|---|
| 离屏渲染**线程** | **不需要新增** | `GlCompositor` 本身就是一条独立渲染线程：`init()` 自建 `QOpenGLContext` + `QOffscreenSurface`，`onScreenFrame()` 全程 `makeCurrent → 画 → 回读`。Live2D 渲染寄生在这条线程即可。 |
| 第二个 GL **上下文** | **不需要新增** | 上下文判据只有两条：①线程归属——同一上下文同一时刻只能 current 在一条线程，而我们本来就在一条线程串行画完桌面→相机→人物；②对象命名空间——同上下文内纹理 id 互通，离屏纹理才能被 `drawQuad` 直接采样。两条都满足，无需第二个上下文（否则还要配 shared context）。 |
| 输出 **FBO** | **方案 B 新增 1 个** | 这是「渲染目标切换」，与上下文数量无关。一个上下文里可有任意多 FBO，`glBindFramebuffer` 一条指令切换。 |

> 注：`L2dRenderer` 内部那张 `_maskFbo` 是裁剪蒙版算法固有的，任何方案都保留，不计入「新增输出 FBO」。

### 方案 B 的数据流

```
onScreenFrame(每帧):
  ── 层渲染阶段 ──
  L2dLayer::render(dt):
      target.ensure(720x1080)        // 离屏 FBO+纹理（尺寸独立于输出，省显存）
      model.updateMotion(dt)         // 动作推进（与 draw 配对，同线程）
      target.bindAndClear()          // 绑离屏 FBO，清成透明底(0,0,0,0)
      renderer.draw(mvp,720,1080)    // 画到离屏 FBO（内部蒙版预渲染→恢复回该 FBO）
      return target.texture()        // 返回承载人物的纹理
  ── 合成阶段 ──
  m_fbo->bind()                      // 绑回主输出
  drawQuad(桌面)  → 铺满，不混合
  drawQuad(相机)  → 右下角，不混合
  composeLayer(人物纹理)             // 按 placement 换算 NDC，预乘 alpha 混合贴上
  glFinish(); m_fbo->toImage()       // 回读 → frameReady 推流
```

---

## 二、新增文件（主文件夹）

| 文件 | 角色 | 设计意图 |
|---|---|---|
| `ICompositorLayer.h` | **合成层抽象接口** | 「渲染出一张纹理」的统一契约。后续加新层（第二个形象、字幕、特效）只需再实现本接口，`GlCompositor` 不改。含 `init/render/placement/premultiplied/bottomUp/release`。 |
| `OffscreenTarget.h` | **离屏目标 RAII（FBO+纹理）** | 方案 B 的新增 FBO 封装成可复用小类，任何「先渲染到纹理再合成」的层都能用。`ensure()` 按需建/重建，`bindAndClear()` 绑定+透明清屏。 |
| `L2dModel.h/.cpp` | **模型数据层** | 从 `Live2D/Live2DModel` 移植改名，去掉 GUI 专用的 `QTimer` 成员；加载 moc3、设默认参数、管理动作。纯 CPU，不碰 GL。 |
| `L2dRenderer.h/.cpp` | **自定义 GL 渲染器** | 从 `Live2D/Live2DRenderer` 移植改名。逐 drawable 绘制 + 自实现裁剪蒙版。关键：`draw()` 自动保存/恢复「调用时绑定的 FBO」，故默认画到调用方当前 FBO。 |
| `L2dLayer.h/.cpp` | **Live2D 合成层** | 把 `L2dModel + L2dRenderer + OffscreenTarget` 打包成 `ICompositorLayer`。`render()` 内部完成离屏渲染并返回纹理。 |

> `L2dRenderer.cpp` 相比原版去掉了 `requestMaskDump/dumpMaskTextures`（PNG 导出是查看器调试用，合成不需要）。

---

## 三、改动文件

### `GlCompositor.h`
- 新增 `#include "ICompositorLayer.h"`、`<QElapsedTimer>`、`<vector>`、`<memory>`。
- 新增 `void addLayer(std::unique_ptr<ICompositorLayer>)`：注册合成层（须在 `init()` 前调用）。
- 新增成员 `std::vector<std::unique_ptr<ICompositorLayer>> m_layers;` 与帧计时 `QElapsedTimer m_frameClock;`。
- `drawQuad` 扩展签名：`drawQuad(tex, x, y, w, h, bool flipV=false, bool premultiplied=false, bool blend=false)`。
- 新增 `void composeLayer(ICompositorLayer*, GLuint tex)`：按 `placement` 换算 NDC + 选择混合方程。

### `GlCompositor.cpp`
- `drawQuad`：按 `blend/premultiplied` 选混合方程（预乘用 `GL_ONE, GL_ONE_MINUS_SRC_ALPHA`），按 `flipV` 设 `uFlipV` uniform；画完还原 `GL_BLEND`。
- `composeLayer`：QRect（左上原点）→ GL（左下原点）换算，调 `drawQuad` 叠加。
- `onScreenFrame`：桌面/相机 quad 之后、`glFinish()` 之前，遍历 `m_layers`：`render(dt)` → 重新 `m_fbo->bind()` → `composeLayer()`。dt 由 `m_frameClock` 提供（采集帧率驱动动画）。
- `init()`：`buildGl()` 后逐层 `init()`，失败的层移除。
- `cleanup()`：`doneCurrent()` 前先逐层 `release()` 并清空 `m_layers`。

### `Shader/composite.vert`
- 新增 `uniform int uFlipV`。原来 V 翻转是写死的（适配自上而下的 QImage 上传纹理）；FBO 离屏渲染结果是自下而上，需可切换。`uFlipV=1` 时不翻（抵消默认翻转）。

### `DesktopCapturer.h`
- `#include "L2dLayer.h"`。
- 在 `m_compositor = new GlCompositor()` 之后、`moveToThread` 之前，`addLayer(std::make_unique<L2dLayer>(...))` 注册人物层（离屏 720x1080，贴输出左下角）。

### `CMakeLists.txt`
- `qt-learn` 目标的源文件列表新增：`ICompositorLayer.h`、`OffscreenTarget.h`、`L2dModel.*`、`L2dRenderer.*`、`L2dLayer.*`。
- `qt-learn` 已链接 `Framework`（Cubism Core + Framework），无需额外链接。

---

## 四、关键技术点 / 坑

1. **预乘 alpha 混合**：`L2dRenderer` 片元输出预乘 alpha，合成该 quad 时必须用
   `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`。桌面层不透明不混合，故每次 `drawQuad`
   单独开/关 `GL_BLEND`。
2. **V 翻转方向**：离屏 FBO 纹理原点在左下（`bottomUp()=true`），与从 QImage 上传的
   自上而下纹理相反，靠 `uFlipV` 区分。
3. **draw 的 FBO 保存/恢复**：`L2dRenderer::draw` 用 `glGetIntegerv(GL_FRAMEBUFFER_BINDING)`
   保存当前 FBO，蒙版预渲染切到 `_maskFbo`，画完恢复——保存值正是 `L2dLayer` 绑定的离屏
   FBO，故能正确回到它。
4. **层渲染后必须重绑主 FBO**：`layer->render()` 会改变绑定的 FBO 与 GL 状态，故每层画完
   都 `m_fbo->bind()` + `glViewport` 再 `composeLayer`。
5. **动画节拍**：原 `Live2DWidget` 用 GUI 线程 `QTimer` 驱动；接入合成后改由采集帧率驱动
   （`m_frameClock.restart()` 算 dt 喂 `updateMotion`）。若采集帧率不稳，动作会跟随抖动。
6. **蒙版纹理尺寸**：按离屏渲染尺寸（720x1080）重建，不再是输出全尺寸，省显存。

---

## 五、构建验证

- ⚠️ `cmake-build-debug/`（CLion 工程）构建失败，报 `as: unknown option -- gdwarf-5`。
  这是**环境问题**：CLion 的新 GCC 配上 PATH 里旧的 `C:\mingw64\bin\as`(binutils 2.30)，
  与本次改动无关（未改动的 `mainwindow.cpp` 同样失败）。
- ✅ `build/Desktop_Qt_6_11_1_MinGW_64_bit-Debug/`（Qt Creator 工程，用 Qt 自带
  `mingw1310_64` 工具链）**编译链接全部通过**，含全部新增/改动文件及 `DesktopCapturer.h` 配线。

  ```
  [ 88%] Linking CXX executable qt-learn.exe
  [100%] Built target qt-learn
  ```

> 未做运行时验证（需真实模型路径与桌面/相机捕获环境）。模型路径目前写死在
> `DesktopCapturer.h`，按需改 `dir/modelFile/motionFile/placement`。

---

## 六、后续拓展示例

加第二个形象 / 字幕层，无需改 `GlCompositor`：

```cpp
m_compositor->addLayer(std::make_unique<L2dLayer>(dir2, "B.model3.json",
                       "motions/idle.motion3.json", QSize(540,960),
                       QRect(1300, 100, 540, 960)));   // 右侧再放一个
// 或实现一个 SubtitleLayer : public ICompositorLayer 加字幕……
```
