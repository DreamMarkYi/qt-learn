#ifndef L2D_MODEL_H
#define L2D_MODEL_H

#include <QString>
#include <QStringList>
#include <QByteArray>
#include <Motion/CubismMotion.hpp>
#include <Motion/CubismMotionManager.hpp>
#include "PoseTypes.h"

// 前置声明，避免在头文件里拖进整个 Framework
namespace Live2D { namespace Cubism { namespace Framework {
    class CubismMoc;
    class CubismModel;
    class CubismModelMatrix;
    class ICubismModelSetting;
}}}

// 纯数据层：把一个 model3.json 模型加载进 Cubism Core，持有 CubismModel
// （顶点/UV/颜色/绘制顺序都从它读），并算好把模型摆进归一化空间的 ModelMatrix。
// **不碰 OpenGL**——GL 资源由 L2dRenderer 管。纯 CPU，可在任意线程加载。
//
// 主合成版：从 Live2D/Live2DModel 移植而来，去掉 GUI 专用的 QTimer 成员，
// 让动画推进完全由合成线程的帧节拍驱动（见 L2dLayer::render）。
class L2dModel
{
public:
    L2dModel() = default;
    ~L2dModel();

    // 进程级：启动 / 释放 Cubism Framework 全局。各调一次。
    static bool initFramework();
    static void disposeFramework();

    // 加载 <dir>/<fileName>（fileName 形如 "Rice.model3.json"）。
    // 读 moc3 建模型、设默认参数、Update() 算出静止姿态顶点。成功返回 true。
    bool load(const QString& dir, const QString& fileName);

    bool isLoaded() const { return _model != nullptr; }

    // 加载并播放一个 motion3.json（绝对路径）。loop=true 循环。
    void startMotion(const QString& motionPath, bool loop = true);

    // 推进动作时间轴并重算顶点。deltaTime 单位秒。返回是否有动作在播。
    bool updateMotion(float deltaTime);

    // 用面部表情数据驱动模型参数（写眼/嘴/眉/头角等），再 Update。
    // 须在 updateMotion 之后调用，使表情覆盖动作设的对应参数。
    void applyFace(const FaceData& f);

    // 用身体语义值驱动模型身体参数（目前只写 ParamBodyAngleZ），再 Update。
    void applyBody(const BodyData& b);

    // 供渲染器读取的访问器
    Live2D::Cubism::Framework::CubismModel*       model() const { return _model; }
    Live2D::Cubism::Framework::CubismModelMatrix* modelMatrix() const { return _modelMatrix; }

    // 贴图绝对路径列表（索引 = drawable 的 TextureIndex）
    const QStringList& texturePaths() const { return _texturePaths; }

private:
    static QByteArray readFile(const QString& path);

    // 按参数 ID 写值；模型没有该参数则跳过（不同模型参数名/有无不一）。
    void setParamSafe(const char* id, float value);

    Live2D::Cubism::Framework::CubismMoc*           _moc           = nullptr;
    Live2D::Cubism::Framework::CubismModel*         _model         = nullptr;
    Live2D::Cubism::Framework::CubismModelMatrix*   _modelMatrix   = nullptr;
    Live2D::Cubism::Framework::ICubismModelSetting* _setting       = nullptr;
    Live2D::Cubism::Framework::CubismMotionManager* _motionManager = nullptr;
    QStringList _texturePaths;
};

#endif // L2D_MODEL_H
