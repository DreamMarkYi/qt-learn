#include "GlCompositor.h"
#include <cstring>   // memcpy（PBO 回读逐行翻转拷贝用）

void GlCompositor::buildGl()
{
    QOpenGLFramebufferObjectFormat ff;
    ff.setInternalTextureFormat(GL_RGBA8);
    m_fbo = new QOpenGLFramebufferObject(m_outputSize, ff);

    // 双 PBO：异步回读缓冲。GL_STREAM_READ 提示驱动这是「GPU 写、CPU 读、用一次」。
    const int pboBytes = m_outputSize.width() * m_outputSize.height() * 4;
    glGenBuffers(2, m_pbo);
    for (int i = 0; i < 2; ++i) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, pboBytes, nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

       // ---- 从单独文件加载着色器 ----
    m_prog = new QOpenGLShaderProgram(this);
    if (!m_prog->addShaderFromSourceFile(QOpenGLShader::Vertex,
                                         ":/Shader/Shader/composite.vert"))
        qWarning() << "顶点着色器编译失败:" << m_prog->log();
    if (!m_prog->addShaderFromSourceFile(QOpenGLShader::Fragment,
                                         ":/Shader/Shader/composite.frag"))
        qWarning() << "片元着色器编译失败:" << m_prog->log();

    m_prog->bindAttributeLocation("aPos", 0);
    if (!m_prog->link())
        qWarning() << "着色器链接失败:" << m_prog->log();

    // ---- 相机 NV12 解码程序：共用顶点着色器，片元换成 YUV→RGB ----
    m_progNv12 = new QOpenGLShaderProgram(this);
    if (!m_progNv12->addShaderFromSourceFile(QOpenGLShader::Vertex,
                                             ":/Shader/Shader/composite.vert"))
        qWarning() << "NV12 顶点着色器编译失败:" << m_progNv12->log();
    if (!m_progNv12->addShaderFromSourceFile(QOpenGLShader::Fragment,
                                             ":/Shader/Shader/composite_nv12.frag"))
        qWarning() << "NV12 片元着色器编译失败:" << m_progNv12->log();
    m_progNv12->bindAttributeLocation("aPos", 0);
    if (!m_progNv12->link())
        qWarning() << "NV12 着色器链接失败:" << m_progNv12->log();

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
    m_prog->release();
}

// x,y 为矩形左下角 NDC 坐标，w,h 为 NDC 宽高。
// flipV: 采样时是否翻 V（FBO 离屏纹理自下而上 → true）。
// premultiplied: 纹理是否预乘 alpha。blend: 是否开混合（叠加层用）。
void GlCompositor::drawQuad(GLuint tex, float x, float y, float w, float h,
                            bool flipV, bool premultiplied, bool blend) {
    m_prog->bind();
    m_vao.bind();

    if (blend) {
        glEnable(GL_BLEND);
        // 预乘 alpha：src 已乘过 alpha，用 ONE；非预乘用 SRC_ALPHA。
        if (premultiplied) glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        else               glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    m_prog->setUniformValue("uTex", 0);
    m_prog->setUniformValue("uRect", QVector4D(x, y, w, h));
    m_prog->setUniformValue("uFlipV", flipV ? 1 : 0);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);  // EBO 索引绘制

    if (blend) glDisable(GL_BLEND);   // 还原，桌面层默认不混合
    m_vao.release();
    m_prog->release();
}

// 把一层的离屏纹理按 placement 矩形换算成 NDC，叠到主 FBO 上。
void GlCompositor::composeLayer(ICompositorLayer* layer, GLuint tex) {
    if (tex == 0) return;
    const QRect r = layer->placement(m_outputSize);
    const float W = m_outputSize.width(), H = m_outputSize.height();

    // QRect 左上原点 → GL 左下原点：底部留白 = H - top - height
    const float px = r.x();
    const float py = H - r.y() - r.height();
    const float ndcX = px / W * 2.f - 1.f;
    const float ndcY = py / H * 2.f - 1.f;
    const float ndcW = r.width()  / W * 2.f;
    const float ndcH = r.height() / H * 2.f;

    drawQuad(tex, ndcX, ndcY, ndcW, ndcH,
             layer->bottomUp(), layer->premultiplied(), /*blend*/ true);
}

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

