#include "Live2DRenderer.h"
#include "Live2DModel.h"

#include <Model/CubismModel.hpp>
#include <Live2DCubismCore.h>   // Core C API：动态/常量标志、绘制顺序

#include <QImage>
#include <QDir>
#include <QDebug>
#include <algorithm>
#include <numeric>

using namespace Live2D::Cubism::Framework;
namespace Core = Live2D::Cubism::Core;   // Core 的枚举/类型/C 接口都在此命名空间

// ── 着色器 ──────────────────────────────────────────────────────────
// 顶点：MVP * (pos, 0, 1)，UV 直传。额外把裁剪空间坐标传给片元，
// 供蒙版采样用（蒙版纹理与主绘制同 MVP 同分辨率，故可直接用屏幕位置采样）。
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

// 片元：乘色 → 屏幕色 → 叠 opacity → 预乘 alpha 输出。
// 与 Cubism NormalShader 一致（贴图非预乘，输出预乘）。
// UV.y 翻转：Cubism UV 原点在左上，OpenGL 纹理原点在左下，需 1-v。
// 若该 drawable 被裁剪(uUseMask=1)：用裁剪空间坐标换算成 [0,1] 取蒙版覆盖度，
// 乘进最终(预乘)颜色；反向蒙版则取 1-coverage。
static const char* kFrag = R"(
#version 330 core
in vec2 vUv;
in vec4 vClipPos;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec4 uBaseColor;   // rgb=1, a=drawable opacity
uniform vec4 uMultiply;    // GetDrawableMultiplyColor
uniform vec4 uScreen;      // GetDrawableScreenColor
uniform int       uUseMask;       // 0=不裁剪 1=裁剪
uniform sampler2D uMaskTex;       // 离屏蒙版纹理
uniform vec4      uMaskChannel;   // one-hot 选通道，如 (1,0,0,0)=R
uniform int       uMaskInverted;  // 1=反向蒙版
void main() {
    vec2 uv = vec2(vUv.x, 1.0 - vUv.y);
    vec4 tex = texture(uTex, uv);
    tex.rgb *= uMultiply.rgb;
    tex.rgb = tex.rgb + uScreen.rgb - tex.rgb * uScreen.rgb;
    vec4 col = tex * uBaseColor;
    col.rgb *= col.a;        // 预乘

    if (uUseMask == 1) {
        // 裁剪空间 -> NDC -> [0,1] 纹理坐标
        vec2 mUv = vClipPos.xy / vClipPos.w * 0.5 + 0.5;
        float coverage = dot(texture(uMaskTex, mUv), uMaskChannel);
        if (uMaskInverted == 1) coverage = 1.0 - coverage;
        col *= coverage;     // 预乘色整体乘覆盖度
    }
    FragColor = col;
}
)";

// 蒙版预渲染着色器：把蒙版 drawable 的贴图 alpha 当作覆盖度输出。
// 只关心 alpha（覆盖与否），rgb 不重要；由 glColorMask 限定只写某个通道。
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
    // 四通道都输出 a；写哪个通道由 glColorMask 决定。
    FragColor = vec4(a, a, a, a);
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
    if (!_maskProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, kMaskVert) ||
        !_maskProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, kMaskFrag) ||
        !_maskProgram.link()) {
        qWarning() << "[Live2DRenderer] mask shader 编译/链接失败:" << _maskProgram.log();
        return false;
    }

    _locMvp       = _program.uniformLocation("uMvp");
    _locBaseColor = _program.uniformLocation("uBaseColor");
    _locMultiply  = _program.uniformLocation("uMultiply");
    _locScreen    = _program.uniformLocation("uScreen");
    _locTex       = _program.uniformLocation("uTex");
    _locUseMask      = _program.uniformLocation("uUseMask");
    _locMaskTex      = _program.uniformLocation("uMaskTex");
    _locMaskChannel  = _program.uniformLocation("uMaskChannel");
    _locMaskInverted = _program.uniformLocation("uMaskInverted");
    _locMaskMvp   = _maskProgram.uniformLocation("uMvp");
    _attrPos      = 0;   // layout(location=0)
    _attrUv       = 1;   // layout(location=1)

    uploadTextures(model);
    buildBuffers(model);
    buildClipContexts(model);
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

// ── 蒙版分组 ────────────────────────────────────────────────────────
// 遍历每个 drawable 的蒙版列表(GetDrawableMasks)。蒙版集合「相同」的被裁剪
// drawable 合并成一个 ClipContext，共用一个通道，避免通道浪费。
// 每张离屏纹理含 4 通道(RGBA)，可容 4 个 context；不够则再开一张纹理。
void Live2DRenderer::buildClipContexts(const Live2DModel* model)
{
    CubismModel* m = model->model();
    const csmInt32 count = m->GetDrawableCount();
    _drawableToClip.assign(count, -1);
    _clipContexts.clear();

    if (!m->IsUsingMasking()) {
        qInfo() << "[Live2DRenderer] 模型不使用蒙版";
        return;
    }

    const csmInt32*  maskCounts = m->GetDrawableMaskCounts();
    const csmInt32** masks      = m->GetDrawableMasks();

    for (csmInt32 i = 0; i < count; ++i) {
        const csmInt32 mc = maskCounts[i];
        if (mc <= 0) continue;   // 不被裁剪

        // 把该 drawable 的蒙版集合规范化（排序）后找已有 context
        std::vector<int> set(masks[i], masks[i] + mc);
        std::sort(set.begin(), set.end());

        int ctxIdx = -1;
        for (size_t c = 0; c < _clipContexts.size(); ++c) {
            if (_clipContexts[c].maskDrawables == set) { ctxIdx = (int)c; break; }
        }
        if (ctxIdx < 0) {
            ClipContext ctx; //创建一个空白的裁剪上下文
            ctx.maskDrawables = set;
            ctx.inverted = m->GetDrawableInvertedMask(i);
            const int global = (int)_clipContexts.size();
            ctx.textureIndex = global / 4;   // 每纹理 4 通道
            ctx.channel      = global % 4;
            ctxIdx = global;
            _clipContexts.push_back(std::move(ctx));
        }
        _clipContexts[ctxIdx].clippedDrawables.push_back(i);
        _drawableToClip[i] = ctxIdx;
    }

    const int texCount = ((int)_clipContexts.size() + 3) / 4;
    _maskTextures.assign(texCount > 0 ? texCount : 0, 0);
    qInfo() << "[Live2DRenderer] 蒙版分组: contexts =" << _clipContexts.size()
            << "离屏纹理张数 =" << texCount;
}

