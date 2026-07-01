#ifndef GLCOMPOSITOR_H
#define GLCOMPOSITOR_H

#include <QObject>
#include <QVideoSink>
#include <QVideoFrame>
#include <QVideoFrameFormat>
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
#include <QElapsedTimer>
#include <QTimer>
#include <vector>
#include <memory>

#include "ICompositorLayer.h"
#include "FrameProfiler.h"

class GlCompositor : public QObject, protected QOpenGLFunctions_3_3_Core
{
    Q_OBJECT;
public:
    explicit GlCompositor(QObject *parent = nullptr) : QObject(parent) {
        m_screenSink = new QVideoSink(this);
        m_cameraSink = new QVideoSink(this);

        // sink 的信号槽是 Qt::AutoConnection：发送方(媒体线程)与接收方
        // (本对象所在的渲染线程)不同线程时，自动变成队列连接，线程安全。
        // 桌面/相机都只「存最新帧」，不立即触发渲染——渲染由固定 60fps 定时器驱动，
        // 与采集帧率彻底解耦（窗口拖到副屏、主屏静止时动画也不会冻结）。
        connect(m_screenSink, &QVideoSink::videoFrameChanged, this,
                [this](const QVideoFrame &f) {
            // 诊断：仅在格式/尺寸变化时打印一次，避免逐帧刷屏
            static QVideoFrameFormat::PixelFormat lastFmt = QVideoFrameFormat::Format_Invalid;
            static QSize lastSize;
            if (f.pixelFormat() != lastFmt || f.size() != lastSize) {
                lastFmt = f.pixelFormat(); lastSize = f.size();
                qDebug() << "[桌面帧] 格式 =" << f.pixelFormat() << " 尺寸 =" << f.size();
            }
            m_lastScreen = f; m_screenDirty = true;
        });
        connect(m_cameraSink, &QVideoSink::videoFrameChanged, this,
                [this](const QVideoFrame &f) {
            static QVideoFrameFormat::PixelFormat lastFmt = QVideoFrameFormat::Format_Invalid;
            static QSize lastSize;
            if (f.pixelFormat() != lastFmt || f.size() != lastSize) {
                lastFmt = f.pixelFormat(); lastSize = f.size();
                qDebug() << "[相机帧] 格式 =" << f.pixelFormat() << " 尺寸 =" << f.size();
            }
            m_lastCamera = f; m_cameraDirty = true;
        });
    }

    QVideoSink* screenSink() const { return m_screenSink; }
    QVideoSink* cameraSink() const { return m_cameraSink; }
    QSize outputSize() const { return m_outputSize; }
    void setOverlay(bool on) { m_overlay = on; }
    bool overlay() const { return m_overlay; }

    // 渲染分步计时开关（诊断用，默认开）。诊断完建议关掉——计时模式会插 glFinish
    // 把流水线串行化，拖慢真实帧率。线程安全性：只在渲染线程读，调用方注意。
    void setProfiling(bool on) { m_profiling = on; }

    // 注册一个合成层（如 Live2D 人物层）。需在 init() 之前调用——init() 里会逐个
    // 调它们的 init() 建 GL 资源。GlCompositor 接管生命周期（cleanup 时 release）。
    // 后续要加新层（字幕/特效/第二个形象）只需 new 一个 ICompositorLayer 实现传进来。
    void addLayer(std::unique_ptr<ICompositorLayer> layer) {
        m_layers.push_back(std::move(layer));
    }

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
    QOpenGLShaderProgram* m_progNv12 = nullptr;   // 相机 NV12 → RGB 解码用
    QOpenGLFramebufferObject* m_fbo = nullptr;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer m_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_ebo{QOpenGLBuffer::IndexBuffer};
    QVideoSink* m_screenSink = nullptr;
    QVideoSink* m_cameraSink = nullptr;
    QVideoFrame m_lastScreen;     // 桌面最新帧（只存，渲染由定时器驱动）
    QVideoFrame m_lastCamera;     // 相机最新帧

    // 按需上传：sink 收到新帧时置 dirty=true；renderTick 只在 dirty 时才做
    // 昂贵的 toImage()+convertToFormat()+上传，否则复用已在纹理里的上一帧。
    // 相机 ~30fps、桌面静止无新帧时，由此省掉大量重复的整图 CPU 转换。
    // sink lambda 与 renderTick 同在渲染线程串行执行，无需加锁。
    bool m_screenDirty = false;   // 有新桌面帧待上传
    bool m_cameraDirty = false;   // 有新相机帧待上传
    bool m_screenReady = false;   // m_texScreen 已有有效内容（可参与合成）
    bool m_cameraReady = false;   // m_texCamera 已有有效内容

    bool m_overlay = true;
    const QSize m_outputSize{1920, 1080};
    const QSize m_camSize{480, 360};
    const int   m_margin = 20;

