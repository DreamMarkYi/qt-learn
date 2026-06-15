#ifndef CAMERAMANAGER_H
#define CAMERAMANAGER_H

#include <QObject>
#include <QCamera>
#include <QMediaDevices>
#include <QCameraDevice>
#include <QMediaCaptureSession>
#include <QVideoWidget>
#include <QVideoSink>
#include <QDebug>

class CameraManager : public QObject
{
    Q_OBJECT
public:
    explicit CameraManager(QObject *parent = nullptr) : QObject(parent) {
        m_session = new QMediaCaptureSession(this);

        // 获取系统默认摄像头并初始化
        QCameraDevice defaultCam = QMediaDevices::defaultVideoInput();
        m_camera = new QCamera(defaultCam, this);
        m_session->setCamera(m_camera);

        // 绑定错误诊断信号
        connect(m_camera, &QCamera::errorOccurred, this,
                [](QCamera::Error err, const QString &msg) {
                    qWarning() << "❌ [Camera] 硬件组件报错：" << err << msg;
                });
    }

    // 绑定本地的小视频渲染窗口
    void setVideoOutput(QVideoWidget *videoWidget) {
        m_session->setVideoOutput(videoWidget);
    }

    // 把相机画面输出到指定的 sink（供本地合成器取帧）
    void setVideoSink(QVideoSink *sink) {
        m_session->setVideoSink(sink);
    }

    // 开启本地相机预览
    void start() {
        if (m_camera) {
            m_camera->start();
            qDebug() << "▶️ [Camera] 本地摄像头预览已开启";
        }
    }

    // 停止本地相机预览
    void stop() {
        if (m_camera) {
            m_camera->stop();
            qDebug() << "⏹️ [Camera] 本地摄像头预览已关闭";
        }
    }

    // 获取当前正在使用的摄像头名字（给 FFmpeg 传参用）
    QString currentCameraDescription() const {
        return m_camera ? m_camera->cameraDevice().description() : "";
    }

    // 获取当前正在使用的摄像头 ID
    QString currentCameraId() const {
        return m_camera ? m_camera->cameraDevice().id() : "";
    }

    // 📋 静态函数：列出系统当前所有可用的摄像头列表
    static QList<QCameraDevice> availableCameras() {
        return QMediaDevices::videoInputs();
    }

    // 🎯 核心功能：切换到指定的摄像头硬件
    // 返回值表示是否成功执行了切换动作
    bool changeCameraDevice(const QCameraDevice &device) {
        if (device.isNull() || !m_camera) return false;

        // 如果已经是当前设备，则无视
        if (m_camera->cameraDevice().id() == device.id()) return false;

        bool wasActive = m_camera->isActive();

        m_camera->stop();
        m_camera->setCameraDevice(device);
        qDebug() << "🎯 [Camera] 硬件已成功更换为：" << device.description();

        if (wasActive) {
            m_camera->start(); // 如果切换前是开着的，换完硬件后自动恢复预览
        }
        return true;
    }

    ~CameraManager() {
        stop();
    }

private:
    QCamera* m_camera = nullptr;
    QMediaCaptureSession* m_session = nullptr;
};

#endif // CAMERAMANAGER_H