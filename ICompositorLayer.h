#ifndef ICOMPOSITOR_LAYER_H
#define ICOMPOSITOR_LAYER_H

#include <QOpenGLFunctions_3_3_Core>
#include <QSize>
#include <QRect>

// 合成管线里的「一层」抽象：每层把自己渲染到一张 GL 纹理，再交给 GlCompositor
// 当作 quad 贴到主输出 FBO 上。把「画什么」与「怎么合成」解耦，后续要加新层
// （另一个虚拟形象、字幕层、特效层……）只需再实现一个本接口，GlCompositor 不动。
//
// 约定：所有方法都在合成线程、GL 上下文 current 时调用（与 GlCompositor 同一上下文，
// 故纹理 id 天然互通，无需共享上下文）。
class ICompositorLayer
{
public:
    virtual ~ICompositorLayer() = default;

    // 创建本层的 GL 资源（program / FBO / 纹理 / 模型缓冲…）。GL 上下文须 current。
    virtual bool init() = 0;

    // 渲染本层这一帧，返回承载结果的颜色纹理 id。dtSe
    // conds 为距上一帧的秒数，
    // 供有动画的层推进时间轴用（静态层可忽略）。GL 上下文须 current。
    // 注意：本方法可能改变当前绑定的 FBO / GL 状态，调用方负责在使用返回纹理前
    // 重新绑定自己的目标 FBO。
    virtual GLuint render(float dtSeconds) = 0;

    // 本层在最终输出里的像素矩形（左上原点，与 QRect 习惯一致）。
    // GlCompositor 据此换算成 NDC 决定 quad 的位置与大小。
    virtual QRect placement(const QSize& outputSize) const = 0;

    // 返回纹理是否为「预乘 alpha」。预乘需用 GL_ONE, GL_ONE_MINUS_SRC_ALPHA 混合；
    // 非预乘用 GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA。Live2D 输出预乘，故默认 true。
    virtual bool premultiplied() const { return true; }

    // 返回纹理是否为「自下而上」（GL FBO 原生方向）。FBO 渲染结果 v=0 在底部，
    // 与从 QImage 上传的自上而下纹理相反，故合成时 V 翻转策略不同。
    virtual bool bottomUp() const { return true; }

    // 释放本层 GL 资源。GL 上下文须 current。
    virtual void release() = 0;
};

#endif // ICOMPOSITOR_LAYER_H
