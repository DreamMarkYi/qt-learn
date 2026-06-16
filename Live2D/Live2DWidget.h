#ifndef LIVE2D_WIDGET_H
#define LIVE2D_WIDGET_H

#include <QOpenGLWidget>
#include <QString>
#include <memory>

#include "Live2DModel.h"
#include "Live2DRenderer.h"
#include <Math/CubismMatrix44.hpp>
#include <QTimer>
#include <QElapsedTimer>
// 独立显示一个 Live2D 模型的 OpenGL 窗口部件。
// 不播动画、不接推流：加载模型 → 每帧用自定义渲染器画静止姿态。
class Live2DWidget : public QOpenGLWidget
{
    Q_OBJECT
public:
    // dir: 模型所在目录；fileName: "xxx.model3.json"
    Live2DWidget(QString dir, QString fileName, QWidget* parent = nullptr);
    ~Live2DWidget() override;

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void computeMvp(float* out16) const;   // 输出列主序 16 floats

    QString _dir, _file;
    Live2DModel    _model;
    Live2DRenderer _renderer;
    bool _ready = false;
    int  _vpW = 1, _vpH = 1;
    QTimer        _timer;
    QElapsedTimer _clock;
};

#endif // LIVE2D_WIDGET_H