// 按当前 framebuffer 尺寸建/重建离屏蒙版纹理与 FBO。尺寸变化才重建。
void Live2DRenderer::ensureMaskTextures(int fbW, int fbH)
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

// 每帧蒙版预渲染：把每个 context 的蒙版 drawable 画进对应纹理的对应通道。
// 调用前需保存好原 FBO/视口，调用后由 draw() 负责恢复。
void Live2DRenderer::renderMasks(const QMatrix4x4& mvp)
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

    // 蒙版累加：同一通道里多个蒙版 drawable 取覆盖度并集 -> alpha 取较大者。
    glEnable(GL_BLEND);
    glBlendEquation(GL_MAX);
    glBlendFunc(GL_ONE, GL_ONE);

    for (int t = 0; t < (int)_maskTextures.size(); ++t) {
        // 把第 t 张纹理挂上 FBO 并清空（清成全 0 = 全不覆盖）
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, _maskTextures[t], 0);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);

        for (const ClipContext& ctx : _clipContexts) {
            if (ctx.textureIndex != t) continue;
            // 只写该 context 占用的通道
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
    glBlendEquation(GL_FUNC_ADD);   // 还原，主绘制依赖标准混合
    _maskProgram.release();
}

void Live2DRenderer::requestMaskDump(const QString& dir)
{
    _maskDumpDir = dir;
    _maskDumpPending = true;
}

// 把每张离屏蒙版纹理读回 CPU 存成 PNG。调用时 _maskFbo 须为当前绑定的 FBO。
// 每张纹理一张图，RGBA 四通道分别是 4 个 ClipContext 的覆盖度。
// glReadPixels 行序自下而上，QImage 自上而下，故 mirrored 翻 Y。
void Live2DRenderer::dumpMaskTextures(const QString& dir)
{
    if (_maskTextures.empty() || _maskW <= 0 || _maskH <= 0) {
        qWarning() << "[Live2DRenderer] 无蒙版可导出";
        return;
    }
    QDir().mkpath(dir);
    std::vector<uint8_t> buf((size_t)_maskW * _maskH * 4);

    for (int t = 0; t < (int)_maskTextures.size(); ++t) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, _maskTextures[t], 0);
        glReadPixels(0, 0, _maskW, _maskH, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());

        QImage img(buf.data(), _maskW, _maskH, QImage::Format_RGBA8888);
        const QString path = QDir(dir).filePath(QString("mask_%1.png").arg(t));
        if (img.flipped(Qt::Vertical).save(path))   // 翻 Y 后存（img 复制了像素，安全）
            qInfo() << "[Live2DRenderer] 蒙版已导出:" << path;
        else
            qWarning() << "[Live2DRenderer] 蒙版导出失败:" << path;
    }
}

void Live2DRenderer::draw(const float* mvp16, int fbW, int fbH)
{
    if (!_model || _drawables.empty()) return;
    CubismModel* m = _model->model();
    Core::csmModel* core = m->GetModel();
    const csmInt32 count = m->GetDrawableCount();

    // CubismMatrix44 是列主序；QMatrix4x4(const float*) 按行主序读，故需转置。
    QMatrix4x4 mvp = QMatrix4x4(mvp16).transposed();

    // —— 蒙版预渲染 —— 先把当前 FBO/视口存下来，画完再恢复。
    // QOpenGLWidget 默认 FBO 并非 0，必须查询保存。
    const bool useMasking = !_clipContexts.empty();
    if (useMasking) {
        GLint prevFbo = 0, prevViewport[4] = {0, 0, fbW, fbH};
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
        glGetIntegerv(GL_VIEWPORT, prevViewport);
        ensureMaskTextures(fbW, fbH);
        renderMasks(mvp);
        // 蒙版纹理刚画好、_maskFbo 仍绑定，此时读回导出最稳妥。
        if (_maskDumpPending) {
            dumpMaskTextures(_maskDumpDir);
            _maskDumpPending = false;
        }
        // 恢复主 framebuffer 与视口
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    }

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
    _program.setUniformValue(_locMaskTex, 1);   // 蒙版纹理固定在单元 1

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

        // 蒙版：该 drawable 若被裁剪，绑定其 context 的纹理+通道到单元 1
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

        // 贴图（单元 0）
        glActiveTexture(GL_TEXTURE0);
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

void Live2DRenderer::releaseMaskResources()
{
    if (_maskFbo) { glDeleteFramebuffers(1, &_maskFbo); _maskFbo = 0; }
    for (GLuint& t : _maskTextures) {
        if (t) { glDeleteTextures(1, &t); t = 0; }
    }
    _maskW = _maskH = 0;
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
    releaseMaskResources();
    _clipContexts.clear();
    _drawableToClip.clear();
}
