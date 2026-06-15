# 方案 C:原生 OpenGL GPU 合成推流

用 **OpenGL**(经 Qt 的 `QOpenGLContext` / `QOpenGLFunctions` / `QOpenGLShaderProgram` / `QOpenGLFramebufferObject` 封装,而非裸 WGL)把"桌面 + 相机"两路画面在 GPU 上合成成一张,再回读到 CPU 喂给 ffmpeg 编码推流。

> 与方案 A(QRhi)的关系:同样是"GPU 合成 + CPU 软编"。区别是这里直接写 OpenGL,着色器和渲染流程对你完全透明、可控,不经过 QRhi 抽象层。代价是要自己管 OpenGL 上下文和跨线程问题,且跨平台性不如 QRhi(但桌面 Windows/Linux/macOS 的 OpenGL 都能跑)。

## 为什么选原生 OpenGL

| | QRhi(方案 A) | 原生 OpenGL(方案 C) |
|---|---|---|
| 抽象层 | 有,底层自动选 D3D11/VK/GL | 无,直接调 GL |
| 着色器语言 | 统一 `.qsb`(需 qsb 工具预编译) | 直接写 GLSL 字符串,运行期编译 |
| 学习曲线 | 需懂 QRhi 的 resource/pass 模型 | 经典 GL 流程,资料多 |
| 跨平台 | 最好 | 桌面够用,移动端是 GLES |
| 与 QVideoFrame 集成 | Qt 内部同款,最顺 | 需自己处理纹理桥接 |

如果你熟悉经典 OpenGL 管线(VBO/FBO/shader/纹理),方案 C 上手反而比 QRhi 直观。

## 整体链路

```
QScreenCapture ─→ session ─→ screenSink.videoFrameChanged
QCamera ───────→ session ─→ cameraSink.videoFrameChanged
                                     │
                       QVideoFrame → GLuint 纹理
                                     │
              ┌──────────────────────────────────────────┐
              │  OpenGL 离屏渲染(QOpenGLContext)         │
              │  1. 绑定 FBO(1920x1080 RGBA8)            │
              │  2. 画桌面纹理铺满整屏(全屏四边形)      │
              │  3. 画相机纹理到右下角(小四边形)        │
              └──────────────────────────────────────────┘
                                     │
                       glReadPixels → QImage(RGBA)
                                     │
                    ┌────────────────┴───────────────┐
                    ▼                                 ▼
              QLabel 预览                  FFmpegStreamer::writeFrame
```

合成节奏依旧由**桌面帧驱动**,相机帧缓存最新一张。这点与 CPU/QRhi 版一致。

## 线程模型(重要)

OpenGL 上下文绑定在某个线程上。最简单稳妥的做法:**全部在主线程(GUI 线程)做**。`videoFrameChanged` 信号默认在主线程触发,合成和回读也在主线程,不涉及跨线程上下文切换。

进阶(本文档不展开):把 OpenGL 放独立线程,用 `moveToThread` + 离屏 surface,主线程只发帧。仅在主线程渲染卡顿明显时才需要。

---

## 第 1 步:CMake 加 OpenGL 依赖

`CMakeLists.txt` 里给 Qt 组件补上 `OpenGL` 和 `OpenGLWidgets`(后者只在用 `QOpenGLWidget` 时需要;纯离屏渲染用 `QOpenGLContext` 属于 `Gui` 模块,但链接 `Qt6::OpenGL` 更省事)。

```cmake
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia MultimediaWidgets OpenGL)

target_link_libraries(qt-learn PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
    Qt6::Multimedia Qt6::MultimediaWidgets
    Qt6::OpenGL                       # 新增
    opengl32                          # Windows 系统库;Linux 用 GL
)
```

---

## 第 2 步:新增 `GlCompositor.h`

OpenGL 合成器。持有一个离屏 `QOpenGLContext` + `QOffscreenSurface`,在桌面帧到达时把两路纹理画进 FBO,再 `glReadPixels` 回读。