// 上传单个像素平面（NV12 的 Y 或 UV）。data 可能有行末 padding，故用
// GL_UNPACK_ROW_LENGTH 按真实行宽读取。pixelBytes：每像素字节数（Y=1, UV=2）。
void GlCompositor::uploadPlane(GLuint &tex, QSize &cachedSize, GLint internalFmt,
                              GLenum extFmt, int w, int h, const uchar *data,
                              int bytesPerLine, int pixelBytes) {
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
    glPixelStorei(GL_UNPACK_ROW_LENGTH, bytesPerLine / pixelBytes);   // 按真实行宽（texel 数）

    if (QSize(w, h) != cachedSize) {
        glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, w, h, 0,
                     extFmt, GL_UNSIGNED_BYTE, data);
        cachedSize = QSize(w, h);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        extFmt, GL_UNSIGNED_BYTE, data);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);   // 还原，免得影响其它上传
}

// 相机 NV12 帧：map() 借用底层缓冲，把 Y、UV 两平面直传到各自纹理。
// 零 QImage 分配、零 CPU 色彩转换；YUV→RGB 推迟到着色器里做。
bool GlCompositor::uploadCameraNv12(const QVideoFrame &f) {
    QVideoFrame frame = f;   // 浅拷贝（引用计数），map 需要非 const
    if (!frame.map(QVideoFrame::ReadOnly)) {
        qWarning() << "[NV12] map 失败";
        return false;
    }
    const int w = frame.width();
    const int h = frame.height();
    bool ok = (frame.planeCount() >= 2) && frame.bits(0) && frame.bits(1);
    if (ok) {
        // Y 平面：全分辨率，单通道 R8
        uploadPlane(m_texCamY, m_texCamYSize, GL_R8, GL_RED,
                    w, h, frame.bits(0), frame.bytesPerLine(0), 1);
        // UV 平面：半分辨率，双通道 RG8（一个 texel = 一对 UV，2 字节）
        uploadPlane(m_texCamUV, m_texCamUVSize, GL_RG8, GL_RG,
                    w / 2, h / 2, frame.bits(1), frame.bytesPerLine(1), 2);
    }
    frame.unmap();
    return ok;
}

// 用 NV12 程序把 Y/UV 解码后贴到目标 NDC 矩形。相机帧自上而下，故 uFlipV=0
// （顶点着色器默认翻转，与 drawQuad 的相机路径一致）。不混合。
void GlCompositor::drawCameraNv12(float x, float y, float w, float h,
                                  int colorSpace, int fullRange) {
    m_progNv12->bind();
    m_vao.bind();
    glDisable(GL_BLEND);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texCamY);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_texCamUV);

    m_progNv12->setUniformValue("uTexY", 0);
    m_progNv12->setUniformValue("uTexUV", 1);
    m_progNv12->setUniformValue("uRect", QVector4D(x, y, w, h));
    m_progNv12->setUniformValue("uFlipV", 0);
    m_progNv12->setUniformValue("uColorSpace", colorSpace);
    m_progNv12->setUniformValue("uFullRange", fullRange);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

    glActiveTexture(GL_TEXTURE0);   // 还原活动纹理单元
    m_vao.release();
    m_progNv12->release();
}

