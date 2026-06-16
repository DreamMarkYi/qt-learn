#include "Live2DRenderer.h"
#include "Live2DModel.h"

#include <Model/CubismModel.hpp>
#include <Live2DCubismCore.h>   // Core C API：动态/常量标志、绘制顺序

#include <QImage>
#include <QDebug>
#include <algorithm>
#include <numeric>

using namespace Live2D::Cubism::Framework;
namespace Core = Live2D::Cubism::Core;   // Core 的枚举/类型/C 接口都在此命名空间

// ── 着色器 ──────────────────────────────────────────────────────────
// 顶点：MVP * (pos, 0, 1)，UV 直传。
static const char* kVert = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
uniform mat4 uMvp;
out vec2 vUv;
void main() {
    vUv = aUv;
    gl_Position = uMvp * vec4(aPos, 0.0, 1.0);
}
)";

// 片元：乘色 → 屏幕色 → 叠 opacity → 预乘 alpha 输出。
// 与 Cubism NormalShader 一致（贴图非预乘，输出预乘）。
// UV.y 翻转：Cubism UV 原点在左上，OpenGL 纹理原点在左下，需 1-v。
static const char* kFrag = R"(
#version 330 core
in vec2 vUv;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec4 uBaseColor;   // rgb=1, a=drawable opacity
uniform vec4 uMultiply;    // GetDrawableMultiplyColor
uniform vec4 uScreen;      // GetDrawableScreenColor
void main() {
    vec2 uv = vec2(vUv.x, 1.0 - vUv.y);
    vec4 tex = texture(uTex, uv);
    tex.rgb *= uMultiply.rgb;
    tex.rgb = tex.rgb + uScreen.rgb - tex.rgb * uScreen.rgb;
    vec4 col = tex * uBaseColor;
    col.rgb *= col.a;        // 预乘
    FragColor = col;
}
)";

Live2DRenderer::~Live2DRenderer()
{
    release();
}

bool Live2DRenderer::initialize(const Live2DModel* model)
{
    initializeOpenGLFunctions();
    _model = model;

    if (!_program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVert) ||
        !_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag) ||
        !_program.link()) {
        qWarning() << "[Live2DRenderer] shader 编译/链接失败:" << _program.log();
        return false;
    }

    _locMvp       = _program.uniformLocation("uMvp");
    _locBaseColor = _program.uniformLocation("uBaseColor");
    _locMultiply  = _program.uniformLocation("uMultiply");
    _locScreen    = _program.uniformLocation("uScreen");
    _locTex       = _program.uniformLocation("uTex");
    _attrPos      = 0;   // layout(location=0)
    _attrUv       = 1;   // layout(location=1)

    uploadTextures(model);
    buildBuffers(model);
    _initialized = true;
    return true;
}