```cpp
#ifndef GLCOMPOSITOR_H
#define GLCOMPOSITOR_H

#include <QObject>
#include <QVideoSink>
#include <QVideoFrame>
#include <QImage>
#include <QSize>
#include <QOpenGLContext>
#include <QOffscreenSurface>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLFramebufferObject>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QDebug>

class GlCompositor : public QObject, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    explicit GlCompositor(QObject *parent = nullptr) : QObject(parent) {
        // 1) 创建离屏 OpenGL 上下文 + surface
        QSurfaceFormat fmt;
        fmt.setRenderableType(QSurfaceFormat::OpenGL);
        fmt.setVersion(3, 3);
        fmt.setProfile(QSurfaceFormat::CoreProfile);

        m_ctx = new QOpenGLContext(this);
        m_ctx->setFormat(fmt);
        m_ctx->create();

        m_surface = new QOffscreenSurface(nullptr, this);
        m_surface->setFormat(fmt);
        m_surface->create();

        // 2) 两路 sink
        m_screenSink = new QVideoSink(this);
        m_cameraSink = new QVideoSink(this);
        connect(m_screenSink, &QVideoSink::videoFrameChanged,
                this, &GlCompositor::onScreenFrame);
        connect(m_cameraSink, &QVideoSink::videoFrameChanged, this,
                [this](const QVideoFrame &f) { m_lastCamera = f; });

        // 3) 在上下文里建管线资源
        m_ctx->makeCurrent(m_surface);
        initializeOpenGLFunctions();
        buildGl();
        m_ctx->doneCurrent();
    }

    ~GlCompositor() {
        // 释放复用纹理(必须在有效上下文里删 GL 资源)
        if (m_ctx->makeCurrent(m_surface)) {
            if (m_texScreen) glDeleteTextures(1, &m_texScreen);
            if (m_texCamera) glDeleteTextures(1, &m_texCamera);
            m_ctx->doneCurrent();
        }
    }

    QVideoSink* screenSink() const { return m_screenSink; }
    QVideoSink* cameraSink() const { return m_cameraSink; }
    QSize outputSize() const { return m_outputSize; }
    void setOverlay(bool on) { m_overlay = on; }
    bool overlay() const { return m_overlay; }

signals:
    void frameReady(const QImage &frame);

private:
    QOpenGLContext*  m_ctx = nullptr;
    QOffscreenSurface* m_surface = nullptr;
    QOpenGLShaderProgram* m_prog = nullptr;
    QOpenGLFramebufferObject* m_fbo = nullptr;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer m_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_ebo{QOpenGLBuffer::IndexBuffer};   // 索引缓冲(EBO)

    QVideoSink* m_screenSink = nullptr;
    QVideoSink* m_cameraSink = nullptr;
    QVideoFrame m_lastCamera;

    bool m_overlay = true;
    const QSize m_outputSize{1920, 1080};
    const QSize m_camSize{480, 360};
    const int   m_margin = 20;

    // 长期复用的两个纹理(避免每帧 new/delete)，及其已分配尺寸
    GLuint m_texScreen = 0;
    GLuint m_texCamera = 0;
    QSize  m_texScreenSize;
    QSize  m_texCameraSize;

    // —— 下面几个步骤的实现见后文 ——
    void buildGl();
    void onScreenFrame(const QVideoFrame &screenFrame);
    // 把一帧上传到复用纹理 tex（首帧分配，之后只更新像素）；返回是否成功
    bool uploadFrame(GLuint &tex, QSize &cachedSize, const QVideoFrame &f);
    void drawQuad(GLuint tex, float x, float y, float w, float h); // NDC 矩形
};

#endif // GLCOMPOSITOR_H
```

---

## 第 3 步:把着色器拆成单独文件

不把 GLSL 写死在 C++ 里,而是放进独立的 `.vert` / `.frag` 文件,再用 Qt 资源系统(`.qrc`)编译进可执行文件 —— 这样运行时路径不依赖工作目录,发布时也不用额外带文件。