// 固定 60fps 定时器驱动的一帧。取桌面/相机最新帧，推进动画，合成，回读推流。
// 跑在渲染线程（定时器在 init 里于本线程创建），GL 上下文同线程，安全。
void GlCompositor::renderTick() {
    if (!m_ctx || !m_ctx->makeCurrent(m_surface)) return;

    // —— 真实时间 dt：量出距上一帧实际过了多少秒，喂给动画推进 ——
    // 首帧没有“上一帧”，用 kSeedDt 兜底；并夹 kMaxDt 上限，避免卡顿/断点后
    // 量出超大间隔导致动画瞬移。Σdt 恒等于真实流逝时间，故动画速度始终正确。
    float dt;
    if (!m_frameClock.isValid()) {       // 理论上 init 已 start，这里兜底
        m_frameClock.start();
        dt = kSeedDt;
    } else {
        const qint64 ns = m_frameClock.nsecsElapsed();
        m_frameClock.restart();
        dt = static_cast<float>(ns) / 1.0e9f;
    }
    if (dt > kMaxDt) dt = kMaxDt;
    if (dt < 0.0f)   dt = 0.0f;

    // 按需上传：只有 sink 报告了新帧（dirty）才做昂贵的 toImage/转换/上传，
    // 否则直接复用纹理里已有的上一帧。相机 ~30fps、桌面静止时省掉大量重复转换。
    // 桌面、相机分开计时，便于看出相机上传到底贵不贵。
    timed("up.screen", [&]{
        if (m_screenDirty && m_lastScreen.isValid()) {
            m_screenReady = uploadFrame(m_texScreen, m_texScreenSize, m_lastScreen);
            m_screenDirty = false;
        }
    });
    timed("up.camera", [&]{
        if (m_cameraDirty && m_lastCamera.isValid()) {
            if (m_lastCamera.pixelFormat() == QVideoFrameFormat::Format_NV12) {
                // NV12：map() 直传 Y/UV，着色器里转 RGB（零分配、零 CPU 转换）
                const QVideoFrameFormat ff = m_lastCamera.surfaceFormat();
                m_camColorSpace = (ff.colorSpace() == QVideoFrameFormat::ColorSpace_BT709) ? 1 : 0;
                m_camFullRange  = (ff.colorRange() == QVideoFrameFormat::ColorRange_Full)  ? 1 : 0;
                m_cameraIsNv12  = true;
                m_cameraReady   = uploadCameraNv12(m_lastCamera);
            } else {
                // 其它格式回退到原 RGBA 路径
                m_cameraIsNv12 = false;
                m_cameraReady  = uploadFrame(m_texCamera, m_texCameraSize, m_lastCamera);
            }
            m_cameraDirty = false;
        }
    });
    const bool hasScreen = m_screenReady;
    const bool hasCamera = m_overlay && m_cameraReady;

    // 基础合成：清屏 + 桌面铺满 + 相机小窗
    timed("base", [&]{
        m_fbo->bind();
        glViewport(0, 0, m_outputSize.width(), m_outputSize.height());
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        m_prog->bind();
        m_prog->setUniformValue("uSwizzleBGRA", 0);
        m_prog->release();

        // 1) 桌面铺满整个 NDC: 左下(-1,-1)，宽高 2x2。不透明，不混合，默认翻 V。
        if (hasScreen) drawQuad(m_texScreen, -1.f, -1.f, 2.f, 2.f);

        // 2) 相机小窗放右下角，把像素尺寸换算成 NDC
        //if (hasCamera) {
        //    float W = m_outputSize.width(), H = m_outputSize.height();
        //    float w = m_camSize.width(),  h = m_camSize.height();
        //    float px = W - w - m_margin;
        //    float py = m_margin;             // GL 原点在左下，所以底部留白即右下
        //    float ndcX = px / W * 2.f - 1.f;
        //    float ndcY = py / H * 2.f - 1.f;
        //    float ndcW = w / W * 2.f;
        //    float ndcH = h / H * 2.f;
        //    if (m_cameraIsNv12)
        //        drawCameraNv12(ndcX, ndcY, ndcW, ndcH, m_camColorSpace, m_camFullRange);
        //    else
        //        drawQuad(m_texCamera, ndcX, ndcY, ndcW, ndcH);
        //}
    });

    // 3) 合成层（Live2D 人物等）：每层先渲染到自己的离屏纹理，再贴回主 FBO。
    //    用真实 dt 推进动作时间轴：节拍掉到 60fps 以下时动画只变卡、不变慢。
    //    layer->render 会改动 FBO/GL 状态，画完重绑主 FBO。
    timed("layers", [&]{
        for (auto& layer : m_layers) {
            GLuint layerTex = layer->render(dt);
            m_fbo->bind();
            glViewport(0, 0, m_outputSize.width(), m_outputSize.height());
            composeLayer(layer.get(), layerTex);
        }
    });

    // —— 异步回读（双 PBO 乒乓）——
    // 不再 glFinish + 同步 toImage。本帧把像素异步拷进 m_pbo[readIdx]
    // （glReadPixels 立即返回，GPU 后台搬运）；同时 map 上一帧已写好的
    // m_pbo[mapIdx] 取出像素——已隔一整帧，map 几乎不阻塞。这样回读与 GPU
    // 渲染重叠，去掉了把 CPU/GPU 串成一条线的 glFinish 硬等。
    QImage out;
    timed("readback", [&]{
        const int readIdx = m_pboIndex;
        const int mapIdx  = 1 - m_pboIndex;
        const int W = m_outputSize.width(), H = m_outputSize.height();

        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[readIdx]);
        // 直接读成 BGRA：其内存布局与 QImage::Format_ARGB32（小端 0xAARRGGBB）一致，
        // 省掉原来 convertToFormat(ARGB32) 那次整图 CPU 遍历。
        glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);

        if (m_pboPrimed) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[mapIdx]);
            const uchar* src = static_cast<const uchar*>(
                glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY));
            if (src) {
                // 深拷贝出来（unmap 后映射内存即失效，且要跨线程发给 UI/推流）。
                // OpenGL 像素自下而上、QImage 自上而下 → 逐行翻转拷贝，顺带完成翻转。
                out = QImage(W, H, QImage::Format_ARGB32);
                const int stride = W * 4;
                for (int row = 0; row < H; ++row)
                    memcpy(out.scanLine(row), src + size_t(H - 1 - row) * stride, stride);
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        m_pboIndex  = mapIdx;   // 乒乓交换：下一帧读写互换
        m_pboPrimed = true;
    });

    m_fbo->release();
    m_ctx->doneCurrent();

    if (!out.isNull())
        emit frameReady(out);

    m_prof.endFrame(kProfileReportFrames);   // 满 N 帧汇总打印各段耗时
}

