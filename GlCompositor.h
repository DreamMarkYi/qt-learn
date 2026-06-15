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

class GlCompositor : public QObject, protected QOpenGLFunctions_3_3_Core
{
    Q_OBJECT;
public:
    explicit GlCompositor(QObject *parent = nullptr) : QObject(parent) {
        QSurfaceFormat fmt;
        fmt.setRenderableType(QSurfaceFormat::OpenGL);//指定渲染类型
        fmt.setVersion(3, 3);//指定OpenGL的格式
        fmt.setProfile(QSurfaceFormat::CoreProfile);//设置openGL的模式

        m_ctx = new QOpenGLContext(this);//向内存申请了一块空间，实例化了一个 Qt 的上下文管理对象
        m_ctx->setFormat(fmt);//设置格式
        m_ctx->create();//创建，我的理解就是分配了一块内存

        if (!m_ctx->create()) {
            qFatal("错误：无法创建需要的 OpenGL 上下文！请检查显卡驱动。");
        }

        // 检查显卡最终实际给你的版本是不是你想要的（有时候系统会拒绝或降级）
        qDebug() << "成功创建 OpenGL 上下文，实际版本为:"
                 << m_ctx->format().version();

        //创建一个离屏对象
        m_surface = new QOffscreenSurface(nullptr, this);
        m_surface->setFormat(fmt);
        m_surface->create();

        m_screenSink = new QVideoSink(this);
        m_cameraSink = new QVideoSink(this);


        //QVideoSink接收原始视频帧
        connect(m_screenSink, &QVideoSink::videoFrameChanged,
                this, &GlCompositor::onScreenFrame);
        connect(m_cameraSink, &QVideoSink::videoFrameChanged, this,
                [this](const QVideoFrame &f) { m_lastCamera = f; });

        m_ctx->makeCurrent(m_surface);//绑定
        initializeOpenGLFunctions();
        buildGl();
        m_ctx->doneCurrent();//初始化之后先释放，后续再调用？
    }

    QVideoSink* screenSink() const { return m_screenSink; }
    QVideoSink* cameraSink() const { return m_cameraSink; }
    QSize outputSize() const { return m_outputSize; }
    void setOverlay(bool on) { m_overlay = on; }
    bool overlay() const { return m_overlay; }

//释放渲染完毕信号
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
