#include "L2dModel.h"
#include "Live2DAllocator.h"

#include <CubismFramework.hpp>
#include <Id/CubismIdManager.hpp>
#include <Model/CubismMoc.hpp>
#include <Model/CubismModel.hpp>
#include <Math/CubismModelMatrix.hpp>
#include <CubismModelSettingJson.hpp>

#include <QFile>
#include <QDir>
#include <QDebug>

using namespace Live2D::Cubism::Framework;

namespace {
    // 全局只需一份分配器与启动选项，进程退出前一直存活。
    Live2DAllocator         g_allocator;
    CubismFramework::Option g_option;
}

// ── 进程级 Framework 启停 ───────────────────────────────────────────
bool L2dModel::initFramework()
{
    g_option.LogFunction  = [](const char* msg){ qDebug() << "[Cubism]" << msg; };
    g_option.LoggingLevel = CubismFramework::Option::LogLevel_Warning;

    CubismFramework::StartUp(&g_allocator, &g_option);
    CubismFramework::Initialize();
    return CubismFramework::IsInitialized();
}

void L2dModel::disposeFramework()
{
    CubismFramework::Dispose();
}

QByteArray L2dModel::readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[L2dModel] 打不开文件:" << path;
        return {};
    }
    return f.readAll();
}

L2dModel::~L2dModel()
{
    delete _modelMatrix;
    delete _setting;
    if (_moc && _model) _moc->DeleteModel(_model);
    if (_moc)           CubismMoc::Delete(_moc);
    _model = nullptr;
    _moc   = nullptr;
    delete _motionManager;
    _motionManager = nullptr;
}

bool L2dModel::load(const QString& dir, const QString& fileName)
{
    const QString base = QDir(dir).absolutePath() + QDir::separator();

    // 1) 读 model3.json，解析清单（moc / 贴图 相对路径）
    QByteArray settingBytes = readFile(base + fileName);
    if (settingBytes.isEmpty()) return false;

    _setting = new CubismModelSettingJson(
        reinterpret_cast<const csmByte*>(settingBytes.constData()),
        static_cast<csmSizeInt>(settingBytes.size()));

    // 2) 读 moc3 建模型
    const char* mocFile = _setting->GetModelFileName();
    if (!mocFile || mocFile[0] == '\0') {
        qWarning() << "[L2dModel] model3.json 未声明 moc";
        return false;
    }
    QByteArray mocBytes = readFile(base + QString::fromUtf8(mocFile));
    if (mocBytes.isEmpty()) return false;

    _moc = CubismMoc::Create(
        reinterpret_cast<const csmByte*>(mocBytes.constData()),
        static_cast<csmSizeInt>(mocBytes.size()),
        /*shouldCheckMocConsistency*/ true);
    if (!_moc) {
        qWarning() << "[L2dModel] moc3 解析失败（版本不符或文件损坏）";
        return false;
    }

    _model = _moc->CreateModel();
    if (!_model) {
        qWarning() << "[L2dModel] CreateModel 失败";
        return false;
    }

    // 3) 把参数重置为模型默认值，再 Update 算出 default pose 顶点
    {
        const csmInt32 paramCount = _model->GetParameterCount();
        for (csmInt32 pi = 0; pi < paramCount; ++pi)
            _model->SetParameterValue(pi, _model->GetParameterDefaultValue(pi));
    }
    _model->Update();

    // 4) ModelMatrix：把模型画布归一化到高度=2 并居中，使顶点经 MVP 落进 NDC
    const float canvasW = _model->GetCanvasWidth();
    const float canvasH = _model->GetCanvasHeight();
    _modelMatrix = new CubismModelMatrix(canvasW -0.5, canvasH);  // 构造里 SetHeight(2.0)
    _modelMatrix->SetCenterPosition(0.0f, 0.0f);

    // 5) 收集贴图绝对路径（索引 = TextureIndex）
    _texturePaths.clear();
    const csmInt32 texCount = _setting->GetTextureCount();
    for (csmInt32 i = 0; i < texCount; ++i) {
        const char* rel = _setting->GetTextureFileName(i);
        _texturePaths << (base + QString::fromUtf8(rel));
    }

    _motionManager = new CubismMotionManager();

    qInfo() << "[L2dModel] 加载成功:"
            << "drawables =" << _model->GetDrawableCount()
            << "textures ="  << texCount
            << "canvas ="    << canvasW << "x" << canvasH;
    return true;
}

void L2dModel::startMotion(const QString& motionPath, bool loop)
{
    QByteArray bytes = readFile(motionPath);
    if (bytes.isEmpty()) return;

    CubismMotion* motion = static_cast<CubismMotion*>(
        CubismMotion::Create(
            reinterpret_cast<const csmByte*>(bytes.constData()),
            static_cast<csmSizeInt>(bytes.size())));
    if (!motion) {
        qWarning() << "[L2dModel] 动作解析失败:" << motionPath;
        return;
    }
    motion->SetLoop(loop);
    _motionManager->StartMotionPriority(motion, false, 3);   // 优先级 3 = 强制打断
}

bool L2dModel::updateMotion(float deltaTime)
{
    if (!_model || !_motionManager) return false;
    _model->LoadParameters();                                  // 读上帧参数（叠加用）
    bool updated = _motionManager->UpdateMotion(_model, deltaTime);
    _model->SaveParameters();                                  // 存本帧参数
    _model->Update();                                          // 重算 drawable 顶点
    return updated;
}

void L2dModel::setParamSafe(const char* id, float value)
{
    if (!_model) return;
    CubismIdHandle handle = CubismFramework::GetIdManager()->GetId(id);
    if (_model->GetParameterIndex(handle) < 0) return;         // 模型无此参数，跳过
    _model->SetParameterValue(handle, value);                  // 内部会按参数范围 clamp
}

void L2dModel::applyFace(const FaceData& f)
{
    if (!_model || !f.ok) return;

    // 语义值已在 Python 端按 Live2D 参数范围算好并平滑，这里直接写绝对值。
    setParamSafe("ParamEyeLOpen",   f.eyeLOpen);
    setParamSafe("ParamEyeROpen",   f.eyeROpen);
    setParamSafe("ParamEyeBallX",   f.eyeballX);
    setParamSafe("ParamEyeBallY",   f.eyeballY);
    setParamSafe("ParamMouthOpenY", f.mouthOpen);
    setParamSafe("ParamMouthForm",  f.mouthForm);
    setParamSafe("ParamBrowLAngle",     f.browL);
    setParamSafe("ParamBrowRAngle",     f.browR);
    setParamSafe("ParamCheek",      f.cheek);
    setParamSafe("Param16",     f.angleX);
    setParamSafe("Param17",     f.angleY);
    setParamSafe("Param18",     f.angleZ);

    _model->Update();                                          // 写完参数重算顶点
}

void L2dModel::applyBody(const BodyData& b)
{
    if (!_model || !b.ok) return;

    // 身体语义值已在 Python 端算好（度），直接写绝对值；setParamSafe 内部会 clamp。
    setParamSafe("ParamBodyAngleZ", b.angleZ);                 // 身体左右摆

    _model->Update();                                          // 写完参数重算顶点
}