    // 长期复用的两个纹理(避免每帧 new/delete)，及其已分配尺寸
    GLuint m_texScreen = 0;
    GLuint m_texCamera = 0;      // 相机非 NV12 时的 RGBA 回退纹理
    QSize  m_texScreenSize;
    QSize  m_texCameraSize;

    // 相机 NV12 路径：Y(R8) + UV(RG8) 两张平面纹理，map() 直传、着色器里转 RGB。
    GLuint m_texCamY  = 0;
    GLuint m_texCamUV = 0;
    QSize  m_texCamYSize;
    QSize  m_texCamUVSize;
    bool   m_cameraIsNv12 = false;   // 本帧相机是否走 NV12 路径
    int    m_camColorSpace = 0;      // 0=BT.601, 1=BT.709（取自帧元信息）
    int    m_camFullRange  = 0;      // 0=limited, 1=full

    // 双 PBO 乒乓：异步回读用。本帧 glReadPixels 把像素丢进一个 PBO（立即返回，
    // GPU 后台搬运），同时 map 另一个「上一帧已写好」的 PBO 取像素——已隔一整帧，
    // 几乎不阻塞。借此去掉 glFinish 硬等，让回读与 GPU 渲染重叠。代价：预览/推流晚 1 帧。
    GLuint m_pbo[2] = {0, 0};
    int    m_pboIndex  = 0;       // 本帧写入哪个 PBO；另一个用于读取
    bool   m_pboPrimed = false;   // 首帧只写不读（另一个 PBO 还没数据）

    // 合成层（Live2D 等）。在主输出 FBO 上按各自 placement 叠加。
    std::vector<std::unique_ptr<ICompositorLayer>> m_layers;

    // 固定 60fps 渲染节拍：驱动「上传桌面/相机 + 推进动画 + 合成 + 回读推流」。
    // 在 init()（渲染线程）里创建，故 timeout 在渲染线程触发，与 GL 上下文同线程。
    QTimer* m_renderTimer = nullptr;
    static constexpr int   kRenderIntervalMs = 16;          // ≈60fps

    // 真实时间驱动动画：用 m_frameClock 量出相邻两 tick 的真实间隔当 dt，
    // 让动画进度跟着墙上时钟走——掉帧只会变卡、速度始终正确（不再随帧率慢放）。
    QElapsedTimer m_frameClock;
    static constexpr float kSeedDt   = 1.0f / 60.0f;  // 首帧没有“上一帧”，用它兜底
    static constexpr float kMaxDt    = 0.1f;          // dt 上限：卡顿后只小跳，不瞬移

    // —— 渲染分步计时（诊断）——
    bool m_profiling = true;
    FrameProfiler m_prof;
    static constexpr int kProfileReportFrames = 120;   // 每 120 帧汇总打印一次

    // 给 fn 计时并归入名为 name 的桶。计时模式下在 fn 之后插 glFinish，
    // 把该段排队的 GPU 命令执行完，才能把 GPU 耗时正确归到这一段（仅诊断，
    // 会拖慢真实帧率）。关闭计时时只是直接执行 fn，零额外开销。
    template <class F>
    void timed(const char* name, F&& fn) {
        if (!m_profiling) { fn(); return; }
        QElapsedTimer t; t.start();
        fn();
        glFinish();                       // 等本段 GPU 活干完再停表
        m_prof.mark(name, t.nsecsElapsed());
    }

    // —— 下面两个步骤的实现见后文 ——
    void buildGl();
    void renderTick();   // 定时器驱动的一帧：取最新帧 → 合成 → 回读 → frameReady
    bool uploadFrame(GLuint &tex, QSize &cachedSize, const QVideoFrame &f);
    // NV12 相机帧：map() 后把 Y / UV 两平面直传到各自纹理（零 QImage 分配、零 CPU 转换）。
    bool uploadCameraNv12(const QVideoFrame &f);
    // 上传单个像素平面到复用纹理（处理行对齐/padding）。pixelBytes：每像素字节数。
    void uploadPlane(GLuint &tex, QSize &cachedSize, GLint internalFmt, GLenum extFmt,
                     int w, int h, const uchar *data, int bytesPerLine, int pixelBytes);
    // 用 NV12 程序把 Y/UV 解码并贴到目标 NDC 矩形（相机小窗）。
    void drawCameraNv12(float x, float y, float w, float h, int colorSpace, int fullRange);
    // NDC 矩形绘制。flipV: 是否上下翻转采样（FBO 纹理自下而上时需 true）；
    // premultiplied: 纹理是否预乘 alpha（决定混合方程）；blend: 是否开启混合。
    void drawQuad(GLuint tex, float x, float y, float w, float h,
                  bool flipV = false, bool premultiplied = false, bool blend = false);
    // 把一个合成层的离屏纹理按其 placement 贴到主 FBO（处理 NDC 换算 + 混合）。
    void composeLayer(ICompositorLayer* layer, GLuint tex);
};

#endif // GLCOMPOSITOR_H