void GlCompositor::init()
{
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);

    // 上下文不设 parent：它属于当前线程，由 cleanup() 显式删除。
    m_ctx = new QOpenGLContext();
    m_ctx->setFormat(fmt);
    if (!m_ctx->create()) {
        qFatal("错误：无法创建需要的 OpenGL 上下文！请检查显卡驱动。");
    }
    qDebug() << "成功创建 OpenGL 上下文，实际版本为:"
             << m_ctx->format().version()
             << " 线程:" << QThread::currentThread();

    m_surface = new QOffscreenSurface();   // 同样不设 parent
    m_surface->setFormat(fmt);
    m_surface->create();

    m_ctx->makeCurrent(m_surface);
    initializeOpenGLFunctions();
    buildGl();

    // 初始化所有合成层（与本上下文同线程，故各层 GL 资源都建在这个上下文里）。
    // it是一个迭代器
    for (auto it = m_layers.begin(); it != m_layers.end(); ) {
        if ((*it)->init()) {
            ++it;
        } else {
            qWarning() << "[GlCompositor] 某合成层初始化失败，已移除";
            it = m_layers.erase(it);
        }
    }

    m_ctx->doneCurrent();

    // 起固定 60fps 渲染节拍。定时器在本线程(渲染线程)创建，timeout 在本线程事件循环
    // 触发，故 renderTick 与 GL 上下文同线程。与采集帧率解耦，动画不再随采集冻结。
    m_renderTimer = new QTimer(this);
    connect(m_renderTimer, &QTimer::timeout, this, &GlCompositor::renderTick);
    m_renderTimer->start(kRenderIntervalMs);

    // 启动真实时间时钟；首帧 renderTick 会据此算出 dt。
    m_frameClock.start();
}
void GlCompositor::cleanup()
{
    if (!m_ctx) return;

    // 先停掉渲染定时器，避免 cleanup 期间还触发 renderTick。
    if (m_renderTimer) { m_renderTimer->stop(); delete m_renderTimer; m_renderTimer = nullptr; }

    m_ctx->makeCurrent(m_surface);

    // 先释放合成层（它们的 GL 资源建在本上下文里，须在 doneCurrent 前清掉）。
    for (auto& layer : m_layers) layer->release();
    m_layers.clear();

    if (m_texScreen) { glDeleteTextures(1, &m_texScreen); m_texScreen = 0; }
    if (m_texCamera) { glDeleteTextures(1, &m_texCamera); m_texCamera = 0; }
    if (m_texCamY)   { glDeleteTextures(1, &m_texCamY);   m_texCamY   = 0; }
    if (m_texCamUV)  { glDeleteTextures(1, &m_texCamUV);  m_texCamUV  = 0; }
    m_texCamYSize = m_texCamUVSize = QSize();

    if (m_pbo[0] || m_pbo[1]) { glDeleteBuffers(2, m_pbo); m_pbo[0] = m_pbo[1] = 0; }
    m_pboIndex = 0;
    m_pboPrimed = false;

    m_frameClock.invalidate();   // 复用时由 init() 重新 start

    m_screenDirty = m_cameraDirty = false;
    m_screenReady = m_cameraReady = false;
    m_cameraIsNv12 = false;

    m_vao.destroy();
    m_vbo.destroy();
    m_ebo.destroy();
    delete m_prog;     m_prog     = nullptr;
    delete m_progNv12; m_progNv12 = nullptr;
    delete m_fbo;   m_fbo  = nullptr;

    m_ctx->doneCurrent();

    delete m_surface; m_surface = nullptr;
    delete m_ctx;     m_ctx     = nullptr;
    qDebug() << "🧹 [GL] 渲染资源已在工作线程释放";
}
