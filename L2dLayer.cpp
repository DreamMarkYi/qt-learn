#include "L2dLayer.h"

#include <Math/CubismMatrix44.hpp>
#include <Math/CubismModelMatrix.hpp>
#include <QDir>
#include <QDebug>

using namespace Live2D::Cubism::Framework;

bool L2dLayer::init()
{
    initializeOpenGLFunctions();

    L2dModel::initFramework();   // 进程级，重复调用安全（StartUp 幂等）

    if (!_model.load(_dir, _modelFile)) {
        qWarning() << "[L2dLayer] 模型加载失败:" << _dir << _modelFile;
        return false;
    }
    if (!_renderer.initialize(&_model)) {
        qWarning() << "[L2dLayer] 渲染器初始化失败";
        return false;
    }
    if (!_motionFile.isEmpty()) {
        const QString motionPath = QDir(_dir).filePath(_motionFile);
        //_model.startMotion(motionPath, true);
    }
    _ready = true;
    qInfo() << "[L2dLayer] 初始化完成, 离屏尺寸 =" << _renderSize
            << "输出位置 =" << _placement;
    return true;
}

// MVP = 投影(按离屏画布宽高比防拉伸) * ModelMatrix(模型归一化到高=2 居中)。
void L2dLayer::computeMvp(float* out16) const
{
    CubismMatrix44 proj;
    const float w = float(_renderSize.width()  > 0 ? _renderSize.width()  : 1);
    const float h = float(_renderSize.height() > 0 ? _renderSize.height() : 1);
    const float aspect = w / h;
    if (aspect >= 1.0f) proj.Scale(1.0f / aspect, 1.0f);   // 宽：压 X
    else                proj.Scale(1.0f, aspect);          // 高：压 Y

    proj.MultiplyByMatrix(_model.modelMatrix());
    const float zoom = 2.0f;          // ←加这行：>1 放大特写，<1 缩小
    proj.ScaleRelative(zoom, zoom);
    const float* a = proj.GetArray();
    for (int i = 0; i < 16; ++i) out16[i] = a[i];
}

GLuint L2dLayer::render(float dtSeconds)
{
    if (!_ready) return 0;
    if (!_target.ensure(*this, _renderSize)) return 0;

    // 动作推进与绘制必须配对，都在本（合成）线程做
    //_model.updateMotion(dtSeconds);

    // 用最新表情覆盖动作设的眼/嘴/眉/头角等参数（数据来自 Python holistic 边车）
    if (_bus) _model.applyFace(_bus->face());
    // 用最新身体语义值驱动身体参数（目前 ParamBodyAngleZ，由两肩连线倾角算）
    if (_bus) _model.applyBody(_bus->body());

    // 画到自己的离屏 FBO（透明底）。L2dRenderer::draw 会自己保存/恢复 FBO，
    // 但它的「保存值」就是我们这里 bind 的 _target.fbo()，所以蒙版预渲染后能正确回到它。
    _target.bindAndClear(*this);

    float mvp[16];
    computeMvp(mvp);
    _renderer.draw(mvp, _renderSize.width(), _renderSize.height());

    return _target.texture();
}

void L2dLayer::release()
{
    _renderer.release();
    _target.release(*this);
    _ready = false;
}
