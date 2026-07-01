#ifndef L2D_RENDERER_H
#define L2D_RENDERER_H

#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QMatrix4x4>
#include <QString>
#include <vector>
#include <cstdint>

class L2dModel;

// 自定义 Live2D OpenGL 渲染器（替代 CubismRenderer_OpenGLES2::DrawModel）。
//
// 每帧把 CubismModel 在 Update() 后算好的 drawable 顶点/UV/不透明度/绘制顺序读出，
// 按 RenderOrder 排序逐个绘制；并自实现裁剪蒙版：把蒙版 drawable 先画进离屏 FBO 的
// R/G/B/A 通道，正式绘制被裁剪 drawable 时采样该通道做覆盖度裁剪。
//
// 关键特性（方案 B 依赖）：draw() 开头用 glGetIntegerv 保存「调用时绑定的 FBO/视口」，
// 蒙版预渲染切到自己的 _maskFbo，画完后恢复回保存值——所以本渲染器**默认就画到
// 调用方当前绑定的 FBO**。L2dLayer 只要先 bind 自己的离屏 FBO 再调 draw 即可。
//
// 所有方法必须在 GL 上下文 current 的线程调用。从 Live2D/Live2DRenderer 移植。
class L2dRenderer : protected QOpenGLFunctions_3_3_Core
{
public:
    L2dRenderer() = default;
    ~L2dRenderer();

    bool initialize(const L2dModel* model);
    void draw(const float* mvp16, int fbW, int fbH);   // 画到当前绑定的 FBO
    void release();

private:
    struct DrawableGL {
        GLuint vao = 0, vboPos = 0, vboUv = 0, ibo = 0;
        GLsizei indexCount = 0;
        int     textureIndex = 0;
    };

    void uploadTextures(const L2dModel* model);
    void buildBuffers(const L2dModel* model);

    struct ClipContext {
        std::vector<int> maskDrawables;
        std::vector<int> clippedDrawables;
        int  textureIndex = 0;
        int  channel      = 0;
        bool inverted     = false;
    };

    void buildClipContexts(const L2dModel* model);
    void ensureMaskTextures(int fbW, int fbH);
    void renderMasks(const QMatrix4x4& mvp);
    void releaseMaskResources();

    const L2dModel*       _model = nullptr;
    QOpenGLShaderProgram  _program;
    QOpenGLShaderProgram  _maskProgram;
    std::vector<DrawableGL> _drawables;
    std::vector<GLuint>     _textures;
    bool                    _initialized = false;

    std::vector<ClipContext> _clipContexts;
    std::vector<int>         _drawableToClip;
    std::vector<GLuint>      _maskTextures;
    GLuint                   _maskFbo = 0;
    int                      _maskW = 0, _maskH = 0;

    int _locMvp = -1, _locBaseColor = -1, _locMultiply = -1, _locScreen = -1, _locTex = -1;
    int _locUseMask = -1, _locMaskTex = -1, _locMaskChannel = -1, _locMaskInverted = -1;
    int _locMaskMvp = -1;
    int _attrPos = -1, _attrUv = -1;
};

#endif // L2D_RENDERER_H
