#include "Live2DModel.h"
#include "../Live2DAllocator.h"

#include <CubismFramework.hpp>
#include <Model/CubismMoc.hpp>
#include <Model/CubismModel.hpp>
#include <Math/CubismModelMatrix.hpp>
#include <CubismModelSettingJson.hpp>

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>

using namespace Live2D::Cubism::Framework;

namespace {
    // 全局只需一份分配器与启动选项，进程退出前一直存活。
    Live2DAllocator        g_allocator;
    CubismFramework::Option g_option;
}

// ── 进程级 Framework 启停 ───────────────────────────────────────────
bool Live2DModel::initFramework()
{
    g_option.LogFunction  = [](const char* msg){ qDebug() << "[Cubism]" << msg; };
    g_option.LoggingLevel = CubismFramework::Option::LogLevel_Warning;

    CubismFramework::StartUp(&g_allocator, &g_option);
    CubismFramework::Initialize();
    return CubismFramework::IsInitialized();
}

void Live2DModel::disposeFramework()
{
    CubismFramework::Dispose();
}

// ── 文件 → 字节 ─────────────────────────────────────────────────────
QByteArray Live2DModel::readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[Live2DModel] 打不开文件:" << path;
        return {};
    }
    return f.readAll();
}

Live2DModel::~Live2DModel()
{
    delete _modelMatrix;
    delete _setting;
    if (_moc && _model) _moc->DeleteModel(_model);
    if (_moc)           CubismMoc::Delete(_moc);
    _model = nullptr;
    _moc   = nullptr;
    if (_motionManager) {
        delete _motionManager;
        _motionManager = nullptr;
    }
}

bool Live2DModel::load(const QString& dir, const QString& fileName)
{
    const QString base = QDir(dir).absolutePath() + QDir::separator();

    // 1) 读 model3.json，解析清单（moc / 贴图 相对路径）
    QByteArray settingBytes = readFile(base + fileName);
    if (settingBytes.isEmpty()) return false;

    _setting = new CubismModelSettingJson(
        reinterpret_cast<const csmByte*>(settingBytes.constData()),
        static_cast<csmSizeInt>(settingBytes.size()));



    // 2) 读 moc3 建模型。CubismMoc::Create 内部会 CSM_MALLOC_ALIGNED + memcpy
    //    做对齐，所以这里传非对齐的 QByteArray 字节完全没问题。
    const char* mocFile = _setting->GetModelFileName();
    if (!mocFile || mocFile[0] == '\0') {
        qWarning() << "[Live2DModel] model3.json 未声明 moc";
        return false;
    }
    QByteArray mocBytes = readFile(base + QString::fromUtf8(mocFile));
    if (mocBytes.isEmpty()) return false;

    _moc = CubismMoc::Create(
        reinterpret_cast<const csmByte*>(mocBytes.constData()),
        static_cast<csmSizeInt>(mocBytes.size()),
        /*shouldCheckMocConsistency*/ true);
    if (!_moc) {
        qWarning() << "[Live2DModel] moc3 解析失败（版本不符或文件损坏）";
        return false;
    }

    _model = _moc->CreateModel();
    if (!_model) {
        qWarning() << "[Live2DModel] CreateModel 失败";
        return false;
    }

    // 3) 把所有参数重置为模型定义的默认值，再 Update 算出 default pose 的顶点。
    //    CreateModel 后参数处于未初始化状态，不手动设默认值会得到零姿态（散架）。
    {
        const csmInt32 paramCount = _model->GetParameterCount();
        for (csmInt32 pi = 0; pi < paramCount; ++pi)
            _model->SetParameterValue(pi, _model->GetParameterDefaultValue(pi));
    }
    _model->Update();
    // 4) ModelMatrix：把模型画布归一化到高度 = 2 的空间并居中，
    //    使顶点经 MVP 后落在 NDC 内。等价于官方 SetupModel 的默认行为。
    const float canvasW = _model->GetCanvasWidth();   // 单位：model 坐标
    const float canvasH = _model->GetCanvasHeight();
    _modelMatrix = new CubismModelMatrix(canvasW, canvasH);  // 构造里 SetHeight(2.0)
    _modelMatrix->SetCenterPosition(0.0f, 0.0f);             // 居中到原点

    // 5) 收集贴图绝对路径（索引 = TextureIndex）
    _texturePaths.clear();
    const csmInt32 texCount = _setting->GetTextureCount();
    for (csmInt32 i = 0; i < texCount; ++i) {
        const char* rel = _setting->GetTextureFileName(i);
        _texturePaths << (base + QString::fromUtf8(rel));
    }

    _motionManager = new CubismMotionManager();

    qInfo() << "[Live2DModel] 加载成功:"
            << "drawables =" << _model->GetDrawableCount()
            << "textures ="  << texCount
            << "canvas ="    << canvasW << "x" << canvasH;
    return true;
}

void Live2DModel::startMotion(const QString& motionPath , bool loop) {
    QByteArray bytes = readFile(motionPath);
    if (bytes.isEmpty()) return;

    CubismMotion* motion = static_cast<CubismMotion*>(
        CubismMotion::Create(
            reinterpret_cast<const csmByte*>(bytes.constData()),
            static_cast<csmSizeInt>(bytes.size())
        )
    );

    motion->SetLoop(loop);
    // 优先级 3 = 强制打断当前动作
    _motionManager->StartMotionPriority(motion, false, 3);
}

bool Live2DModel::updateMotion(float deltaTime)
{
    _model->LoadParameters();   // 读取上一帧保存的参数（口型/眨眼等叠加用）

    bool updated = _motionManager->UpdateMotion(_model, deltaTime);

    _model->SaveParameters();   // 保存本帧参数供下次叠加
    _model->Update();           // 根据新参数重算所有 drawable 的顶点
    return updated;
}