新建 `shaders/composite.vert`:
```glsl
#version 330 core
layout(location=0) in vec2 aPos;   // 单位四边形 [0,1]x[0,1]
uniform vec4 uRect;                // x,y,w,h 均为 NDC
out vec2 vUv;
void main() {
    vUv = vec2(aPos.x, 1.0 - aPos.y);          // 翻转 V：GL 纹理原点在左下
    vec2 ndc = uRect.xy + aPos * uRect.zw;     // 映射到目标矩形
    gl_Position = vec4(ndc, 0.0, 1.0);
}
```

新建 `shaders/composite.frag`:
```glsl
#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
uniform int uSwizzleBGRA;          // 1=输入是BGRA，需交换R/B
out vec4 FragColor;
void main() {
    vec4 c = texture(uTex, vUv);
    FragColor = (uSwizzleBGRA == 1) ? c.bgra : c;
}
```

新建 `shaders.qrc`(用 `alias` 让资源路径更短):
```xml
<RCC>
    <qresource prefix="/shaders">
        <file alias="composite.vert">shaders/composite.vert</file>
        <file alias="composite.frag">shaders/composite.frag</file>
    </qresource>
</RCC>
```

`CMakeLists.txt` 把 `shaders.qrc` 加进可执行文件的源列表即可(AUTORCC 会自动处理):
```cmake
qt_add_executable(qt-learn
    main.cpp mainwindow.cpp
    # ...其它源文件...
    shaders.qrc                       # 新增
)
```

加了 `alias` 后,资源路径就是 `:/shaders/composite.vert`(不加 alias 则是 `:/shaders/shaders/composite.vert`)。

---

## 第 4 步:`buildGl()` — 从文件加载着色器 + 几何

着色器改成 `addShaderFromSourceFile` 从资源加载,几何部分不变。每步都检查返回值并打日志,编译失败时能立刻定位。

```cpp
void GlCompositor::buildGl() {
    // FBO:1920x1080 RGBA8
    QOpenGLFramebufferObjectFormat ff;
    ff.setInternalTextureFormat(GL_RGBA8);
    m_fbo = new QOpenGLFramebufferObject(m_outputSize, ff);

    // ---- 从单独文件加载着色器 ----
    m_prog = new QOpenGLShaderProgram(this);
    if (!m_prog->addShaderFromSourceFile(QOpenGLShader::Vertex,
                                         ":/shaders/composite.vert"))
        qWarning() << "顶点着色器编译失败:" << m_prog->log();
    if (!m_prog->addShaderFromSourceFile(QOpenGLShader::Fragment,
                                         ":/shaders/composite.frag"))
        qWarning() << "片元着色器编译失败:" << m_prog->log();

    m_prog->bindAttributeLocation("aPos", 0);
    if (!m_prog->link())
        qWarning() << "着色器链接失败:" << m_prog->log();

    // ---- 单位四边形:EBO 索引绘制(4 顶点 + 6 索引)----
    // 4 个唯一顶点(原 glDrawArrays 版要重复成 6 个，这里用索引复用)
    static const float quad[] = {
        0.f, 0.f,   // 0 左下
        1.f, 0.f,   // 1 右下
        1.f, 1.f,   // 2 右上
        0.f, 1.f,   // 3 左上
    };
    static const unsigned int idx[] = {
        0, 1, 2,    // 右下三角
        0, 2, 3,    // 左上三角
    };
    m_vao.create();
    m_vao.bind();

    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(quad, sizeof(quad));

    m_ebo.create();
    m_ebo.bind();                       // EBO 的绑定会被当前 VAO 记住
    m_ebo.allocate(idx, sizeof(idx));

    m_prog->enableAttributeArray(0);
    m_prog->setAttributeBuffer(0, GL_FLOAT, 0, 2);

    m_vao.release();                    // 必须先解绑 VAO
    m_vbo.release();                    // 再解绑 VBO
    m_ebo.release();                    // 最后解绑 EBO——顺序反了会把 EBO 关联从 VAO 抹掉
}
```

