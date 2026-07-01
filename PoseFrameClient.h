#ifndef POSE_FRAME_CLIENT_H
#define POSE_FRAME_CLIENT_H

#include "PoseTypes.h"
#include "PoseBus.h"

#include <QObject>
#include <QTcpSocket>
#include <QElapsedTimer>
#include <QVideoFrame>

// 拓扑 B 的 C++ 端：把摄像头帧发给 Python 边车，收回 33 个骨骼点。
//
// 取帧：不自己开摄像头，而是 tap 现有 QVideoSink 的 videoFrameChanged 信号
//       （见 DesktopCapturer 里把 compositor->cameraSink() 连到 onFrame）。
//       该信号在渲染线程发出，本对象在主线程，Qt 自动用队列连接拷一份帧过来，
//       完全不阻塞渲染线程。
//
// 协议（与 tools/pose_sidecar/pose_server.py 对应）：
//   发送:  uint32(大端) 长度 + JPEG 字节
//   接收:  uint32(大端) 长度 + UTF-8 JSON  {"ok":bool,"lm":[[x,y,z,vis]*33]}
//
// 背压：帧率限到 ~20fps，且当 socket 写缓冲积压时直接丢帧，绝不排队堆积。
class PoseFrameClient : public QObject
{
    Q_OBJECT
public:
    explicit PoseFrameClient(QString host = QStringLiteral("127.0.0.1"),
                             quint16 port = 5066,
                             QObject* parent = nullptr);

    // 设置数据总线：收到的姿态/表情会 publish 进去，供渲染线程读取
    void setBus(PoseBus* bus) { _bus = bus; }

    // 最近一次检测结果（本对象所属线程读写）
    PoseData latest() const { return _latest; }
    FaceData latestFace() const { return _latestFace; }
    BodyData latestBody() const { return _latestBody; }

public slots:
    // 接收一帧摄像头画面（连到某个 QVideoSink::videoFrameChanged）
    void onFrame(const QVideoFrame& frame);

signals:
    // 每收到一组骨骼点就发一次
    void poseUpdated(const PoseData& pose);
    // 每收到一组面部表情就发一次（仅 holistic 模式有）
    void faceUpdated(const FaceData& face);
    // 每收到一组身体语义值就发一次
    void bodyUpdated(const BodyData& body);

private slots:
    void onConnected();
    void onReadyRead();
    void onDisconnected();

private:
    void tryConnect();
    void parseInbox();                 // 从 _rx 里拆出完整 JSON 帧

    QTcpSocket    _sock;
    QString       _host;
    quint16       _port;
    QByteArray    _rx;                 // 接收累积缓冲
    QElapsedTimer _throttle;          // 控发送帧率
    int           _minIntervalMs = 50;// 50ms ≈ 20fps
    int           _sendWidth = 640;   // 发送前缩放到的宽度（越大越准越吃算力）
    int           _jpegQuality = 85;  // 越高细节越多，检测越稳
    PoseData      _latest;
    FaceData      _latestFace;
    BodyData      _latestBody;
    PoseBus*      _bus = nullptr;
};

#endif // POSE_FRAME_CLIENT_H
