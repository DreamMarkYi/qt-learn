#ifndef L2D_LAYER_H
#define L2D_LAYER_H

#include "ICompositorLayer.h"
#include "OffscreenTarget.h"
#include "L2dModel.h"
#include "L2dRenderer.h"
#include "PoseBus.h"
#include <QString>
#include <QRect>
#include <QSize>

// 方案 B 的 Live2D 合成层：把模型 + 渲染器 + 一张离屏 FBO 打包成 ICompositorLayer。
// render() 内部先把人物画到自己的离屏纹理（透明底），返回该纹理；GlCompositor 再
// 把它当 quad 贴到主输出的 placement() 矩形里。
//
// 与 GlCompositor 共用同一个 GL 上下文/线程，故不需要第二个上下文，也不需要独立线程。
//
// 多继承 QOpenGLFunctions_3_3_Core：本层自身也要发 GL 调用（清屏/绑 FBO），
// 且要把函数表传给 OffscreenTarget。protected 继承避免污染公开接口。
class L2dLayer : public ICompositorLayer, protected QOpenGLFunctions_3_3_Core
{
public:
    // dir: 模型目录；modelFile: "Rice.model3.json"；motionFile: 可空（不播动作）。
    // renderSize: 离屏渲染分辨率（人物自身的清晰度，可独立于输出，省显存）。
    // placement: 人物在最终输出里的像素矩形（左上原点）。
    L2dLayer(QString dir, QString modelFile, QString motionFile,
             QSize renderSize, QRect placement)
        : _dir(std::move(dir)), _modelFile(std::move(modelFile)),

          _renderSize(renderSize), _placement(placement) {}

    bool init() override;
    GLuint render(float dtSeconds) override;
    QRect placement(const QSize& /*outputSize*/) const override { return _placement; }
    bool premultiplied() const override { return true; }   // L2dRenderer 输出预乘 alpha
    bool bottomUp() const override { return true; }        // 离屏 FBO 渲染结果自下而上
    void release() override;

    void setPlacement(const QRect& r) { _placement = r; }

    // 注入姿态/表情数据源（渲染线程每帧读取来驱动模型）。可在 init 前后任意时刻设置。
    void setPoseBus(PoseBus* bus) { _bus = bus; }

private:
    // 把模型摆进离屏画布的 MVP：按 renderSize 宽高比防拉伸（照搬 Live2DWidget::computeMvp）。
    void computeMvp(float* out16) const;

    QString _dir, _modelFile, _motionFile;
    QSize   _renderSize;
    QRect   _placement;

    L2dModel        _model;
    L2dRenderer     _renderer;
    OffscreenTarget _target;
    PoseBus*        _bus = nullptr;     // 不持有，仅引用
    bool _ready = false;
};

#endif // L2D_LAYER_H
