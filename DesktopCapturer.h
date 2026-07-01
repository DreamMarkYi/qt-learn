#ifndef DESKTOPCAPTURER_H
#define DESKTOPCAPTURER_H

#include <QObject>
#include <QScreenCapture>
#include <QMediaCaptureSession>
#include <QGuiApplication>
#include <QImage>
#include <QDebug>
#include <QThread>

#include "CameraManager.h"
#include "FFmpegStreamer.h"
#include "GlCompositor.h"
#include "L2dLayer.h"
#include "PoseFrameClient.h"
#include "PoseBus.h"

class DesktopCapturer : public QObject
{
    Q_OBJECT
public:
    explicit DesktopCapturer(QObject *parent = nullptr) : QObject(parent) {
        // 1. 桌面捕获链路
        m_screenSession = new QMediaCaptureSession(this);
        m_screenCapturer = new QScreenCapture(this);
        m_screenCapturer->setScreen(QGuiApplication::primaryScreen());
        m_screenSession->setScreenCapture(m_screenCapturer);

        // 2. 相机管理器
        m_cameraManager = new CameraManager(this);

        // 3. 推流器（纯编码，吃 stdin 原始帧）
        m_streamer = new FFmpegStreamer(this);

        // 4. 本地 GPU 合成器，把两路捕获接到它的 sink 上
        m_compositor = new GlCompositor();

        // 4.4 姿态/表情数据总线（PoseFrameClient 写，L2dLayer 在渲染线程读）。
        //     须在渲染线程启动前注入给 L2dLayer，避免 setPoseBus 与 render 竞争。
        m_poseBus = new PoseBus();

        // 4.5 注册 Live2D 人物层（方案 B：先渲染到离屏纹理再合成）。
        //     必须在 moveToThread/init 之前 addLayer——init() 会在渲染线程里
        //     连同各层一起建 GL 资源。离屏渲染尺寸 720x1080，贴到输出左下角。
        {
            const QString dir  = QStringLiteral(
                "D:/My project (5)/Assets/海洋 模型");
            const QString modelFile  = QStringLiteral("海洋.model3.json");
            const QString motionFile = QStringLiteral("motions/mtn_01.motion3.json");
            const QSize outSize = m_compositor->outputSize();   // 1920x1080
            const QSize l2dSize(720, 1080);
            const QRect placement(240, outSize.height() - 1080, 720, 1080); // 左下
            auto l2d = std::make_unique<L2dLayer>(
                dir, modelFile, motionFile, l2dSize, placement);
            l2d->setPoseBus(m_poseBus);                 // 注入表情数据源
            m_compositor->addLayer(std::move(l2d));
        }

        m_renderThread = new QThread(this);
        m_compositor->moveToThread(m_renderThread);
        connect(m_renderThread, &QThread::started,
                m_compositor, &GlCompositor::init);
        m_renderThread->start();

        m_screenSession->setVideoSink(m_compositor->screenSink());
        m_cameraManager->setVideoSink(m_compositor->cameraSink());

        // 4.6 骨骼检测：tap 摄像头 sink 的帧，发给 Python 边车做 Pose 检测。
        //     cameraSink 在渲染线程发信号，m_poseClient 在主线程 → 自动队列连接，
        //     拷一份 QVideoFrame 过来，不阻塞渲染。
        m_poseClient = new PoseFrameClient(QStringLiteral("127.0.0.1"), 5066, this);
        m_poseClient->setBus(m_poseBus);                // 收到的姿态/表情写入总线
        connect(m_compositor->cameraSink(), &QVideoSink::videoFrameChanged,
                m_poseClient, &PoseFrameClient::onFrame);

        // 5. 合成帧出炉：一路预览(转发给 UI)，一路推流
        connect(m_compositor, &GlCompositor::frameReady, this,
                [this](const QImage &frame) {
            emit previewFrame(frame);            // 给 UI 预览
            if (m_streamer->isPushing())
                m_streamer->writeFrame(frame);   // 给 ffmpeg 推流
        });
    }

    // 🎬 开启本地双路预览（合成帧会通过 previewFrame 信号持续吐出）
    void startAllPreviews() {
        m_screenCapturer->start();
        m_cameraManager->start();
        qDebug() << "▶️ [总控] 本地双路预览已启动";
    }

    void stopAllPreviews() {
        m_screenCapturer->stop();
        m_cameraManager->stop();
        qDebug() << "⏹️ [总控] 本地预览已关闭";
    }

    // 🚀 开启推流：输入用合成器当前尺寸，输出固定缩放到 1080p（FFmpeg scale），
    //    音频抓 VoiceMeeter 的 B1 虚拟输出（桌面音乐 + Voicemod 变声人声在此混好）。
    //    设备名须与 `ffmpeg -list_devices` 列出的完全一致（逐字精确匹配）。
    //    前提：系统默认输出设为 Voicemeeter Input；Voicemod 虚拟麦接一路硬件输入；
    //    两路都路由到 B1；监听用 A1 接物理耳机。
    void startPush() {
        const QString audioDevice = "Voicemeeter Out B1 (VB-Audio Voicemeeter VAIO)";
        m_streamer->startPush(m_compositor->outputSize(), 25, QSize(1280, 720),
                              audioDevice);
    }

    void stopPush() { m_streamer->stopPush(); }
    bool isPushing() const { return m_streamer->isPushing(); }

    // 画中画开关（合成时是否叠加相机），实时生效
    void setCameraOverlay(bool on) { m_compositor->setOverlay(on); }
    bool cameraOverlay() const { return m_compositor->overlay(); }

    // 🎯 切换相机硬件
    void setCameraDevice(const QCameraDevice &device) {
        m_cameraManager->changeCameraDevice(device);
    }

    // 📋 暴露静态函数给 UI 层填下拉框
    static QList<QCameraDevice> availableCameras() {
        return CameraManager::availableCameras();
    }

    ~DesktopCapturer() {
        stopAllPreviews();
        stopPush();

        if (m_renderThread->isRunning()) {
            QMetaObject::invokeMethod(m_compositor, "cleanup",
                                      Qt::BlockingQueuedConnection);
        }
        // ② 退出事件循环并等线程结束
        m_renderThread->quit();
        m_renderThread->wait();
        // ③ compositor 没 parent，手动删（此时已在主线程，且 GL 已清空）
        delete m_compositor;
        m_compositor = nullptr;
        // ④ 总线在渲染线程停止、compositor 删除后再删（确保无人再读）
        delete m_poseBus;
        m_poseBus = nullptr;
    }

signals:
    // 合成好的预览帧
    void previewFrame(const QImage &frame);

private:
    QScreenCapture* m_screenCapturer = nullptr;
    QMediaCaptureSession* m_screenSession = nullptr;
    CameraManager* m_cameraManager = nullptr;
    FFmpegStreamer* m_streamer = nullptr;
    GlCompositor* m_compositor = nullptr;     // ⚠️ 无 parent，手动管理
    QThread* m_renderThread = nullptr;
    PoseFrameClient* m_poseClient = nullptr;  // 骨骼检测边车客户端
    PoseBus* m_poseBus = nullptr;             // 姿态/表情数据总线（无 parent，手动删）
};

#endif // DESKTOPCAPTURER_H
