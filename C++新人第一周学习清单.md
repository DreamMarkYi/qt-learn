# C++ 部分 —— 第一周学习清单

目标：一周内看懂 `qt-learn` 主程序从「采集 → 合成 → 渲染 → 推流」的完整链路，
具备独立改动/调试的能力。每天「学一块知识」配「在真代码上做一件小事」，不空对空看文档。

学习深度以「够用、能看懂当前代码」为准，不要求精通，遇到细节可以跳到
[新人入门指南.md](./新人入门指南.md) 第 7 节列的知识点再深挖。

## Day 1 —— Qt 基础 + 把项目跑起来

**学到哪**
- Qt 项目结构：CMake 组织方式（`find_package`/`target_link_libraries`），不需要学 qmake
- `QObject`/信号槽机制：`connect()` 的几种写法（成员函数指针、lambda）
- 信号槽的**跨线程行为**：`Qt::AutoConnection` 在跨线程时自动变成队列连接——这是本项目
  几乎所有模块解耦的基础，第一天必须搞懂
- `QMainWindow` + `.ui` 文件的关系（`ui_mainwindow.h` 是自动生成的）

**配合做什么**
- 用 Qt Creator 或 CMake 把 `qt-learn` target 构建、跑起来，界面能打开
- 通读 `mainwindow.cpp`：找出每个按钮的 `connect()`，画一张「按钮 → 触发了哪个函数」的小表
- 动手改一个不影响主流程的小地方，比如给「开启预览」按钮加一行 `qDebug()` 打印当前时间，
  重新编译确认生效——目的是确认环境能编译、能调试，不是加真功能

## Day 2 —— Qt Multimedia：采集链路

**学到哪**
- `QScreenCapture`（桌面捕获）、`QMediaCaptureSession`（会话把 capture 和 sink 绑一起）
- `QCamera` / `QVideoSink` / `QVideoFrame`：一帧画面怎么从设备流到你的回调里
- 像素格式概念：RGBA vs NV12（YUV 420 的一种），知道「为什么摄像头帧要单独处理」即可，
  不需要背 YUV 转 RGB 公式

**配合做什么**
- 读 `CameraManager.*` + `DesktopCapturer.h` 里 `m_screenSession`/`m_cameraManager` 那几行，
  搞清楚 `setVideoSink()` 把数据接到了 `GlCompositor` 的哪个 sink
- 在 `GlCompositor.h` 里 `m_cameraSink` 的 lambda 处（已有一段 `qDebug` 诊断打印格式/尺寸），
  运行程序、切换不同摄像头，观察控制台打印的格式/尺寸变化——直观感受「一帧数据长什么样」
- 小任务：给下拉框选择摄像头这段逻辑加一个「刷新摄像头列表」按钮（复用
  `DesktopCapturer::availableCameras()` 即可，不用新写采集逻辑）

## Day 3 —— 多线程模型 + 渲染节拍

**学到哪**
- `QThread` 的 **worker + moveToThread** 用法（本项目用的这种，不是继承 QThread）：
  为什么 `QTimer`/GL 上下文要在 `QThread::started` 里创建，才能保证和渲染线程同线程
- 为什么渲染要用固定 `QTimer`（60fps）驱动，而不是「来一帧画一帧」
  （采集帧率和渲染帧率解耦，副屏静止/丢帧时动画不冻结）
- `PoseBus` 那种「一把 mutex 保护、只存最新值」的简单线程安全写法，理解它为什么够用
  （不需要历史数据，只关心当前状态）

**配合做什么**
- 读 `DesktopCapturer.h` 里 `m_renderThread`/`moveToThread`/`connect(started, init)` 这几行，
  和 `GlCompositor.h` 里 `m_renderTimer`、`kRenderIntervalMs` 的注释
