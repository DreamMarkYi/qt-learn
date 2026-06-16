#ifndef LIVE2D_RENDERER_H
#define LIVE2D_RENDERER_H

#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
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
// 第一阶段不做裁剪蒙版(mask)，被裁剪的 drawable 直接整块画（可能有边缘溢出，
// 但模型主体可见）。mask 留待第二阶段补 ClippingManager。
//
// 所有方法必须在 GL 上下文 current 的线程调用。
class Live2DRenderer : protected QOpenGLFunctions_3_3_Core
{
public:
    Live2DRenderer() = default;
    ~Live2DRenderer();

    // 建 program、为每个 drawable 建 VBO/IBO、上传贴图。GL 上下文须 current。
    bool initialize(const Live2DModel* model);

    // 用给定的 MVP（列主序 16 floats）把模型画到当前绑定的 framebuffer。
    void draw(const float* mvp16);

    // 删除全部 GL 资源。GL 上下文须 current。
    void release();

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

    const Live2DModel*       _model = nullptr;
    QOpenGLShaderProgram     _program;
    std::vector<DrawableGL>  _drawables;
    std::vector<GLuint>      _textures;   // GL 纹理 id，索引 = TextureIndex
    bool                     _initialized = false;

    // uniform / attribute 位置
    int _locMvp = -1, _locBaseColor = -1, _locMultiply = -1, _locScreen = -1, _locTex = -1;
    int _attrPos = -1, _attrUv = -1;
};

#endif // LIVE2D_RENDERER_H
