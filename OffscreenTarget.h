#ifndef OFFSCREEN_TARGET_H
#define OFFSCREEN_TARGET_H

#include <QOpenGLFunctions_3_3_Core>
#include <QSize>

// 一张「离屏渲染目标」：FBO + 单张 RGBA8 颜色纹理。方案 B 里 Live2D 先画到它，
// 再由 GlCompositor 把它的纹理当 quad 贴回主输出。做成独立小类是为了复用——
// 任何「先渲染到纹理再合成」的层都能直接拿来用。
//
// 不持有 GL 函数指针：所有 GL 操作都接收一个 QOpenGLFunctions_3_3_Core& 由调用方传入，
// 保证操作发生在调用方的上下文/线程里。
class OffscreenTarget
{
public:
    OffscreenTarget() = default;
    ~OffscreenTarget() = default;   // GL 资源须显式 release()，析构不碰 GL（可能无上下文）

    GLuint texture() const { return _tex; }
    GLuint fbo()     const { return _fbo; }
    QSize  size()    const { return _size; }

    // 确保 FBO/纹理存在且尺寸为 size；尺寸变化才重建。返回是否可用。
    bool ensure(QOpenGLFunctions_3_3_Core& f, const QSize& size)
    {
        const int w = size.width()  > 0 ? size.width()  : 1;
        const int h = size.height() > 0 ? size.height() : 1;
        if (_fbo && _tex && _size.width() == w && _size.height() == h)
            return true;

        release(f);
        _size = QSize(w, h);

        f.glGenTextures(1, &_tex);
        f.glBindTexture(GL_TEXTURE_2D, _tex);
        f.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                       GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        f.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        f.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        f.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        f.glGenFramebuffers(1, &_fbo);
        f.glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
        f.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                 GL_TEXTURE_2D, _tex, 0);

        const GLenum st = f.glCheckFramebufferStatus(GL_FRAMEBUFFER);
        f.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            qWarning() << "[OffscreenTarget] FBO 不完整, status =" << st;
            release(f);
            return false;
        }
        return true;
    }

    // 绑定本 FBO 并设好视口；清成全透明（预乘 alpha 的底色）。
    void bindAndClear(QOpenGLFunctions_3_3_Core& f)
    {
        f.glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
        f.glViewport(0, 0, _size.width(), _size.height());
        f.glClearColor(0.f, 0.f, 0.f, 0.f);
        f.glClear(GL_COLOR_BUFFER_BIT);
    }

    void release(QOpenGLFunctions_3_3_Core& f)
    {
        if (_fbo) { f.glDeleteFramebuffers(1, &_fbo); _fbo = 0; }
        if (_tex) { f.glDeleteTextures(1, &_tex);     _tex = 0; }
        _size = QSize();
    }

private:
    GLuint _fbo = 0;
    GLuint _tex = 0;
    QSize  _size;
};

#endif // OFFSCREEN_TARGET_H