- 读 `PoseBus.h`（很短，20 多行），理解 `publishPose`/`pose()` 各自在哪个线程被调用
- 小任务：把 `kRenderIntervalMs` 从 16（≈60fps）改成 33（≈30fps），跑一下感受画面流畅度变化，
  然后改回来——用来直观建立「渲染节拍」这个概念，不是真要改配置

## Day 4 —— OpenGL 基础：着色器 + FBO + 纹理

**学到哪**
- OpenGL 3.3 Core 最基本的管线：VAO/VBO/EBO 是什么、顶点着色器和片段着色器分别干嘛
- FBO（Framebuffer Object）离屏渲染：为什么 Live2D 人物要先画到一张「独立画布」上，
  再当成一张贴图贴到最终画面里
- NDC（归一化设备坐标，-1~1）：合成时怎么把「像素矩形位置」换算成 NDC 矩形
- `flipV`/`premultiplied`/`blend` 这几个参数大概解决什么问题（翻转、预乘 alpha、混合）
  不需要能自己推导所有数学，能看懂 `drawQuad` 的调用在做什么就够

**配合做什么**
- 看 `Shader/composite.vert` + `Shader/composite.frag`（很短），对照 `GlCompositor::drawQuad`
  的实现读一遍，搞清楚顶点数据怎么传进着色器
- 读 `GlCompositor::composeLayer`，理解 Live2D 层的离屏纹理是怎么贴到主输出 FBO 上的
- 小任务：改一下摄像头画中画的位置/大小（`GlCompositor.h` 里的 `m_camSize`/`m_margin`），
  编译运行看小窗位置变化——用来验证「像素矩形 → NDC」这层换算你是不是看懂了

## Day 5 —— Live2D 概念 + 推流基础，串联全流程

**学到哪**
- Cubism SDK 的 Core（二进制推理库，读 `.moc3` 算骨骼变形）和 Framework（C++ 胶水层，
  管模型加载/参数/动作播放）的分工，不需要看 Core 源码（本来就是预编译的）
- `.model3.json`（模型清单）、Parameter（参数，如 `ParamEyeLOpen`）、`.motion3.json`（动作）
  这三个概念，对应 `PoseTypes.h` 里 `FaceData`/`BodyData` 的字段是怎么喂进去的
- `ICompositorLayer` 接口设计意图：为什么这样抽象以后能随便加字幕层/特效层
- FFmpeg 推流基础：`ffmpeg` 是被当成子进程拉起来纯编码用的，输入是 stdin 里的原始 BGRA 帧，
  不需要搞懂 FFmpeg 内部实现，知道「一路画面 + 一路混音音频 → RTMP」这个模型即可

**配合做什么**
- 读 `L2dLayer.h` + `L2dLayer::render` 的实现，搞清楚它怎么把 `PoseBus` 里的 `FaceData`
  取出来、赋值给哪个 Live2D 参数
- 读 `FFmpegStreamer.h` 的 `startPush`，理解 RTMP 地址、分辨率、音频设备参数从哪来
- 收尾任务（串联全流程）：换一个 Live2D 模型目录/动作文件试跑（改
  `DesktopCapturer.h` 里 `l2d` 那段的 `dir`/`modelFile`/`motionFile`），确认模型能加载、
  能被姿态数据驱动；跑一次完整链路：启动 Python 边车 → 启动主程序 → 开预览 → 开推流，
  用 `ffplay rtmp://...` 或本地播放器验证真的能收到流

## 第一周结束时，应该能回答这几个问题

- 一帧摄像头画面从「进入程序」到「出现在合成画面里」，经过了哪几个对象、哪几次线程切换？
- 如果 Live2D 形象不动了，可能是链路上哪一环出的问题？会去看哪个文件、加什么调试打印？
- `GlCompositor` 为什么不是「来一帧画一帧」，而是固定 60fps 定时器驱动？
- Live2D 人物为什么要先渲染到离屏 FBO，而不是直接画到主输出上？

答不上来的部分回去对应 Day 的小节重新过一遍；都能答上来就可以开始接手正式任务了。
