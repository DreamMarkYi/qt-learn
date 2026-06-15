#ifndef GLCOMPOSITOR_H
#define GLCOMPOSITOR_H

#include <QObject>
#include <QVideoSink>
#include <QVideoFrame>
#include <QImage>
#include <QSize>
#include <QOpenGLContext>
#include <QOffscreenSurface>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLFramebufferObject>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QDebug>
#include <QThread>

class GlCompositor : public QObject, protected QOpenGLFunctions_3_3_Core
{
    Q_OBJECT;
public:
    explicit GlCompositor(QObject *parent = nullptr) : QObject(parent) {
        m_screenSink = new QVideoSink(this);
        m_cameraSink = new QVideoSink(this);

        // sink 的信号槽是 Qt::AutoConnection：发送方(媒体线程)与接收方
        // (本对象所在的渲染线程)不同线程时，自动变成队列连接，线程安全。
        connect(m_screenSink, &QVideoSink::videoFrameChanged,
                this, &GlCompositor::onScreenFrame);
        connect(m_cameraSink, &QVideoSink::videoFrameChanged, this,
                [this](const QVideoFrame &f) { m_lastCamera = f; });
    }

    QVideoSink* screenSink() const { return m_screenSink; }
    QVideoSink* cameraSink() const { return m_cameraSink; }
    QSize outputSize() const { return m_outputSize; }
    void setOverlay(bool on) { m_overlay = on; }
    bool overlay() const { return m_overlay; }

public slots:
    // 在渲染线程里执行：创建上下文 + 离屏面 + 编译着色器。
    // 由 QThread::started 触发，保证 GL 上下文归属工作线程。
    void init();

    // 在渲染线程里执行：上下文 current 状态下释放所有 GL 资源。
    // 线程退出前用 BlockingQueuedConnection 调一次。
    void cleanup();

signals:
    void frameReady(const QImage &frame);

private:
    QOpenGLContext*  m_ctx = nullptr;
    QOffscreenSurface* m_surface = nullptr;
    QOpenGLShaderProgram* m_prog = nullptr;
    QOpenGLFramebufferObject* m_fbo = nullptr;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer m_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_ebo{QOpenGLBuffer::IndexBuffer};
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

    // —— 下面两个步骤的实现见后文 ——
    void buildGl();
    void onScreenFrame(const QVideoFrame &screenFrame);
     bool uploadFrame(GLuint &tex, QSize &cachedSize, const QVideoFrame &f);
    void drawQuad(GLuint tex, float x, float y, float w, float h); // NDC 矩形
};

#endif // GLCOMPOSITOR_H
