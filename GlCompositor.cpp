#include "GlCompositor.h"

void GlCompositor::buildGl()
{
    QOpenGLFramebufferObjectFormat ff;
    ff.setInternalTextureFormat(GL_RGBA8);
    m_fbo = new QOpenGLFramebufferObject(m_outputSize, ff);

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

    m_ctx->doneCurrent();

    if (!out.isNull())
        emit frameReady(out.convertToFormat(QImage::Format_ARGB32));
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
    m_ctx->doneCurrent();
}

// ── 在渲染线程里释放所有 GL 资源（上下文 current 状态下执行）──
void GlCompositor::cleanup()
{
    if (!m_ctx) return;
    m_ctx->makeCurrent(m_surface);

    if (m_texScreen) { glDeleteTextures(1, &m_texScreen); m_texScreen = 0; }
    if (m_texCamera) { glDeleteTextures(1, &m_texCamera); m_texCamera = 0; }

    m_vao.destroy();
    m_vbo.destroy();
    m_ebo.destroy();
    delete m_prog;  m_prog = nullptr;
    delete m_fbo;   m_fbo  = nullptr;

    m_ctx->doneCurrent();

    delete m_surface; m_surface = nullptr;
    delete m_ctx;     m_ctx     = nullptr;
    qDebug() << "🧹 [GL] 渲染资源已在工作线程释放";
}