> **EBO 对四边形的收益可忽略。** 这里只省 2 个顶点(48 字节),GPU 画两个三角形的开销看不出差别。EBO 的真正价值在**顶点量大、共享顶点多**的网格(成百上千顶点的模型),四边形用它主要是写法规范。若觉得多余,保留原 `glDrawArrays` 6 顶点版完全没问题。
>
> **两个易错点:**
> - EBO 必须在 **VAO 绑定期间**绑定,VAO 会记住"用哪个 EBO";解绑时先 `m_vao.release()` 再解绑其它,顺序反了 VAO 会丢掉 EBO 关联。
> - `QOpenGLBuffer` 类型要用 `QOpenGLBuffer::IndexBuffer`(对应 `GL_ELEMENT_ARRAY_BUFFER`)。

> **buildGl 写在哪?** 它是 `GlCompositor` 的成员函数,和文档其它部分一样**写在 `GlCompositor.h`**。既然你项目里 `CameraManager`/`FFmpegStreamer` 都是纯头文件内联风格,最一致的做法是把函数体直接写进类声明的 `{}` 内(类内定义天然 inline)。若想类外定义在头文件里,记得加 `inline` 防止重复定义;或拆出 `GlCompositor.cpp` 并加入 CMake 源列表。
>
> **调试期想免编译改着色器:** 把路径换成磁盘相对路径(如 `"shaders/composite.vert"`),改 GLSL 后不用重新编译 C++,但要保证 shaders 目录在程序工作目录下。定型后再切回 `:/` 资源路径发布。

> **坐标系说明:** `uRect` 用 NDC(-1..1)。桌面铺满 = `(-1,-1,2,2)`;相机右下角小窗需要把"像素位置"换算成 NDC(见下一步)。

---

## 第 5 步:`drawQuad()` — 画一个纹理矩形

```cpp
// x,y 为矩形左下角 NDC 坐标，w,h 为 NDC 宽高
void GlCompositor::drawQuad(GLuint tex, float x, float y, float w, float h) {
    m_prog->bind();
    m_vao.bind();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    m_prog->setUniformValue("uTex", 0);
    m_prog->setUniformValue("uRect", QVector4D(x, y, w, h));

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);  // EBO 索引绘制

    m_vao.release();
    m_prog->release();
}
```

`uSwizzleBGRA` 在 `onScreenFrame` 里按帧格式设置(见下一步)。

---

## 第 6 步:`uploadFrame()` — QVideoFrame → 复用 GL 纹理

这是方案 C 的关键桥接。`QVideoFrame` 在 Windows 上可能是 GPU 纹理(D3D11)也可能是 CPU 帧,**统一走 `toImage` + CPU 上传** 是最稳、可移植的做法(零拷贝 GPU 互操作另见末尾"优化")。

**复用版**:纹理对象只在首帧(或尺寸变化时)用 `glTexImage2D` 创建/重新分配,之后每帧只用 `glTexSubImage2D` 更新像素,不再 `glGenTextures`/`glDeleteTextures`。

```cpp
// 把一帧上传到复用纹理 tex。cachedSize 记录该纹理当前已分配的尺寸。
// 首帧或尺寸变化时重新分配；否则只更新像素。返回 false 表示该帧无效（跳过）。
bool GlCompositor::uploadFrame(GLuint &tex, QSize &cachedSize, const QVideoFrame &f) {
    QImage img = f.toImage();                 // 拿到 CPU 像素(必要时含 GPU 回读)
    if (img.isNull()) return false;

    // 统一成 RGBA8888，UV 与着色器约定一致，且不必再 swizzle
    img = img.convertToFormat(QImage::Format_RGBA8888);

    // 首帧:创建纹理对象并设置采样参数(只做一次)
    if (tex == 0) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, tex);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (img.size() != cachedSize) {
        // 尺寸变了(含首帧):重新分配存储
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width(), img.height(),
                     0, GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
        cachedSize = img.size();
    } else {
        // 尺寸不变:只更新像素，省去重新分配
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, img.width(), img.height(),
                        GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
    }
    return true;
}
```

