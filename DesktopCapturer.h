#ifndef DESKTOPCAPTURER_H
#define DESKTOPCAPTURER_H

#include <QObject>
#include <QScreenCapture>
#include <QMediaCaptureSession>
#include <QGuiApplication>
#include <QImage>
#include <QDebug>

#include "CameraManager.h"
#include "FFmpegStreamer.h"
#include "GlCompositor.h"

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
        m_compositor = new GlCompositor(this);
        m_screenSession->setVideoSink(m_compositor->screenSink());
        m_cameraManager->setVideoSink(m_compositor->cameraSink());

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

    // 🚀 开启推流：用合成器当前的输出尺寸告诉 ffmpeg
    void startPush() {
        m_streamer->startPush(m_compositor->outputSize(), 25);
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
    }

signals:
    // 合成好的预览帧
    void previewFrame(const QImage &frame);

private:
    QScreenCapture* m_screenCapturer = nullptr;
    QMediaCaptureSession* m_screenSession = nullptr;
    CameraManager* m_cameraManager = nullptr;
    FFmpegStreamer* m_streamer = nullptr;
    GlCompositor* m_compositor = nullptr;
};

#endif // DESKTOPCAPTURER_H
