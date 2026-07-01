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

    // 🚀 开启推流：size 是输入原始帧尺寸，fps 是帧率，
    //    outSize 是推流输出尺寸（默认无效=不缩放，与输入同尺寸）。
    //    audioDevice 是 Windows DirectShow 音频设备名（空=不推声音）。
    //      取设备名：命令行跑 ffmpeg -list_devices true -f dshow -i dummy
    //      麦克风类似 "麦克风 (Realtek Audio)"；要推“系统声音”需开启
    //      “立体声混音(Stereo Mix)”或装虚拟声卡(VB-CABLE/virtual-audio-capturer)。
    void startPush(const QSize &size, int fps = 25, const QSize &outSize = QSize(),
                   const QString &audioDevice = QString()) {
        if (m_ffmpegProcess->state() == QProcess::Running) return;

        m_frameSize = size;                    // 写帧校验用的是输入尺寸，不是输出尺寸

        // 输出尺寸：无效或与输入相同则不加 scale（省一道缩放）
        const bool doScale = outSize.isValid() && outSize != size;
        const bool hasAudio = !audioDevice.isEmpty();

        QString aliyunIP = "8.152.169.7";
        QString rtmpUrl = QString("rtmp://%1:1935/live/livestream").arg(aliyunIP);

        QStringList args;
        // —— 输入0：从 stdin 读 BGRA 原始帧 ——
        // 管道帧无时间戳，用 wallclock 打实时时间戳，便于与实时音频对齐(音画同步)
        args << "-use_wallclock_as_timestamps" << "1"
             << "-f" << "rawvideo"
             << "-pixel_format" << "bgra"      // 对应 QImage::Format_ARGB32(小端=BGRA)
             << "-video_size" << QString("%1x%2").arg(size.width()).arg(size.height())
             << "-framerate" << QString::number(fps)
             << "-i" << "-";                    // '-' 表示从标准输入读
        // —— 输入1：DirectShow 音频设备（可选）——
        if (hasAudio) {
            args << "-f" << "dshow"
                 << "-rtbufsize" << "100M"      // 🟢 为音频输入设置缓冲区，防止大流量时音频数据被 Windows 丢弃
                 << "-i" << QString("audio=%1").arg(audioDevice);
        }

        // ====== 缩放滤镜 ======
        if (doScale) {
            // 🟢 如果需要缩放，强制使用最快的算法（fast_bilinear），默认算法很吃 CPU
            args << "-vf" << QString("scale=%1:%2:flags=fast_bilinear").arg(outSize.width()).arg(outSize.height());
        }

        // ====== 视频编码器优化：空间换时间 ======
        args << "-vcodec" << "libx264"
             << "-preset" << "ultrafast"
             << "-tune" << "zerolatency"
             << "-b:v" << "1000k"         // 目标视频码率
             << "-maxrate" << "1000k"     // 最大允许码率
             << "-bufsize" << "2000k"     // 严格控制发送缓存
             << "-threads" << "4"               // 🟢 显式指定多线程编码（根据你的CPU核心数调整，如4或8）
             << "-g" << QString::number(fps * 2)// 🟢 强制设置关键帧间隔（GOP），防止网络播放端卡顿
             << "-crf" << "26"
             << "-pix_fmt" << "yuv420p";

        // ====== 音频编码与同步 ======

            args << "-map" << "0:v:0";


        // ====== 封装优化 ======
        args << "-f" << "flv"
             << "-flvflags" << "no_duration_filesize"
             << rtmpUrl;

        m_ffmpegProcess->start("ffmpeg", args);
        m_ffmpegProcess->waitForStarted();
        qDebug() << "🚀 [FFmpeg] 推流已启动 输入" << size
                 << "输出" << (doScale ? outSize : size) << fps << "fps"
                 << "音频" << (hasAudio ? audioDevice : QStringLiteral("无"));
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