> 用了 `Format_RGBA8888` 后,着色器里 `uSwizzleBGRA` 恒为 0(不交换)。保留这个 uniform 是为了将来你想省掉 `convertToFormat`、直接传 `Format_ARGB32`(BGRA)纹理时能用 GPU 做 swizzle。
>
> 纹理在 `~GlCompositor()` 里统一 `glDeleteTextures` 释放(记得先 `makeCurrent`)。桌面尺寸基本固定,所以绝大多数帧都走 `glTexSubImage2D` 这条快路径。

---

## 第 7 步:`onScreenFrame()` — 合成 + 回读

桌面帧驱动:绑 FBO → 画桌面铺满 → 画相机到右下角 → `glReadPixels` 回读成 `QImage` → 发信号。

```cpp
void GlCompositor::onScreenFrame(const QVideoFrame &screenFrame) {
    if (!m_ctx->makeCurrent(m_surface)) return;

    // 上传到复用纹理(首帧分配，之后只更新像素)
    bool hasScreen = uploadFrame(m_texScreen, m_texScreenSize, screenFrame);
    bool hasCamera = (m_overlay && m_lastCamera.isValid())
                       && uploadFrame(m_texCamera, m_texCameraSize, m_lastCamera);

    m_fbo->bind();
    glViewport(0, 0, m_outputSize.width(), m_outputSize.height());
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    m_prog->bind();
    m_prog->setUniformValue("uSwizzleBGRA", 0);
    m_prog->release();

    // 1) 桌面铺满整个 NDC: 左下(-1,-1)，宽高 2x2
    if (hasScreen) drawQuad(m_texScreen, -1.f, -1.f, 2.f, 2.f);

    // 2) 相机小窗放右下角，把像素尺寸换算成 NDC
    if (hasCamera) {
        float W = m_outputSize.width(), H = m_outputSize.height();
        float w = m_camSize.width(),  h = m_camSize.height();
        // 像素左下角坐标(右下角，留 margin)
        float px = W - w - m_margin;
        float py = m_margin;                 // GL 原点在左下，所以底部留白即右下
        // 像素 → NDC
        float ndcX = px / W * 2.f - 1.f;
        float ndcY = py / H * 2.f - 1.f;
        float ndcW = w / W * 2.f;
        float ndcH = h / H * 2.f;
        drawQuad(m_texCamera, ndcX, ndcY, ndcW, ndcH);
    }

    glFinish();   // 确保渲染完成再回读

    // 3) 回读:QOpenGLFramebufferObject 自带便捷 toImage()
    QImage out = m_fbo->toImage();   // 返回 ARGB32(已处理翻转)
    m_fbo->release();

    // 纹理是复用的，不在此删除(见 ~GlCompositor)

    m_ctx->doneCurrent();

    if (!out.isNull())
        emit frameReady(out.convertToFormat(QImage::Format_ARGB32));
}
```

> `QOpenGLFramebufferObject::toImage()` 已经帮你处理了 GL 上下颠倒的问题,直接得到正向的 `QImage`。它内部就是 `glReadPixels`。
>
> **格式对齐:** 这里输出 `Format_ARGB32`(=小端 BGRA),正好对上方案文档里 `FFmpegStreamer` 的 `-pixel_format bgra`。不用改推流器。

---

## 第 8 步:接入 `DesktopCapturer`

和 CPU/QRhi 版完全一样的接法 —— 把 `GlCompositor` 换掉 `FrameCompositor` 即可,上层接口不变。

```cpp
// DesktopCapturer 构造里:
m_compositor = new GlCompositor(this);
m_screenSession->setVideoSink(m_compositor->screenSink());
m_cameraManager->setVideoSink(m_compositor->cameraSink());

connect(m_compositor, &GlCompositor::frameReady, this,
        [this](const QImage &frame) {
    emit previewFrame(frame);
    if (m_streamer->isPushing())
        m_streamer->writeFrame(frame);
});

// startPush 用合成器输出尺寸:
void startPush() { m_streamer->startPush(m_compositor->outputSize(), 25); }
void setCameraOverlay(bool on) { m_compositor->setOverlay(on); }
```

