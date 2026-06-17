#include "Live2DWidget.h"

#include <Math/CubismModelMatrix.hpp>
#include <QDebug>
#include <QKeyEvent>

using namespace Live2D::Cubism::Framework;

Live2DWidget::Live2DWidget(QString dir, QString fileName, QWidget* parent)
    : QOpenGLWidget(parent), _dir(std::move(dir)), _file(std::move(fileName))
{
    resize(600, 800);
    setWindowTitle(QStringLiteral("Live2D 自定义渲染器"));
    setFocusPolicy(Qt::StrongFocus);   // 接收键盘事件（按 S 导出蒙版）
}

Live2DWidget::~Live2DWidget()
{
    // GL 资源必须在上下文 current 时释放；makeCurrent 后让 _renderer 的析构器自己清理。
    makeCurrent();
    _renderer.release();
    doneCurrent();
}

void Live2DWidget::initializeGL()
{
    // Framework 全局启动（进程一次）。纯 CPU，不依赖 GL，但放这里最省事。
    Live2DModel::initFramework();

    if (!_model.load(_dir, _file)) {
        qWarning() << "[Live2DWidget] 模型加载失败";
        return;
    }
    if (!_renderer.initialize(&_model)) {
        qWarning() << "[Live2DWidget] 渲染器初始化失败";
        return;
    }
    _ready = true;

    // 首帧自动把蒙版导出到模型目录，方便检查；之后可按 S 重导。
    _renderer.requestMaskDump(_dir);

    _model.startMotion(_dir + "/motions/mtn_01.motion3.json", true);

    // 启动 60fps 定时器
    connect(&_timer, &QTimer::timeout, this, [this]() {
        float dt = _clock.elapsed() / 1000.0f;  // 毫秒转秒
        _clock.restart();
        _model.updateMotion(dt);                 // 推进动作 + Update()
        update();                                // 触发 paintGL
    });
    _clock.start();
    _timer.start(16);   // 约 60fps
}

void Live2DWidget::resizeGL(int w, int h)
{
    _vpW = w > 0 ? w : 1;
    _vpH = h > 0 ? h : 1;
}

// MVP = 投影(按窗口宽高比防拉伸) * ModelMatrix(把模型归一化到高=2 居中)。
void Live2DWidget::computeMvp(float* out16) const
{
    CubismMatrix44 proj;
    const float aspect = float(_vpW) / float(_vpH);
    // 模型在 ModelMatrix 后高度约为 2（NDC 满高）。窗口越宽，X 越要压缩，
    // 否则模型横向被拉胖。竖屏则压 Y。
    if (aspect >= 1.0f) proj.Scale(1.0f / aspect, 1.0f);   // 宽屏：压 X
    else                proj.Scale(1.0f, aspect);          // 竖屏：压 Y

    // proj * model
    proj.MultiplyByMatrix(_model.modelMatrix());

    const float* a = proj.GetArray();
    for (int i = 0; i < 16; ++i) out16[i] = a[i];
}

void Live2DWidget::paintGL()
{
    glViewport(0, 0, _vpW, _vpH);
    glClearColor(0.15f, 0.15f, 0.18f, 1.0f);   // 深灰背景，便于看清模型边缘
    glClear(GL_COLOR_BUFFER_BIT);

    if (!_ready) return;

    float mvp[16];
    computeMvp(mvp);
    // 蒙版离屏纹理与主绘制用同一套视口尺寸，保证屏幕坐标 ↔ 蒙版坐标对齐。
    _renderer.draw(mvp, _vpW, _vpH);
}

// 按 S：把当前帧的离屏蒙版纹理导出为 PNG 到模型目录。
void Live2DWidget::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_S) {
        _renderer.requestMaskDump(_dir);
        update();   // 触发一帧，导出在 draw 内完成
    } else {
        QOpenGLWidget::keyPressEvent(e);
    }
}
