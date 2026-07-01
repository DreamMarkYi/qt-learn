#include "PoseFrameClient.h"

#include <QBuffer>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QtEndian>
#include <QDebug>

PoseFrameClient::PoseFrameClient(QString host, quint16 port, QObject* parent)
    : QObject(parent), _host(std::move(host)), _port(port)
{
    qRegisterMetaType<PoseData>("PoseData");
    qRegisterMetaType<FaceData>("FaceData");
    qRegisterMetaType<BodyData>("BodyData");

    connect(&_sock, &QTcpSocket::connected,    this, &PoseFrameClient::onConnected);
    connect(&_sock, &QTcpSocket::readyRead,     this, &PoseFrameClient::onReadyRead);
    connect(&_sock, &QTcpSocket::disconnected,  this, &PoseFrameClient::onDisconnected);

    _throttle.start();
    tryConnect();
}

void PoseFrameClient::tryConnect()
{
    if (_sock.state() == QAbstractSocket::UnconnectedState) {
        qInfo() << "[PoseFrameClient] 连接 Python 边车" << _host << _port;
        _sock.connectToHost(_host, _port);
    }
}

void PoseFrameClient::onConnected()
{
    _sock.setSocketOption(QAbstractSocket::LowDelayOption, 1);   // TCP_NODELAY
    qInfo() << "[PoseFrameClient] 已连接 Python 边车";
}

void PoseFrameClient::onDisconnected()
{
    qWarning() << "[PoseFrameClient] 与边车断开，2s 后重连";
    _rx.clear();
    QTimer::singleShot(2000, this, &PoseFrameClient::tryConnect);
}

void PoseFrameClient::onFrame(const QVideoFrame& frame)
{
    // 1) 节流：限帧率
    if (_throttle.elapsed() < _minIntervalMs) return;

    // 2) 背压：没连上 / 写缓冲还有大量未发出去，直接丢这一帧
    if (_sock.state() != QAbstractSocket::ConnectedState) return;
    if (_sock.bytesToWrite() > 256 * 1024) return;

    // 3) 转 QImage 并缩小
    QImage img = frame.toImage();
    if (img.isNull()) return;
    if (img.width() > _sendWidth)
        img = img.scaledToWidth(_sendWidth, Qt::SmoothTransformation);

    // 4) 编码 JPEG 到内存
    QByteArray jpeg;
    {
        QBuffer buf(&jpeg);
        buf.open(QIODevice::WriteOnly);
        if (!img.save(&buf, "JPG", _jpegQuality)) return;
    }

    // 5) 发送：4 字节大端长度 + JPEG
    quint32 len = qToBigEndian<quint32>(static_cast<quint32>(jpeg.size()));
    _sock.write(reinterpret_cast<const char*>(&len), 4);
    _sock.write(jpeg);

    _throttle.restart();
}

void PoseFrameClient::onReadyRead()
{
    _rx.append(_sock.readAll());
    parseInbox();
}

void PoseFrameClient::parseInbox()
{
    // 循环拆包：每个包 = 4 字节大端长度 + 该长度的 JSON
    for (;;) {
        if (_rx.size() < 4) return;
        quint32 len = qFromBigEndian<quint32>(
            reinterpret_cast<const uchar*>(_rx.constData()));
        if (_rx.size() < int(4 + len)) return;          // 还没收全

        QByteArray json = _rx.mid(4, int(len));
        _rx.remove(0, int(4 + len));

        QJsonParseError err{};
        const QJsonObject obj = QJsonDocument::fromJson(json, &err).object();
        if (err.error != QJsonParseError::NoError) continue;

        // ---- 身体 33 点 ----
        PoseData d;
        d.ok = obj.value("ok").toBool();
        if (d.ok) {
            const QJsonArray arr = obj.value("lm").toArray();
            for (int i = 0; i < 33 && i < arr.size(); ++i) {
                const QJsonArray p = arr.at(i).toArray();
                if (p.size() < 4) continue;
                d.lm[i] = { float(p.at(0).toDouble()), float(p.at(1).toDouble()),
                            float(p.at(2).toDouble()), float(p.at(3).toDouble()) };
            }
        }
        _latest = d;
        emit poseUpdated(d);
        if (_bus) _bus->publishPose(d);

        // ---- 面部表情（holistic 模式才有 "face" 段）----
        if (obj.contains("face")) {
            const QJsonObject fo = obj.value("face").toObject();
            FaceData f;
            f.ok = fo.value("ok").toBool();
            if (f.ok) {
                f.eyeLOpen  = float(fo.value("eyeLOpen").toDouble(1.0));
                f.eyeROpen  = float(fo.value("eyeROpen").toDouble(1.0));
                f.eyeballX  = float(fo.value("eyeballX").toDouble());
                f.eyeballY  = float(fo.value("eyeballY").toDouble());
                f.mouthOpen = float(fo.value("mouthOpen").toDouble());
                f.mouthForm = float(fo.value("mouthForm").toDouble());
                f.browL     = float(fo.value("browL").toDouble());
                f.browR     = float(fo.value("browR").toDouble());
                f.cheek     = float(fo.value("cheek").toDouble());
                f.angleX    = float(fo.value("angleX").toDouble());
                f.angleY    = float(fo.value("angleY").toDouble());
                f.angleZ    = float(fo.value("angleZ").toDouble());
            }
            _latestFace = f;
            emit faceUpdated(f);
            if (_bus) _bus->publishFace(f);
        }

        // ---- 身体语义值（目前只有 ParamBodyAngleZ）----
        if (obj.contains("body")) {
            const QJsonObject bo = obj.value("body").toObject();
            BodyData b;
            b.ok = bo.value("ok").toBool();
            if (b.ok) {
                b.angleZ = float(bo.value("angleZ").toDouble());
            }
            _latestBody = b;
            emit bodyUpdated(b);
            if (_bus) _bus->publishBody(b);
        }
    }
}
