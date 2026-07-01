#ifndef FRAME_PROFILER_H
#define FRAME_PROFILER_H

#include <QByteArray>
#include <QString>
#include <QDebug>
#include <vector>

// 极简「渲染分步计时器」（诊断用）。
// 用法：每帧对各阶段调用 mark(name, ns) 累加耗时；帧末调 endFrame(N)，
// 每满 N 帧就打印一次各桶的「平均 / 最大」毫秒数并清零。
//
// 注意：配合调用方在每段后插 glFinish 才能把异步 GPU 时间归到对应段——
// 这会把流水线串行化、令「合计/帧」偏高、不代表真实运行帧时间；但各段的
// 相对占比可信，足以定位瓶颈。诊断完应关闭计时（见 GlCompositor::m_profiling）。
class FrameProfiler
{
public:
    void mark(const char* name, qint64 ns) {
        Bucket& b = bucket(name);
        b.sumNs += ns;
        if (ns > b.maxNs) b.maxNs = ns;
        ++b.n;
    }

    // 每帧末调用一次；满 reportEvery 帧汇总打印并清零。
    void endFrame(int reportEvery) {
        if (++_frames < reportEvery) return;

        QString line = QStringLiteral("[Profiler] 近 %1 帧均值(ms): ").arg(_frames);
        qint64 totalSum = 0;
        for (const Bucket& b : _buckets) {
            if (b.n == 0) continue;
            const double avg = double(b.sumNs) / 1.0e6 / b.n;
            const double mx  = double(b.maxNs) / 1.0e6;
            line += QStringLiteral("%1=%2(max%3) ")
                        .arg(QString::fromLatin1(b.name))
                        .arg(avg, 0, 'f', 2)
                        .arg(mx, 0, 'f', 2);
            totalSum += b.sumNs;
        }
        line += QStringLiteral("| 合计/帧=%1")
                    .arg(double(totalSum) / 1.0e6 / _frames, 0, 'f', 2);
        qDebug().noquote() << line;

        _buckets.clear();
        _frames = 0;
    }

private:
    struct Bucket { QByteArray name; qint64 sumNs = 0; qint64 maxNs = 0; int n = 0; };

    Bucket& bucket(const char* name) {
        for (Bucket& b : _buckets)
            if (b.name == name) return b;
        _buckets.push_back(Bucket{QByteArray(name), 0, 0, 0});
        return _buckets.back();
    }

    std::vector<Bucket> _buckets;
    int _frames = 0;
};

#endif // FRAME_PROFILER_H
