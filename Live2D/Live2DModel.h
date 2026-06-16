#ifndef LIVE2D_MODEL_H
#define LIVE2D_MODEL_H

#include <QString>
#include <QStringList>
#include <QByteArray>
#include <Motion/CubismMotion.hpp>
#include <Motion/CubismMotionManager.hpp>
#include <QTimer>
#include <QElapsedTimer>

// 前置声明，避免在头文件里拖进整个 Framework
namespace Live2D { namespace Cubism { namespace Framework {
    class CubismMoc;
    class CubismModel;
    class CubismModelMatrix;
    class ICubismModelSetting;
}}}

// 纯数据层：只负责把一个 model3.json 模型加载进 Cubism Core，
// 持有 CubismModel（顶点/UV/颜色/绘制顺序都从它读），并算好把模型
// 摆进归一化空间的 ModelMatrix。**不碰 OpenGL**——GL 资源由 Live2DRenderer 管。
//
// 不用 CubismUserModel：我们不播动画、不要物理/动作管理器，
// 直接走 CubismMoc -> CreateModel 的最短加载路径。
class Live2DModel
{
public:
    Live2DModel() = default;
    ~Live2DModel();

    // 进程级：启动 / 释放 Cubism Framework 全局。各调一次。
    static bool initFramework();
    static void disposeFramework();

    // 加载 <dir>/<fileName>（fileName 形如 "我.model3.json"）。
    // 读 moc3 建模型、调一次 Update() 算出静止姿态的顶点。成功返回 true。
    // 纯 CPU 操作，可在任意线程调用（不需要 GL 上下文）。
    bool load(const QString& dir, const QString& fileName);

    bool isLoaded() const { return _model != nullptr; }

    void startMotion(const QString& motionPath , bool loop =  true);

    bool updateMotion(float deltaTime);

    // 供渲染器读取的访问器
    Live2D::Cubism::Framework::CubismModel*       model() const { return _model; }
    Live2D::Cubism::Framework::CubismModelMatrix* modelMatrix() const { return _modelMatrix; }

    // 贴图绝对路径列表（索引 = drawable 的 TextureIndex）
    const QStringList& texturePaths() const { return _texturePaths; }

private:
    // 用 QFile 读整个文件为字节（QFile 原生支持中文路径，避开 std::ifstream 的坑）
    static QByteArray readFile(const QString& path);
    QTimer        _timer;
    QElapsedTimer _clock;

    Live2D::Cubism::Framework::CubismMoc*             _moc         = nullptr;
    Live2D::Cubism::Framework::CubismModel*           _model       = nullptr;
    Live2D::Cubism::Framework::CubismModelMatrix*     _modelMatrix = nullptr;
    Live2D::Cubism::Framework::ICubismModelSetting*   _setting     = nullptr;
    Live2D::Cubism::Framework::CubismMotionManager* _motionManager = nullptr;
    QStringList _texturePaths;
};

#endif // LIVE2D_MODEL_H
