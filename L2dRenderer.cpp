#include "L2dRenderer.h"
#include "L2dModel.h"

#include <Model/CubismModel.hpp>
#include <Live2DCubismCore.h>

#include <QImage>
#include <QDebug>
#include <algorithm>
#include <numeric>

using namespace Live2D::Cubism::Framework;
namespace Core = Live2D::Cubism::Core;

// ── 着色器（与 Live2D/Live2DRenderer 一致）──────────────────────────
static const char* kVert = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
uniform mat4 uMvp;
out vec2 vUv;
out vec4 vClipPos;
void main() {
    vUv = aUv;
    gl_Position = uMvp * vec4(aPos, 0.0, 1.0);
    vClipPos = gl_Position;
}
)";

static const char* kFrag = R"(
#version 330 core
in vec2 vUv;
in vec4 vClipPos;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec4 uBaseColor;
uniform vec4 uMultiply;
uniform vec4 uScreen;
uniform int       uUseMask;
uniform sampler2D uMaskTex;
uniform vec4      uMaskChannel;
uniform int       uMaskInverted;
void main() {
    vec2 uv = vec2(vUv.x, 1.0 - vUv.y);
    vec4 tex = texture(uTex, uv);
    tex.rgb *= uMultiply.rgb;
    tex.rgb = tex.rgb + uScreen.rgb - tex.rgb * uScreen.rgb;
    vec4 col = tex * uBaseColor;
    col.rgb *= col.a;        // 预乘
    if (uUseMask == 1) {
        vec2 mUv = vClipPos.xy / vClipPos.w * 0.5 + 0.5;
        float coverage = dot(texture(uMaskTex, mUv), uMaskChannel);
        if (uMaskInverted == 1) coverage = 1.0 - coverage;
        col *= coverage;
    }
    FragColor = col;
}
)";

static const char* kMaskVert = R"(
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
static const char* kMaskFrag = R"(
#version 330 core
in vec2 vUv;
out vec4 FragColor;
uniform sampler2D uTex;
void main() {
    vec2 uv = vec2(vUv.x, 1.0 - vUv.y);
    float a = texture(uTex, uv).a;
    FragColor = vec4(a, a, a, a);
}
)";

L2dRenderer::~L2dRenderer()
{
    release();
}

bool L2dRenderer::initialize(const L2dModel* model)
{
    initializeOpenGLFunctions();
    _model = model;

    if (!_program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVert) ||
        !_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag) ||
        !_program.link()) {
        qWarning() << "[L2dRenderer] shader 编译/链接失败:" << _program.log();
        return false;
    }
    if (!_maskProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, kMaskVert) ||
        !_maskProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, kMaskFrag) ||
        !_maskProgram.link()) {
        qWarning() << "[L2dRenderer] mask shader 编译/链接失败:" << _maskProgram.log();
        return false;
    }

    _locMvp          = _program.uniformLocation("uMvp");
    _locBaseColor    = _program.uniformLocation("uBaseColor");
    _locMultiply     = _program.uniformLocation("uMultiply");
    _locScreen       = _program.uniformLocation("uScreen");
    _locTex          = _program.uniformLocation("uTex");
    _locUseMask      = _program.uniformLocation("uUseMask");
    _locMaskTex      = _program.uniformLocation("uMaskTex");
    _locMaskChannel  = _program.uniformLocation("uMaskChannel");
    _locMaskInverted = _program.uniformLocation("uMaskInverted");
    _locMaskMvp      = _maskProgram.uniformLocation("uMvp");
    _attrPos = 0;
    _attrUv  = 1;

    uploadTextures(model);
    buildBuffers(model);
    buildClipContexts(model);
    _initialized = true;
    return true;
}

