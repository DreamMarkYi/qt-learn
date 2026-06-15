#ifndef FFMPEGSTREAMER_H
#define FFMPEGSTREAMER_H

#include <QObject>
#include <QProcess>
#include <QImage>
#include <QSize>
#include <QDebug>

// 纯编码推流器：从 stdin 接收 BGRA 原始帧，用 libx264 编码后推到 RTMP。
class FFmpegStreamer : public QObject
{
    Q_OBJECT
public:
    explicit FFmpegStreamer(QObject *parent = nullptr) : QObject(parent) {
        m_ffmpegProcess = new QProcess(this);
        connect(m_ffmpegProcess, &QProcess::readyReadStandardError, this, [this]() {
            qDebug() << "FFmpeg Log:" << m_ffmpegProcess->readAllStandardError();
        });
    }

    // 🚀 开启推流：size 是输入原始帧的尺寸，fps 是帧率
    void startPush(const QSize &size, int fps = 25) {
        if (m_ffmpegProcess->state() == QProcess::Running) return;

        m_frameSize = size;

        QString aliyunIP = "8.152.169.7";
        QString rtmpUrl = QString("rtmp://%1:1935/live/livestream").arg(aliyunIP);

        QStringList args;
        // —— 输入：从 stdin 读 BGRA 原始帧 ——
        args << "-f" << "rawvideo"
             << "-pixel_format" << "bgra"      // 对应 QImage::Format_ARGB32(小端=BGRA)
             << "-video_size" << QString("%1x%2").arg(size.width()).arg(size.height())
             << "-framerate" << QString::number(fps)
             << "-i" << "-"                     // '-' 表示从标准输入读
        // —— 编码：H.264 ——
             << "-vcodec" << "libx264"
             << "-preset" << "ultrafast"
             << "-tune" << "zerolatency"
             << "-pix_fmt" << "yuv420p"         // 编码输出标准像素格式
        // —— 封装 + 目的地 ——
             << "-f" << "flv"
             << rtmpUrl;

        m_ffmpegProcess->start("ffmpeg", args);
        m_ffmpegProcess->waitForStarted();
        qDebug() << "🚀 [FFmpeg] 纯编码推流已启动" << size << fps << "fps";
    }

    // 写入一帧合成画面。返回 false 表示因背压丢帧或未在推流。
    bool writeFrame(const QImage &src) {
        if (m_ffmpegProcess->state() != QProcess::Running) return false;

        // 尺寸不符直接丢弃，避免花屏/崩溃
        if (src.size() != m_frameSize) return false;

        // 背压保护：写缓冲堆积超过 1 帧时丢帧，防止内存暴涨和延迟累积
        const qint64 frameBytes = qint64(m_frameSize.width()) * m_frameSize.height() * 4;
        if (m_ffmpegProcess->bytesToWrite() > frameBytes) return false;

        // 统一为 ARGB32，确保字节序与 bgra 对齐
        QImage img = (src.format() == QImage::Format_ARGB32)
                         ? src : src.convertToFormat(QImage::Format_ARGB32);

        // 逐行写入：QImage 每行可能有对齐填充(bytesPerLine > width*4)，
        // 必须按 width*4 写有效像素，不能直接整块写 sizeInBytes()。
        const int rowBytes = img.width() * 4;
        for (int y = 0; y < img.height(); ++y) {
            m_ffmpegProcess->write(
                reinterpret_cast<const char*>(img.constScanLine(y)), rowBytes);
        }
        return true;
    }

    // 🛑 停止网络推流
    void stopPush() {
        if (m_ffmpegProcess->state() == QProcess::Running) {
            qDebug() << "⏹️ [FFmpeg] 正在关闭推流器...";
            m_ffmpegProcess->closeWriteChannel();   // 关闭 stdin，ffmpeg 收到 EOF 自然退出
            if (!m_ffmpegProcess->waitForFinished(5000))
                m_ffmpegProcess->kill();
            qDebug() << "⏹️ [FFmpeg] 推流已断开";
        }
    }

    bool isPushing() const {
        return m_ffmpegProcess->state() == QProcess::Running;
    }

    ~FFmpegStreamer() { stopPush(); }

private:
    QProcess* m_ffmpegProcess = nullptr;
    QSize     m_frameSize;
};

#endif // FFMPEGSTREAMER_H