`FFmpegStreamer`(stdin 管道纯编码器)、`CameraManager::setVideoSink`、`mainwindow` 的 QLabel 预览,**全部沿用 CPU 版方案文档里的代码,一字不改**。方案 C 只替换了"合成"这一环。

---

## 性能优化清单(从易到难)

1. **复用纹理(已采用)。** 上面第 6 步 `uploadFrame` 已用复用纹理:首帧 `glTexImage2D` 分配,后续帧 `glTexSubImage2D` 只更新像素,省掉每帧 new/delete。这是最基本的优化,文档代码已含。
2. **PBO 异步回读。** `glReadPixels` + 立即用 `QImage` 是同步阻塞。改用 Pixel Buffer Object(PBO)双缓冲:本帧发起回读,下一帧再取上一帧结果,GPU 不空等。这是 OpenGL 回读提速的标准手法。
3. **避免 `toImage()` 回读上传往返。** 当前 `uploadFrame` 走的是 `QVideoFrame::toImage()`(可能 GPU→CPU)再 `glTexSubImage2D`(CPU→GPU)。Windows 上若 `handleType()==RhiTextureHandle`,理论上能用 `WGL_NV_DX_interop2` 把 D3D11 纹理直接共享给 GL,零拷贝 —— 但代码复杂、依赖驱动扩展,**不建议初版做**。
4. **降分辨率 / 调滤波。** 输出从 1080p 降到 720p,或相机缩放用 `GL_NEAREST`。

> 现实建议:第 1 项(复用纹理)文档代码已经做了。第 2 项(PBO)在 1080p@25fps 下通常不必要,真卡了再上。

---

## 三个方案怎么选

| | CPU 版 | 方案 A(QRhi) | 方案 C(原生 OpenGL) |
|---|---|---|---|
| 合成位置 | CPU(QPainter) | GPU | GPU |
| 上手难度 | 低 | 中(QRhi 模型 + qsb) | 中(经典 GL,资料多) |
| 着色器灵活度 | 无 | 高 | 高,GLSL 直接写 |
| 跨平台 | 最好 | 最好 | 桌面好,移动端是 GLES |
| 适用 | 1080p 单画中画 | 多源/特效/要跨后端 | 想完全掌控 GL 管线 |

**坦白讲:** 方案 C 和方案 A 性能基本一个档次(瓶颈都在最后的 GPU→CPU 回读)。选 C 的理由通常是"团队熟 OpenGL、想直接写 GLSL、不想碰 QRhi 的 qsb 预编译流程";选 A 的理由是"要跨 D3D/VK/Metal 后端、和 Qt 多媒体集成更顺"。

如果只是 1080p 画中画,**CPU 版就够,别上 GPU**。GPU 方案的复杂度只有在分辨率高、源多、或要实时特效时才回本。

## 常见坑

1. **黑屏/全黑帧** — 多半是上下文没 `makeCurrent`,或在错误线程调 GL。确认 `videoFrameChanged` 和合成在同一线程,且每次渲染前 `makeCurrent`、用完 `doneCurrent`。
2. **画面上下颠倒** — GL 纹理原点在左下。本文档在顶点着色器翻 V(`1.0 - aPos.y`)+ FBO `toImage()` 内部再处理了一次,**别两边都翻**导致翻回去。调试时先只信任 `m_fbo->toImage()` 的结果。
3. **颜色 R/B 互换(偏蓝/偏红)** — 纹理上传格式和着色器 swizzle 没对齐。本文档统一用 `Format_RGBA8888` 上传 + 不 swizzle,输出再转 `ARGB32`,是一致的;若你改用 `Format_ARGB32` 上传就要把 `uSwizzleBGRA` 置 1。
4. **首帧或切设备时崩** — `toImage()` 可能返回空,代码已 `isNull()` 判空,务必保留。
5. **`QOffscreenSurface` 创建失败** — 确保在主线程、`QGuiApplication` 已就绪后再建合成器(放 MainWindow 构造里即可)。