void L2dRenderer::uploadTextures(const L2dModel* model)
{
    const QStringList& paths = model->texturePaths();
    _textures.resize(paths.size(), 0);

    for (int i = 0; i < paths.size(); ++i) {
        QImage img(paths[i]);
        if (img.isNull()) {
            qWarning() << "[L2dRenderer] 贴图加载失败:" << paths[i];
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

void L2dRenderer::buildBuffers(const L2dModel* model)
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

        glGenVertexArrays(1, &d.vao);
        glBindVertexArray(d.vao);

        glGenBuffers(1, &d.vboPos);
        glBindBuffer(GL_ARRAY_BUFFER, d.vboPos);
        glBufferData(GL_ARRAY_BUFFER, sizeof(Core::csmVector2) * vtxCount,
                     pos, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(_attrPos);
        glVertexAttribPointer(_attrPos, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

        glGenBuffers(1, &d.vboUv);
        glBindBuffer(GL_ARRAY_BUFFER, d.vboUv);
        glBufferData(GL_ARRAY_BUFFER, sizeof(Core::csmVector2) * vtxCount,
                     uv, GL_STATIC_DRAW);
        glEnableVertexAttribArray(_attrUv);
        glVertexAttribPointer(_attrUv, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

        glGenBuffers(1, &d.ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(csmUint16) * idxCount,
                     idx, GL_STATIC_DRAW);

        glBindVertexArray(0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void L2dRenderer::buildClipContexts(const L2dModel* model)
{
    CubismModel* m = model->model();
    const csmInt32 count = m->GetDrawableCount();
    _drawableToClip.assign(count, -1);
    _clipContexts.clear();

    if (!m->IsUsingMasking()) {
        qInfo() << "[L2dRenderer] 模型不使用蒙版";
        return;
    }

    const csmInt32*  maskCounts = m->GetDrawableMaskCounts();
    const csmInt32** masks      = m->GetDrawableMasks();

    for (csmInt32 i = 0; i < count; ++i) {
        const csmInt32 mc = maskCounts[i];
        if (mc <= 0) continue;

        std::vector<int> set(masks[i], masks[i] + mc);
        std::sort(set.begin(), set.end());

        int ctxIdx = -1;
        for (size_t c = 0; c < _clipContexts.size(); ++c) {
            if (_clipContexts[c].maskDrawables == set) { ctxIdx = (int)c; break; }
        }
        if (ctxIdx < 0) {
            ClipContext ctx;
            ctx.maskDrawables = set;
            ctx.inverted = m->GetDrawableInvertedMask(i);
            const int global = (int)_clipContexts.size();
            ctx.textureIndex = global / 4;
            ctx.channel      = global % 4;
            ctxIdx = global;
            _clipContexts.push_back(std::move(ctx));
        }
        _clipContexts[ctxIdx].clippedDrawables.push_back(i);
        _drawableToClip[i] = ctxIdx;
    }

    const int texCount = ((int)_clipContexts.size() + 3) / 4;
    _maskTextures.assign(texCount > 0 ? texCount : 0, 0);
    qInfo() << "[L2dRenderer] 蒙版分组: contexts =" << _clipContexts.size()
            << "离屏纹理张数 =" << texCount;
}

void L2dRenderer::ensureMaskTextures(int fbW, int fbH)
{
    if (_clipContexts.empty()) return;
    if (_maskFbo && fbW == _maskW && fbH == _maskH) return;

    releaseMaskResources();
    _maskW = fbW > 0 ? fbW : 1;
    _maskH = fbH > 0 ? fbH : 1;

    const int texCount = (int)_maskTextures.size();
    for (int i = 0; i < texCount; ++i) {
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, _maskW, _maskH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        _maskTextures[i] = tex;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &_maskFbo);
}

void L2dRenderer::renderMasks(const QMatrix4x4& mvp)
{
    CubismModel* m = _model->model();

    glBindFramebuffer(GL_FRAMEBUFFER, _maskFbo);
    glViewport(0, 0, _maskW, _maskH);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    _maskProgram.bind();
    _maskProgram.setUniformValue(_locMaskMvp, mvp);
    _maskProgram.setUniformValue(_maskProgram.uniformLocation("uTex"), 0);
    glActiveTexture(GL_TEXTURE0);

    glEnable(GL_BLEND);
    glBlendEquation(GL_MAX);
    glBlendFunc(GL_ONE, GL_ONE);

    for (int t = 0; t < (int)_maskTextures.size(); ++t) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, _maskTextures[t], 0);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);

        for (const ClipContext& ctx : _clipContexts) {
            if (ctx.textureIndex != t) continue;
            glColorMask(ctx.channel == 0, ctx.channel == 1,
                        ctx.channel == 2, ctx.channel == 3);
            for (int di : ctx.maskDrawables) {
                const DrawableGL& d = _drawables[di];
                if (d.indexCount <= 0) continue;
                if (d.textureIndex >= 0 && d.textureIndex < (int)_textures.size())
                    glBindTexture(GL_TEXTURE_2D, _textures[d.textureIndex]);

                const csmInt32 vtxCount = m->GetDrawableVertexCount(di);
                const Core::csmVector2* positions = m->GetDrawableVertexPositions(di);
                glBindBuffer(GL_ARRAY_BUFFER, d.vboPos);
                glBufferSubData(GL_ARRAY_BUFFER, 0,
                    sizeof(Core::csmVector2) * vtxCount, positions);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glBindVertexArray(d.vao);
                glDrawElements(GL_TRIANGLES, d.indexCount, GL_UNSIGNED_SHORT, nullptr);
            }
        }
    }

    glBindVertexArray(0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlendEquation(GL_FUNC_ADD);
    _maskProgram.release();
}

void L2dRenderer::draw(const float* mvp16, int fbW, int fbH)
{
    if (!_model || _drawables.empty()) return;
    CubismModel* m = _model->model();
    Core::csmModel* core = m->GetModel();
    const csmInt32 count = m->GetDrawableCount();

    QMatrix4x4 mvp = QMatrix4x4(mvp16).transposed();

    // —— 蒙版预渲染：保存调用方当前 FBO/视口，画完恢复 ——
    const bool useMasking = !_clipContexts.empty();
    if (useMasking) {
        GLint prevFbo = 0, prevViewport[4] = {0, 0, fbW, fbH};
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
        glGetIntegerv(GL_VIEWPORT, prevViewport);
        ensureMaskTextures(fbW, fbH);
        renderMasks(mvp);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    }

    const csmInt32* renderOrders = Core::csmGetRenderOrders(core);
    std::vector<int> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](int a, int b){ return renderOrders[a] < renderOrders[b]; });

    const Core::csmFlags* constFlags = Core::csmGetDrawableConstantFlags(core);
    const Core::csmFlags* dynFlags   = Core::csmGetDrawableDynamicFlags(core);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);

    _program.bind();
    _program.setUniformValue(_locTex, 0);
    _program.setUniformValue(_locMaskTex, 1);

    for (int di : order) {
        const DrawableGL& d = _drawables[di];
        if (!(dynFlags[di] & Core::csmIsVisible)) continue;
        const float opacity = m->GetDrawableOpacity(di);
        if (opacity <= 0.0f) continue;
        if (d.indexCount <= 0) continue;

        const bool additive = (constFlags[di] & Core::csmBlendAdditive)       != 0;
        const bool multiply = (constFlags[di] & Core::csmBlendMultiplicative) != 0;
        if (additive)      glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
        else if (multiply) glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        else               glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        _program.setUniformValue(_locMvp, mvp);
        _program.setUniformValue(_locBaseColor, QVector4D(1, 1, 1, opacity));
        const Core::csmVector4 mul = m->GetDrawableMultiplyColor(di);
        const Core::csmVector4 scr = m->GetDrawableScreenColor(di);
        _program.setUniformValue(_locMultiply, QVector4D(mul.X, mul.Y, mul.Z, mul.W));
        _program.setUniformValue(_locScreen,   QVector4D(scr.X, scr.Y, scr.Z, scr.W));

        const int clip = di < (int)_drawableToClip.size() ? _drawableToClip[di] : -1;
        if (clip >= 0) {
            const ClipContext& ctx = _clipContexts[clip];
            _program.setUniformValue(_locUseMask, 1);
            _program.setUniformValue(_locMaskInverted, ctx.inverted ? 1 : 0);
            _program.setUniformValue(_locMaskChannel, QVector4D(
                ctx.channel == 0 ? 1.f : 0.f, ctx.channel == 1 ? 1.f : 0.f,
                ctx.channel == 2 ? 1.f : 0.f, ctx.channel == 3 ? 1.f : 0.f));
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, _maskTextures[ctx.textureIndex]);
        } else {
            _program.setUniformValue(_locUseMask, 0);
        }

        glActiveTexture(GL_TEXTURE0);
        if (d.textureIndex >= 0 && d.textureIndex < (int)_textures.size())
            glBindTexture(GL_TEXTURE_2D, _textures[d.textureIndex]);

        const csmInt32 vtxCount = m->GetDrawableVertexCount(di);
        const Core::csmVector2* positions = m->GetDrawableVertexPositions(di);
        glBindBuffer(GL_ARRAY_BUFFER, d.vboPos);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
            sizeof(Core::csmVector2) * vtxCount, positions);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(d.vao);
        glDrawElements(GL_TRIANGLES, d.indexCount, GL_UNSIGNED_SHORT, nullptr);
    }

    glBindVertexArray(0);
    _program.release();
}

void L2dRenderer::releaseMaskResources()
{
    if (_maskFbo) { glDeleteFramebuffers(1, &_maskFbo); _maskFbo = 0; }
    for (GLuint& t : _maskTextures) {
        if (t) { glDeleteTextures(1, &t); t = 0; }
    }
    _maskW = _maskH = 0;
}

void L2dRenderer::release()
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
    releaseMaskResources();
    _clipContexts.clear();
    _drawableToClip.clear();
}