// PNG → GL 纹理。用 QImage 解码（原生支持中文路径），转 RGBA8888 直传。
// 注意：不翻 V。Cubism 导出的 UV 与「图像左上为原点」一致，QImage 行序也是
// 自上而下，二者吻合；若发现上下颠倒再在此 mirror。
void Live2DRenderer::uploadTextures(const Live2DModel* model)
{
    const QStringList& paths = model->texturePaths();
    _textures.resize(paths.size(), 0);

    for (int i = 0; i < paths.size(); ++i) {
        QImage img(paths[i]);
        if (img.isNull()) {
            qWarning() << "[Live2DRenderer] 贴图加载失败:" << paths[i];
            continue;
        }
        img = img.convertToFormat(QImage::Format_RGBA8888);

        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width(), img.height(), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        _textures[i] = tex;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

// 为每个 drawable 建 VAO + VBO(pos/uv) + IBO，把布局录入 VAO。
void Live2DRenderer::buildBuffers(const Live2DModel* model)
{
    CubismModel* m = model->model();
    const csmInt32 count = m->GetDrawableCount();
    _drawables.resize(count);

    for (csmInt32 i = 0; i < count; ++i) {
        DrawableGL& d = _drawables[i];
        d.textureIndex = m->GetDrawableTextureIndex(i);

        const csmInt32 vtxCount = m->GetDrawableVertexCount(i);
        const Core::csmVector2* pos = m->GetDrawableVertexPositions(i);
        const Core::csmVector2* uv  = m->GetDrawableVertexUvs(i);
        const csmInt32 idxCount     = m->GetDrawableVertexIndexCount(i);
        const csmUint16* idx        = m->GetDrawableVertexIndices(i);
        d.indexCount = idxCount;

        // 建 VAO，后续所有绑定都录入它
        glGenVertexArrays(1, &d.vao);
        glBindVertexArray(d.vao);

        // 位置 VBO → attribute 0
        glGenBuffers(1, &d.vboPos);
        glBindBuffer(GL_ARRAY_BUFFER, d.vboPos);
        glBufferData(GL_ARRAY_BUFFER, sizeof(Core::csmVector2) * vtxCount,
                     pos, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(_attrPos);
        glVertexAttribPointer(_attrPos, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

        // UV VBO → attribute 1
        glGenBuffers(1, &d.vboUv);
        glBindBuffer(GL_ARRAY_BUFFER, d.vboUv);
        glBufferData(GL_ARRAY_BUFFER, sizeof(Core::csmVector2) * vtxCount,
                     uv, GL_STATIC_DRAW);
        glEnableVertexAttribArray(_attrUv);
        glVertexAttribPointer(_attrUv, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

        // IBO（绑到 GL_ELEMENT_ARRAY_BUFFER 也会被 VAO 记住）
        glGenBuffers(1, &d.ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(csmUint16) * idxCount,
                     idx, GL_STATIC_DRAW);

        glBindVertexArray(0);   // 解绑，结束录制
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Live2DRenderer::draw(const float* mvp16)
{
    if (!_model || _drawables.empty()) return;
    CubismModel* m = _model->model();
    Core::csmModel* core = m->GetModel();
    const csmInt32 count = m->GetDrawableCount();

    // —— 按 RenderOrder 排序，得到本帧绘制次序 ——
    // renderOrders[i] = drawable i 的层级；值小的先画(在底层)。
    const csmInt32* renderOrders = Core::csmGetRenderOrders(core);
    std::vector<int> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](int a, int b){ return renderOrders[a] < renderOrders[b]; });

    const Core::csmFlags* constFlags = Core::csmGetDrawableConstantFlags(core);
    const Core::csmFlags* dynFlags   = Core::csmGetDrawableDynamicFlags(core);

    // —— 公共 GL 状态：Live2D 用画家算法，关深度、开混合 ——
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);          // drawable 可能正反面，统一不剔除
    glEnable(GL_BLEND);

    _program.bind();
    _program.setUniformValue(_locTex, 0);
    glActiveTexture(GL_TEXTURE0);

    // CubismMatrix44 是列主序；QMatrix4x4(const float*) 按行主序读，故需转置。
    QMatrix4x4 mvp = QMatrix4x4(mvp16).transposed();

    for (int di : order) {
        const DrawableGL& d = _drawables[di];

        // 不可见或全透明则跳过
        if (!(dynFlags[di] & Core::csmIsVisible)) continue;
        const float opacity = m->GetDrawableOpacity(di);
        if (opacity <= 0.0f) continue;
        if (d.indexCount <= 0) continue;

        // 混合模式
        const bool additive = (constFlags[di] & Core::csmBlendAdditive)       != 0;
        const bool multiply = (constFlags[di] & Core::csmBlendMultiplicative) != 0;
        if (additive)      glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
        else if (multiply) glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        else               glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        // uniform
        _program.setUniformValue(_locMvp, mvp);
        _program.setUniformValue(_locBaseColor, QVector4D(1, 1, 1, opacity));
        const Core::csmVector4 mul = m->GetDrawableMultiplyColor(di);
        const Core::csmVector4 scr = m->GetDrawableScreenColor(di);
        _program.setUniformValue(_locMultiply, QVector4D(mul.X, mul.Y, mul.Z, mul.W));
        _program.setUniformValue(_locScreen,   QVector4D(scr.X, scr.Y, scr.Z, scr.W));

        // 贴图
        if (d.textureIndex >= 0 && d.textureIndex < (int)_textures.size())
            glBindTexture(GL_TEXTURE_2D, _textures[d.textureIndex]);

        const csmInt32 vtxCount = m->GetDrawableVertexCount(di);
        const Core::csmVector2* positions = m->GetDrawableVertexPositions(di);

        glBindBuffer(GL_ARRAY_BUFFER, d.vboPos);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
            sizeof(Core::csmVector2) * vtxCount, positions);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        // VAO 已记住顶点布局，直接绑定后绘制
        glBindVertexArray(d.vao);
        glDrawElements(GL_TRIANGLES, d.indexCount, GL_UNSIGNED_SHORT, nullptr);
    }

    glBindVertexArray(0);
    _program.release();
}

void Live2DRenderer::release()
{
    if (!_initialized) return;
    _initialized = false;

    for (auto& d : _drawables) {
        if (d.vao)    glDeleteVertexArrays(1, &d.vao);
        if (d.vboPos) glDeleteBuffers(1, &d.vboPos);
        if (d.vboUv)  glDeleteBuffers(1, &d.vboUv);
        if (d.ibo)    glDeleteBuffers(1, &d.ibo);
    }
    _drawables.clear();
    for (GLuint t : _textures) if (t) glDeleteTextures(1, &t);
    _textures.clear();
}
