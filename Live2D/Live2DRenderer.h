#ifndef LIVE2D_RENDERER_H
#define LIVE2D_RENDERER_H

#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QMatrix4x4>
#include <QString>
#include <vector>
#include <cstdint>

class Live2DModel;

// 自定义 Live2D OpenGL 渲染器（替代 CubismRenderer_OpenGLES2::DrawModel）。
//
// 原理：CubismModel 在 Update() 后，已把每个 drawable(ArtMesh) 的顶点、UV、
// 不透明度、绘制顺序算好放在 Core 内存里。本类每帧把这些读出来：
//   按 RenderOrder 排序 → 逐 drawable：绑纹理 → 设混合模式 → 传 MVP/颜色
//   uniform → glDrawElements。
//
// 第二阶段：自己实现裁剪蒙版(mask)，不调用 Cubism 的 ClippingManager。
//   做法：把每个被裁剪 drawable 的「蒙版 drawable」先画到一张离屏 FBO 的
//   某个颜色通道(R/G/B/A) 里(只写 alpha 覆盖度)，正式绘制该 drawable 时再
//   采样这张 FBO，用覆盖度裁掉蒙版外的像素。蒙版 FBO 用与主绘制相同的 MVP
//   和分辨率，故屏幕坐标 ↔ 蒙版纹理坐标天然对齐，无需包围盒换算。
//
// 所有方法必须在 GL 上下文 current 的线程调用。
class Live2DRenderer : protected QOpenGLFunctions_3_3_Core
{
public:
    Live2DRenderer() = default;
    ~Live2DRenderer();

    // 建 program、为每个 drawable 建 VBO/IBO、上传贴图、分析蒙版分组。GL 上下文须 current。
    bool initialize(const Live2DModel* model);

    // 用给定的 MVP（列主序 16 floats）把模型画到当前绑定的 framebuffer。
    // fbW/fbH 为当前 framebuffer 的像素尺寸，用于（重）建离屏蒙版纹理。
    void draw(const float* mvp16, int fbW, int fbH);

    // 删除全部 GL 资源。GL 上下文须 current。
    void release();

    // 请求在下一帧蒙版渲染完成后，把离屏蒙版纹理导出为 PNG 到 dir 目录。
    // 一次请求导出一帧（每张离屏纹理一张图：mask_0.png、mask_1.png …）。
    // 仅当模型使用蒙版时才有输出。GL 上下文须 current（实际导出发生在 draw 内）。
    void requestMaskDump(const QString& dir);

private:
    // 每个 drawable 一套缓冲
    struct DrawableGL {
        GLuint vao    = 0;   // 记录该 drawable 的顶点布局
        GLuint vboPos = 0;   // 顶点位置 (vec2)，每帧可更新（本例静止，仅传一次）
        GLuint vboUv  = 0;   // UV (vec2)，静态
        GLuint ibo    = 0;   // 索引
        GLsizei indexCount = 0;
        int     textureIndex = 0;
    };

    void uploadTextures(const Live2DModel* model);
    void buildBuffers(const Live2DModel* model);

    // —— 蒙版(mask) ——
    // 一个 ClipContext = 一组「蒙版 drawable 集合相同」的被裁剪 drawable。
    // 每个 context 占用某张离屏纹理的某个颜色通道(0=R,1=G,2=B,3=A)。
    struct ClipContext {
        std::vector<int> maskDrawables;     // 充当蒙版的 drawable 下标
        std::vector<int> clippedDrawables;  // 被这组蒙版裁剪的 drawable 下标
        int textureIndex = 0;               // 用第几张离屏纹理
        int channel      = 0;               // 写入/采样哪个通道
        bool inverted    = false;           // 反向蒙版（蒙版外可见）
    };

    void buildClipContexts(const Live2DModel* model);   // init 时分析蒙版分组
    void ensureMaskTextures(int fbW, int fbH);          // 按 fb 尺寸建/重建离屏纹理+FBO
    void renderMasks(const QMatrix4x4& mvp);            // 每帧：把蒙版画进离屏纹理通道
    void dumpMaskTextures(const QString& dir);          // 把离屏蒙版纹理读回存为 PNG
    void releaseMaskResources();

    const Live2DModel*       _model = nullptr;
    QOpenGLShaderProgram     _program;       // 主绘制
    QOpenGLShaderProgram     _maskProgram;   // 蒙版预渲染（只输出覆盖度 alpha）
    std::vector<DrawableGL>  _drawables;
    std::vector<GLuint>      _textures;   // GL 纹理 id，索引 = TextureIndex
    bool                     _initialized = false;

    // 蒙版分组与离屏资源
    std::vector<ClipContext> _clipContexts;
    std::vector<int>         _drawableToClip;  // drawable -> _clipContexts 下标，-1 表示不裁剪
    std::vector<GLuint>      _maskTextures;    // 离屏蒙版纹理（每张 4 通道，最多容 4 个 context）
    GLuint                   _maskFbo = 0;
    int                      _maskW = 0, _maskH = 0;
    bool                     _maskDumpPending = false;   // 下一帧导出 PNG
    QString                  _maskDumpDir;               // 导出目录

    // uniform / attribute 位置（主绘制）
    int _locMvp = -1, _locBaseColor = -1, _locMultiply = -1, _locScreen = -1, _locTex = -1;
    int _locUseMask = -1, _locMaskTex = -1, _locMaskChannel = -1, _locMaskInverted = -1;
    // uniform 位置（蒙版预渲染）
    int _locMaskMvp = -1;
    int _attrPos = -1, _attrUv = -1;
};

#endif // LIVE2D_RENDERER_H
