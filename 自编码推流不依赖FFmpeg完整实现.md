# 自编码推流(不依赖 FFmpeg)完整实现

> 目标:把 GPU 合成的画面 + 系统声音,**自己编码成 H.264 / AAC**,自己封装 **FLV**,用 **librtmp** 推到 RTMP 服务器。
> 全程不启动 `ffmpeg.exe`,不用 `libx264`,不用管道,从根上消除「音频合并卡顿」。
>
> 本文所有代码都写在本 md 内,**不改动你现有任何源码**。你照着新增几个头文件即可。

---

## 0. 总体架构

```
                              ┌───────────────────────────┐
GL合成纹理 ──readback/interop─▶│ MfH264Encoder (H.264 MFT) │─Annex-B─┐
(GlCompositor)                └───────────────────────────┘         │  NALU
                                                                     ▼
                                                            ┌─────────────────┐
WASAPI Loopback ─PCM─▶ MfAacEncoder (AAC MFT) ─AAC frame──▶│   FlvMuxer       │
(系统声音,补静音)                                          │  生成 FLV tag    │
                                                            └────────┬────────┘
                                                                     ▼ FLV tag 队列
                                                            ┌─────────────────┐
                                                            │  RtmpPusher     │ 单线程
                                                            │  librtmp 推流    │
                                                            └─────────────────┘
```

模块清单:

| 文件 | 职责 |
|---|---|
| `StreamClock.h` | 统一毫秒时钟,音视频共用 |
| `FlvMuxer.h` | 把 H264/AAC 裸流封装成 FLV tag(含 seq header) |
| `MfH264Encoder.h` | Media Foundation H.264 编码(BGRA→NV12→H264) |
| `MfAacEncoder.h` | Media Foundation AAC 编码(PCM→AAC) |
| `WasapiLoopback.h` | 抓系统声音,定时补静音,输出恒定速率 PCM |
| `RtmpPusher.h` | librtmp 封装,单线程发送 FLV tag |
| `SelfEncodeStreamer.h` | 把上面串起来,对外暴露 `writeFrame(QImage)` |

> **为什么选 Media Foundation:** Windows 自带、零额外依赖、有硬件时自动走硬件(Intel/AMD/NV 都能用)。N 卡想要极致低延迟可换 NVENC,接口替换 `MfH264Encoder` 即可,FLV/RTMP 部分不动。本文第 9 节给出 NVENC 切换要点。

---

## 1. 依赖与编译配置

### 1.1 librtmp
推荐用 vcpkg 安装:

```bash
vcpkg install librtmp:x64-windows
```

### 1.2 CMakeLists.txt 追加(示例,按你的工程调整)

```cmake
# —— Media Foundation / WASAPI 系统库 ——
target_link_libraries(你的target PRIVATE
    mf mfplat mfuuid mfreadwrite wmcodecdspuuid
    ole32 oleaut32 ksuser   # WASAPI / COM
)

# —— librtmp(vcpkg)——
find_package(librtmp CONFIG REQUIRED)   # 或手动 find_library
target_link_libraries(你的target PRIVATE librtmp::librtmp)
```

> MSVC 下也可以用 `#pragma comment(lib, "mfplat.lib")` 等就地链接,见各头文件顶部。

---

## 2. StreamClock.h —— 统一时钟

音视频时间戳必须来自同一基准,否则音画对不上。

```cpp
#ifndef STREAMCLOCK_H
#define STREAMCLOCK_H

#include <chrono>

// 统一毫秒时钟:start() 后,nowMs() 返回自起点的毫秒数(单调递增)。
class StreamClock {
public:
    void start() { m_start = clock::now(); m_started = true; }
    bool started() const { return m_started; }

    // 当前相对毫秒(用于 FLV timestamp / MF sample time 基准)
    uint32_t nowMs() const {
        using namespace std::chrono;
        return (uint32_t)duration_cast<milliseconds>(clock::now() - m_start).count();
    }
    // 当前相对 100ns(Media Foundation 的时间单位)
    int64_t now100ns() const {
        using namespace std::chrono;
        return duration_cast<duration<int64_t, std::ratio<1,10000000>>>(clock::now() - m_start).count();
    }
private:
    using clock = std::chrono::steady_clock;
    clock::time_point m_start{};
    bool m_started = false;
};

#endif
```

---

## 3. FlvMuxer.h —— FLV 封装

librtmp 的 `RTMP_Write` **直接吃 FLV tag 字节流**:它解析 11 字节 tag 头自己做 RTMP 分块。所以我们只要产出标准 FLV tag(`11字节头 + body + 4字节 PreviousTagSize`)即可。

> **关键坑都在这里**:AVCC vs Annex-B、SPS/PPS 配置记录、AudioSpecificConfig、关键帧标记。

```cpp
#ifndef FLVMUXER_H
#define FLVMUXER_H

#include <cstdint>
#include <vector>
#include <cstring>

// 产出标准 FLV tag(含 11 字节头 + body + 4 字节 PreviousTagSize),
// 直接交给 librtmp 的 RTMP_Write。
class FlvMuxer {
public:
    using Bytes = std::vector<uint8_t>;

    // ---------- 视频 ----------
    // AVC sequence header(只发一次):用 SPS/PPS 构造 AVCDecoderConfigurationRecord
    Bytes videoSeqHeader(const Bytes& sps, const Bytes& pps, uint32_t ts = 0) {
        Bytes cfg = buildAvcConfig(sps, pps);
        Bytes body;
        body.push_back(0x17);          // frameType=1(key) | codecID=7(AVC)
        body.push_back(0x00);          // AVCPacketType=0 (seq header)
        put24(body, 0);                // CompositionTime=0
        body.insert(body.end(), cfg.begin(), cfg.end());
        return makeTag(0x09, body, ts);
    }

    // 一帧 H.264(传入 AVCC 格式的 NALU 串,见 MfH264Encoder 转换)
    Bytes videoNalu(const Bytes& avccData, bool keyframe, uint32_t ts) {
        Bytes body;
        body.push_back(keyframe ? 0x17 : 0x27);  // key=0x17 / inter=0x27,低4位=7(AVC)
        body.push_back(0x01);                     // AVCPacketType=1 (NALU)
        put24(body, 0);                           // CompositionTime=0(无B帧,DTS=PTS)
        body.insert(body.end(), avccData.begin(), avccData.end());
        return makeTag(0x09, body, ts);
    }

    // ---------- 音频 ----------
    // AAC sequence header(只发一次):AudioSpecificConfig
    Bytes audioSeqHeader(const Bytes& asc, uint32_t ts = 0) {
        Bytes body;
        body.push_back(0xAF);          // AAC,44/48kHz,16bit,Stereo(FLV 规定 AAC 固定 0xAF)
        body.push_back(0x00);          // AACPacketType=0 (seq header)
        body.insert(body.end(), asc.begin(), asc.end());
        return makeTag(0x08, body, ts);
    }

    // 一帧 AAC(裸 AAC,不带 ADTS)
    Bytes audioFrame(const Bytes& aac, uint32_t ts) {
        Bytes body;
        body.push_back(0xAF);
        body.push_back(0x01);          // AACPacketType=1 (raw)
        body.insert(body.end(), aac.begin(), aac.end());
        return makeTag(0x08, body, ts);
    }

    // ---------- 工具:AVCDecoderConfigurationRecord ----------
    static Bytes buildAvcConfig(const Bytes& sps, const Bytes& pps) {
        Bytes c;
        c.push_back(0x01);                 // configurationVersion
        c.push_back(sps.size() > 1 ? sps[1] : 0x42); // AVCProfileIndication
        c.push_back(sps.size() > 2 ? sps[2] : 0x00); // profile_compatibility
        c.push_back(sps.size() > 3 ? sps[3] : 0x1f); // AVCLevelIndication
        c.push_back(0xFF);                 // 6bit reserved + lengthSizeMinusOne=3
        c.push_back(0xE1);                 // 3bit reserved + numOfSPS=1
        put16(c, (uint16_t)sps.size());
        c.insert(c.end(), sps.begin(), sps.end());
        c.push_back(0x01);                 // numOfPPS=1
        put16(c, (uint16_t)pps.size());
        c.insert(c.end(), pps.begin(), pps.end());
        return c;
    }

private:
    static void put16(Bytes& b, uint16_t v) { b.push_back(v>>8); b.push_back(v&0xff); }
    static void put24(Bytes& b, uint32_t v) { b.push_back(v>>16); b.push_back(v>>8); b.push_back(v&0xff); }
    static void put32(Bytes& b, uint32_t v) { b.push_back(v>>24); b.push_back(v>>16); b.push_back(v>>8); b.push_back(v&0xff); }

    // 组装一个完整 FLV tag:
    //  [TagType(1)][DataSize(3)][Timestamp(3)][TimestampExt(1)][StreamID(3)] + body + [PrevTagSize(4)]
    Bytes makeTag(uint8_t type, const Bytes& body, uint32_t ts) {
        Bytes t;
        t.push_back(type);
        put24(t, (uint32_t)body.size());
        put24(t, ts & 0xFFFFFF);           // 低 24 位
        t.push_back((ts >> 24) & 0xFF);    // 高 8 位扩展
        put24(t, 0);                       // StreamID 固定 0
        t.insert(t.end(), body.begin(), body.end());
        put32(t, (uint32_t)(11 + body.size())); // PreviousTagSize
        return t;
    }
};

#endif
```

---

## 4. MfH264Encoder.h —— H.264 编码

用 Media Foundation 的 **H.264 Encoder MFT**(`CLSID_CMSH264EncoderMFT`,同步软编)。
输入要求 **NV12**,这里附带一个 BGRA→NV12 的 CPU 转换(简单可用;GPU 优化见注释)。
输出是 **Annex-B**,我们解析 NALU,提取 SPS/PPS 一次性做 seq header,其余帧转成 **AVCC** 交给 FlvMuxer。

```cpp
#ifndef MFH264ENCODER_H
#define MFH264ENCODER_H

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <vector>
#include <functional>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

class MfH264Encoder {
public:
    using Bytes = std::vector<uint8_t>;
    // 编码完成回调:avccData=AVCC NALU 串, keyframe, ts(毫秒)
    std::function<void(const Bytes& avcc, bool keyframe, uint32_t tsMs)> onEncoded;
    // 首帧拿到 SPS/PPS 时回调一次(用于发 seq header)
    std::function<void(const Bytes& sps, const Bytes& pps)> onSeqHeader;

    bool init(int w, int h, int fps, int bitrateBps) {
        m_w = w; m_h = h; m_fps = fps;
        MFStartup(MF_VERSION);

        if (FAILED(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&m_mft)))) return false;

        // —— 必须先设输出类型 ——
        ComPtr<IMFMediaType> outType;
        MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outType->SetUINT32(MF_MT_AVG_BITRATE, bitrateBps);
        MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, w, h);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base); // Baseline,兼容性最好
        if (FAILED(m_mft->SetOutputType(0, outType.Get(), 0))) return false;

        // —— 再设输入类型 NV12 ——
        ComPtr<IMFMediaType> inType;
        MFCreateMediaType(&inType);
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, w, h);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(m_mft->SetInputType(0, inType.Get(), 0))) return false;

        // 低延迟:关 B 帧,实时
        if (ComPtr<ICodecAPI> codec; SUCCEEDED(m_mft.As(&codec))) {
            VARIANT v; v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
            codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
            VARIANT b; b.vt = VT_UI4; b.ulVal = 0;
            codec->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &b);
        }

        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        m_nv12.resize(w * h * 3 / 2);
        return true;
    }

    // 输入一帧 BGRA(stride 可能 > w*4)
    void encodeBGRA(const uint8_t* bgra, int stride, int64_t time100ns, uint32_t tsMs) {
        bgraToNV12(bgra, stride, m_nv12.data());
        // 构造输入 sample
        ComPtr<IMFMediaBuffer> buf;
        MFCreateMemoryBuffer((DWORD)m_nv12.size(), &buf);
        BYTE* p = nullptr; DWORD maxLen = 0;
        buf->Lock(&p, &maxLen, nullptr);
        memcpy(p, m_nv12.data(), m_nv12.size());
        buf->Unlock();
        buf->SetCurrentLength((DWORD)m_nv12.size());

        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buf.Get());
        sample->SetSampleTime(time100ns);
        sample->SetSampleDuration(10000000LL / m_fps);

        if (m_mft->ProcessInput(0, sample.Get(), 0) == MF_E_NOTACCEPTING) {
            drainOutput(tsMs);                       // 先取走输出再喂
            m_mft->ProcessInput(0, sample.Get(), 0);
        }
        drainOutput(tsMs);
    }

    void shutdown() {
        if (m_mft) {
            m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            m_mft.Reset();
        }
    }

private:
    // 取走所有可用输出
    void drainOutput(uint32_t tsMs) {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO info{};
            m_mft->GetOutputStreamInfo(0, &info);

            MFT_OUTPUT_DATA_BUFFER out{};
            ComPtr<IMFSample> outSample;
            ComPtr<IMFMediaBuffer> outBuf;
            // MFT 不自分配则我们分配
            bool selfAlloc = !(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                               MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));
            if (selfAlloc) {
                MFCreateSample(&outSample);
                MFCreateMemoryBuffer(info.cbSize ? info.cbSize : (m_w*m_h), &outBuf);
                outSample->AddBuffer(outBuf.Get());
                out.pSample = outSample.Get();
            }
            DWORD status = 0;
            HRESULT hr = m_mft->ProcessOutput(0, 1, &out, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { handleStreamChange(); continue; }
            if (FAILED(hr)) break;

            ComPtr<IMFSample> s = out.pSample;
            // 第一次输出后,SPS/PPS 已可从输出类型拿到
            ensureSeqHeader();

            // 读出 Annex-B 数据
            ComPtr<IMFMediaBuffer> b;
            s->ConvertToContiguousBuffer(&b);
            BYTE* p = nullptr; DWORD len = 0;
            b->Lock(&p, nullptr, &len);
            int64_t st = 0; s->GetSampleTime(&st);
            uint32_t ts = (uint32_t)(st / 10000);    // 100ns→ms
            emitFrame(p, len, ts);
            b->Unlock();

            if (!selfAlloc && out.pSample) out.pSample->Release();
            if (out.pEvents) out.pEvents->Release();
        }
    }

    void handleStreamChange() {
        ComPtr<IMFMediaType> t;
        m_mft->GetOutputAvailableType(0, 0, &t);
        m_mft->SetOutputType(0, t.Get(), 0);
    }

    // 从输出当前类型的 MF_MT_MPEG_SEQUENCE_HEADER 提取 SPS/PPS(Annex-B)
    void ensureSeqHeader() {
        if (m_seqDone) return;
        ComPtr<IMFMediaType> t;
        if (FAILED(m_mft->GetOutputCurrentType(0, &t))) return;
        UINT32 sz = 0;
        if (FAILED(t->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &sz)) || sz == 0) return;
        std::vector<uint8_t> blob(sz);
        t->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), sz, nullptr);
        // blob 是 Annex-B 的 SPS/PPS,逐个解析
        Bytes sps, pps;
        forEachNalu(blob.data(), (int)blob.size(), [&](const uint8_t* n, int l){
            int type = n[0] & 0x1F;
            if (type == 7) sps.assign(n, n + l);
            else if (type == 8) pps.assign(n, n + l);
        });
        if (!sps.empty() && !pps.empty()) {
            if (onSeqHeader) onSeqHeader(sps, pps);
            m_seqDone = true;
        }
    }

    // 把一帧 Annex-B 拆成 NALU,丢弃 AUD/SPS/PPS,其余转 AVCC 输出
    void emitFrame(const uint8_t* data, int len, uint32_t ts) {
        Bytes avcc;
        bool key = false;
        forEachNalu(data, len, [&](const uint8_t* n, int l){
            int type = n[0] & 0x1F;
            if (type == 9 || type == 7 || type == 8) return; // AUD/SPS/PPS 不进帧体
            if (type == 5) key = true;                        // IDR
            // AVCC:4 字节大端长度 + NALU
            avcc.push_back((l>>24)&0xff); avcc.push_back((l>>16)&0xff);
            avcc.push_back((l>>8)&0xff);  avcc.push_back(l&0xff);
            avcc.insert(avcc.end(), n, n + l);
        });
        if (!avcc.empty() && onEncoded) onEncoded(avcc, key, ts);
    }

    // 遍历 Annex-B(支持 3/4 字节起始码),回调每个 NALU(去掉起始码)
    template<class F>
    static void forEachNalu(const uint8_t* d, int len, F cb) {
        int i = 0;
        auto isStart = [&](int p, int& sc)->bool{
            if (p+3 <= len && d[p]==0 && d[p+1]==0 && d[p+2]==1) { sc=3; return true; }
            if (p+4 <= len && d[p]==0 && d[p+1]==0 && d[p+2]==0 && d[p+3]==1) { sc=4; return true; }
            return false;
        };
        int sc = 0;
        while (i < len && !isStart(i, sc)) ++i;
        while (i < len) {
            i += sc;
            int start = i;
            int s2 = 0;
            while (i < len && !isStart(i, s2)) ++i;
            int naluLen = i - start;
            if (naluLen > 0) cb(d + start, naluLen);
            sc = s2;
        }
    }

    // —— BGRA → NV12(BT.601 limited),CPU 版。性能敏感可改 GPU/shader 输出 NV12 ——
    void bgraToNV12(const uint8_t* bgra, int stride, uint8_t* nv12) {
        const int w = m_w, h = m_h;
        uint8_t* y = nv12;
        uint8_t* uv = nv12 + w * h;
        for (int j = 0; j < h; ++j) {
            const uint8_t* row = bgra + j * stride;
            for (int i = 0; i < w; ++i) {
                int B = row[i*4+0], G = row[i*4+1], R = row[i*4+2];
                int Y = ((66*R + 129*G + 25*B + 128) >> 8) + 16;
                y[j*w + i] = (uint8_t)clamp8(Y);
                if ((j & 1) == 0 && (i & 1) == 0) {
                    int U = ((-38*R - 74*G + 112*B + 128) >> 8) + 128;
                    int V = ((112*R - 94*G - 18*B + 128) >> 8) + 128;
                    int idx = (j/2) * w + (i/2)*2;
                    uv[idx]   = (uint8_t)clamp8(U);
                    uv[idx+1] = (uint8_t)clamp8(V);
                }
            }
        }
    }
    static int clamp8(int v){ return v<0?0:(v>255?255:v); }

    ComPtr<IMFTransform> m_mft;
    std::vector<uint8_t> m_nv12;
    int m_w=0, m_h=0, m_fps=25;
    bool m_seqDone=false;
};

#endif
```

> **性能提示**:`bgraToNV12` 是纯 CPU,1080p 下有一定开销。最优做法是在 `GlCompositor` 的合成 shader 里直接输出 NV12 两张纹理(Y 全分辨率 + UV 半分辨率),`glReadPixels` 直接拿 NV12,省掉这一步 CPU 转换。再进一步是 WGL_NV_DX_interop 把纹理共享给 D3D11,启用 MF 硬件异步编码器全程零回读(见第 9 节)。

---

## 5. WasapiLoopback.h —— 系统声音采集(恒定速率)

抓「系统正在播放的声音」。**核心是定时补静音**:无声播放时 loopback 不给数据,我们按墙钟补零,保证喂给 AAC 编码器的是一条恒定 48kHz、永不中断的 PCM 流。

```cpp
#ifndef WASAPILOOPBACK_H
#define WASAPILOOPBACK_H

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>

#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

// 输出格式固定:48000Hz, 立体声, 16bit PCM。
// 回调按固定 10ms 节拍给出 PCM(不足补静音),pts 毫秒。
class WasapiLoopback {
public:
    // pcm: 交错 s16le 立体声; samplesPerCh: 每声道样本数; tsMs: 该块起始毫秒
    std::function<void(const int16_t* pcm, int samplesPerCh, uint32_t tsMs)> onPcm;

    static constexpr int kRate = 48000;
    static constexpr int kCh   = 2;

    bool start() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&en)))) return false;
        ComPtr<IMMDevice> dev;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return false; // eRender=输出设备做loopback
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &m_client))) return false;

        WAVEFORMATEX* mix = nullptr;
        m_client->GetMixFormat(&mix);
        m_mixRate = mix->nSamplesPerSec;
        m_mixCh   = mix->nChannels;
        m_mixFloat = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                     (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                      reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

        REFERENCE_TIME dur = 10 * 10000; // 10ms 缓冲
        HRESULT hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                          AUDCLNT_STREAMFLAGS_LOOPBACK,
                                          dur, 0, mix, nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr)) return false;
        if (FAILED(m_client->GetService(IID_PPV_ARGS(&m_capture)))) return false;

        m_client->Start();
        m_run = true;
        m_thread = std::thread([this]{ loop(); });
        return true;
    }

    void stop() {
        m_run = false;
        if (m_thread.joinable()) m_thread.join();
        if (m_client) m_client->Stop();
    }
    ~WasapiLoopback(){ stop(); }

private:
    void loop() {
        using clock = std::chrono::steady_clock;
        auto t0 = clock::now();
        int64_t emitted = 0;                 // 已输出的每声道样本数
        std::vector<int16_t> block;          // 复用缓冲

        while (m_run) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

            // 1) 把 loopback 当前可读数据全部转换进 m_pending(48k/2ch s16)
            UINT32 packet = 0;
            m_capture->GetNextPacketSize(&packet);
            while (packet) {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                if (FAILED(m_capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                bool silent = flags & AUDCLNT_BUFFERFLAGS_SILENT;
                appendConverted(silent ? nullptr : data, frames);
                m_capture->ReleaseBuffer(frames);
                m_capture->GetNextPacketSize(&packet);
            }

            // 2) 按墙钟算「到现在应当输出多少样本」,不足从 m_pending 取,再不足补静音
            int64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now()-t0).count();
            int64_t target = elapsedMs * kRate / 1000;
            int64_t need = target - emitted;
            if (need <= 0) continue;

            block.assign((size_t)need * kCh, 0);
            size_t avail = m_pending.size() / kCh;
            size_t take = std::min<size_t>(avail, (size_t)need);
            if (take) {
                memcpy(block.data(), m_pending.data(), take * kCh * sizeof(int16_t));
                m_pending.erase(m_pending.begin(), m_pending.begin() + take * kCh);
            }
            // 其余已是 0(静音补齐)
            uint32_t tsMs = (uint32_t)(emitted * 1000 / kRate);
            if (onPcm) onPcm(block.data(), (int)need, tsMs);
            emitted += need;
        }
    }

    // 把设备原始格式(可能 float/不同声道)转成 48k/2ch s16 追加到 m_pending
    // 注:此处假设设备就是 48000Hz(绝大多数 Win10 默认)。若不是需做重采样。
    void appendConverted(const BYTE* data, UINT32 frames) {
        if (m_mixRate != kRate) {
            // 简化处理:这里不做重采样,生产环境请用 MF Resampler / soxr。
            // 为保证速率,仍按 frames 推进等量静音,避免时间轴漂移。
        }
        size_t base = m_pending.size();
        m_pending.resize(base + (size_t)frames * kCh, 0);
        int16_t* dst = m_pending.data() + base;
        if (!data) return; // 静音包,已填 0

        for (UINT32 i = 0; i < frames; ++i) {
            float l, r;
            if (m_mixFloat) {
                const float* f = reinterpret_cast<const float*>(data) + (size_t)i * m_mixCh;
                l = f[0]; r = (m_mixCh > 1) ? f[1] : f[0];
            } else {
                const int16_t* s = reinterpret_cast<const int16_t*>(data) + (size_t)i * m_mixCh;
                l = s[0] / 32768.0f; r = (m_mixCh > 1) ? s[1] / 32768.0f : l;
            }
            dst[i*2+0] = (int16_t)clampF(l);
            dst[i*2+1] = (int16_t)clampF(r);
        }
    }
    static float clampF(float v){ v*=32767.f; return v<-32768?-32768:(v>32767?32767:v); }

    ComPtr<IAudioClient>        m_client;
    ComPtr<IAudioCaptureClient> m_capture;
    std::vector<int16_t>        m_pending;   // 已转成 48k/2ch s16 的待输出数据
    std::thread m_thread;
    std::atomic<bool> m_run{false};
    UINT32 m_mixRate=48000, m_mixCh=2; bool m_mixFloat=true;
};

#endif
```

> 若你的声卡默认不是 48000Hz(少数情况),需在 `appendConverted` 里接一个重采样器(MF 的 `CLSID_CResamplerMediaObject` 或 soxr)。代码里已留位置和说明。

---

## 6. MfAacEncoder.h —— AAC 编码

Media Foundation **AAC Encoder MFT**。输入 PCM(48k/2ch/16bit),输出裸 AAC(每帧 1024 样本)。AudioSpecificConfig 从输出类型的 `MF_MT_USER_DATA` 取(跳过前 12 字节 HEAACWAVEINFO)。

```cpp
#ifndef MFAACENCODER_H
#define MFAACENCODER_H

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#include <vector>
#include <functional>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

using Microsoft::WRL::ComPtr;

class MfAacEncoder {
public:
    using Bytes = std::vector<uint8_t>;
    std::function<void(const Bytes& aac, uint32_t tsMs)> onEncoded;
    std::function<void(const Bytes& asc)> onSeqHeader; // AudioSpecificConfig

    // rate=48000, ch=2, bitrate 例 128000
    bool init(int rate, int ch, int bitrate) {
        m_rate = rate; m_ch = ch;
        MFStartup(MF_VERSION);
        if (FAILED(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&m_mft)))) return false;

        // 输入:PCM
        ComPtr<IMFMediaType> in;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
        if (FAILED(m_mft->SetInputType(0, in.Get(), 0))) return false;

        // 输出:AAC
        ComPtr<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
        out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrate / 8);
        out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);           // 0=裸 AAC(无 ADTS)
        if (FAILED(m_mft->SetOutputType(0, out.Get(), 0))) return false;

        extractAsc();
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    // 输入一块 PCM(交错 s16),编码器内部会切成 1024 样本/帧
    void encode(const int16_t* pcm, int samplesPerCh, int64_t time100ns) {
        const DWORD bytes = samplesPerCh * m_ch * sizeof(int16_t);
        ComPtr<IMFMediaBuffer> buf;
        MFCreateMemoryBuffer(bytes, &buf);
        BYTE* p=nullptr; buf->Lock(&p,nullptr,nullptr);
        memcpy(p, pcm, bytes); buf->Unlock();
        buf->SetCurrentLength(bytes);

        ComPtr<IMFSample> s; MFCreateSample(&s);
        s->AddBuffer(buf.Get());
        s->SetSampleTime(time100ns);
        s->SetSampleDuration(10000000LL * samplesPerCh / m_rate);

        if (m_mft->ProcessInput(0, s.Get(), 0) == MF_E_NOTACCEPTING) { drain(); m_mft->ProcessInput(0, s.Get(), 0); }
        drain();
    }

    void shutdown(){ if(m_mft){ m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN,0); drain(); m_mft.Reset(); } }

private:
    void extractAsc() {
        ComPtr<IMFMediaType> t;
        if (FAILED(m_mft->GetOutputCurrentType(0, &t))) return;
        UINT32 sz=0;
        if (FAILED(t->GetBlobSize(MF_MT_USER_DATA, &sz)) || sz <= 12) return;
        std::vector<uint8_t> ud(sz);
        t->GetBlob(MF_MT_USER_DATA, ud.data(), sz, nullptr);
        Bytes asc(ud.begin()+12, ud.end());   // 跳过 12 字节 HEAACWAVEINFO 头
        if (onSeqHeader) onSeqHeader(asc);
    }

    void drain() {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO info{}; m_mft->GetOutputStreamInfo(0,&info);
            MFT_OUTPUT_DATA_BUFFER out{};
            ComPtr<IMFSample> os; ComPtr<IMFMediaBuffer> ob;
            MFCreateSample(&os);
            MFCreateMemoryBuffer(info.cbSize?info.cbSize:4096,&ob);
            os->AddBuffer(ob.Get()); out.pSample=os.Get();
            DWORD st=0;
            HRESULT hr=m_mft->ProcessOutput(0,1,&out,&st);
            if (hr==MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (FAILED(hr)) break;
            ComPtr<IMFMediaBuffer> b; os->ConvertToContiguousBuffer(&b);
            BYTE* p=nullptr; DWORD len=0; b->Lock(&p,nullptr,&len);
            int64_t t=0; os->GetSampleTime(&t);
            if (onEncoded) onEncoded(Bytes(p,p+len), (uint32_t)(t/10000));
            b->Unlock();
            if (out.pEvents) out.pEvents->Release();
        }
    }

    ComPtr<IMFTransform> m_mft;
    int m_rate=48000, m_ch=2;
};

#endif
```

---

## 7. RtmpPusher.h —— librtmp 推流(单线程)

RTMP 连接非线程安全,这里用**单独发送线程 + 互斥队列**。各编码器把 FLV tag 投进队列,发送线程串行 `RTMP_Write`。

```cpp
#ifndef RTMPPUSHER_H
#define RTMPPUSHER_H

#include <librtmp/rtmp.h>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>

class RtmpPusher {
public:
    using Bytes = std::vector<uint8_t>;

    bool start(const std::string& url) {
        m_rtmp = RTMP_Alloc();
        RTMP_Init(m_rtmp);
        if (!RTMP_SetupURL(m_rtmp, const_cast<char*>(url.c_str()))) return false;
        RTMP_EnableWrite(m_rtmp);                       // 推流(发布)模式
        if (!RTMP_Connect(m_rtmp, nullptr)) return false;
        if (!RTMP_ConnectStream(m_rtmp, 0)) return false;
        m_run = true;
        m_thread = std::thread([this]{ loop(); });
        return true;
    }

    // 线程安全:投递一个完整 FLV tag
    void push(Bytes tag) {
        { std::lock_guard<std::mutex> lk(m_mtx); m_q.push_back(std::move(tag)); }
        m_cv.notify_one();
    }

    void stop() {
        m_run = false; m_cv.notify_one();
        if (m_thread.joinable()) m_thread.join();
        if (m_rtmp) { RTMP_Close(m_rtmp); RTMP_Free(m_rtmp); m_rtmp=nullptr; }
    }
    ~RtmpPusher(){ stop(); }

private:
    void loop() {
        while (m_run) {
            Bytes tag;
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cv.wait(lk, [this]{ return !m_q.empty() || !m_run; });
                if (!m_run && m_q.empty()) break;
                tag = std::move(m_q.front()); m_q.pop_front();
            }
            if (!RTMP_IsConnected(m_rtmp)) break;
            RTMP_Write(m_rtmp, reinterpret_cast<const char*>(tag.data()), (int)tag.size());
        }
    }

    RTMP* m_rtmp=nullptr;
    std::deque<Bytes> m_q;
    std::mutex m_mtx; std::condition_variable m_cv;
    std::thread m_thread; std::atomic<bool> m_run{false};
};

#endif
```

---

## 8. SelfEncodeStreamer.h —— 总装(替代 FFmpegStreamer)

对外接口刻意和你现在的 `FFmpegStreamer` 类似:`startPush / writeFrame / stopPush`。
GlCompositor 那边几乎不用改调用方式。

```cpp
#ifndef SELFENCODESTREAMER_H
#define SELFENCODESTREAMER_H

#include <QObject>
#include <QImage>
#include <QSize>
#include <QDebug>
#include <atomic>
#include "StreamClock.h"
#include "FlvMuxer.h"
#include "MfH264Encoder.h"
#include "MfAacEncoder.h"
#include "WasapiLoopback.h"
#include "RtmpPusher.h"

class SelfEncodeStreamer : public QObject {
    Q_OBJECT
public:
    explicit SelfEncodeStreamer(QObject* parent=nullptr) : QObject(parent) {}
    ~SelfEncodeStreamer(){ stopPush(); }

    // captureSystemAudio=true 抓系统声音(WASAPI loopback)
    void startPush(const QSize& size, int fps=25, int videoBitrate=1500000,
                   bool captureSystemAudio=true) {
        if (m_running) return;
        m_size = size; m_fps = fps;
        m_clock.start();

        const QString url = "rtmp://8.152.169.7:1935/live/livestream";
        if (!m_rtmp.start(url.toStdString())) { qWarning() << "RTMP 连接失败"; return; }

        // —— 视频编码器 ——
        m_venc.onSeqHeader = [this](const auto& sps, const auto& pps){
            m_rtmp.push(m_flv.videoSeqHeader(sps, pps, 0));
        };
        m_venc.onEncoded = [this](const auto& avcc, bool key, uint32_t ts){
            m_rtmp.push(m_flv.videoNalu(avcc, key, ts));
        };
        if (!m_venc.init(size.width(), size.height(), fps, videoBitrate)) {
            qWarning() << "H264 编码器初始化失败"; return;
        }

        // —— 音频编码器 + 采集 ——
        if (captureSystemAudio) {
            m_aenc.onSeqHeader = [this](const auto& asc){
                m_rtmp.push(m_flv.audioSeqHeader(asc, 0));
            };
            m_aenc.onEncoded = [this](const auto& aac, uint32_t ts){
                m_rtmp.push(m_flv.audioFrame(aac, ts));
            };
            if (m_aenc.init(WasapiLoopback::kRate, WasapiLoopback::kCh, 128000)) {
                m_wasapi.onPcm = [this](const int16_t* pcm, int n, uint32_t /*ts*/){
                    m_aenc.encode(pcm, n, m_clock.now100ns());
                };
                m_wasapi.start();
            } else {
                qWarning() << "AAC 编码器初始化失败,改为纯视频推流";
            }
        }

        m_running = true;
        qDebug() << "🚀 [SelfEncode] 推流已启动" << size << fps << "fps,音频"
                 << (captureSystemAudio ? "系统声音" : "无");
    }

    // 写入一帧 BGRA(QImage::Format_ARGB32 小端即 BGRA)
    bool writeFrame(const QImage& src) {
        if (!m_running || src.size() != m_size) return false;
        QImage img = (src.format()==QImage::Format_ARGB32)
                       ? src : src.convertToFormat(QImage::Format_ARGB32);
        m_venc.encodeBGRA(img.constBits(), img.bytesPerLine(),
                          m_clock.now100ns(), m_clock.nowMs());
        return true;
    }

    void stopPush() {
        if (!m_running) return;
        m_running = false;
        m_wasapi.stop();
        m_aenc.shutdown();
        m_venc.shutdown();
        m_rtmp.stop();
        qDebug() << "⏹️ [SelfEncode] 推流已断开";
    }
    bool isPushing() const { return m_running; }

private:
    QSize m_size; int m_fps=25;
    std::atomic<bool> m_running{false};
    StreamClock    m_clock;
    FlvMuxer       m_flv;
    MfH264Encoder  m_venc;
    MfAacEncoder   m_aenc;
    WasapiLoopback m_wasapi;
    RtmpPusher     m_rtmp;
};

#endif
```

### 与 GlCompositor 对接

和现在用 `FFmpegStreamer` 完全一样,只是换个类型:

```cpp
// 原来:
// FFmpegStreamer* m_streamer;
// m_streamer->startPush(size, 25, outSize, audioDevice);
// m_streamer->writeFrame(composedImage);

// 改为:
SelfEncodeStreamer* m_streamer = new SelfEncodeStreamer(this);
m_streamer->startPush(composedImage.size(), 25, 1500000, /*captureSystemAudio=*/true);
m_streamer->writeFrame(composedImage);   // 在渲染线程或写帧线程调用
m_streamer->stopPush();
```

> `writeFrame` 内部是同步编码,1080p 软编可能耗时较多,**建议放到独立线程**(参考你已有的「单独渲染线程」),不要堵 GUI。

---

## 9. 进阶:换 NVENC / 干掉 CPU 回读

当前实现已经消除了「管道 + 子进程 + 音频争抢」,但 `bgraToNV12` + 软编仍占 CPU。要更极致:

1. **GPU 直接产 NV12**:在 `GlCompositor` 合成 shader 里输出 Y、UV 两张纹理,`glReadPixels` 直接拿 NV12,省掉 CPU 颜色转换。
2. **NVENC**:把 `MfH264Encoder` 换成 NVIDIA Video Codec SDK 的 `nvEncodeAPI`。
   - 输入:`NV_ENC_BUFFER_FORMAT_NV12`,或用 `cuGraphicsGLRegisterImage` 把 GL 纹理映射进 CUDA,**全程零回读**。
   - 输出:`NV_ENC_OUTPUT_PTR` 里是 Annex-B/AVCC,SPS/PPS 用 `nvEncGetSequenceParams` 取。
   - **FlvMuxer / RtmpPusher / 音频链路完全不动**,只换视频编码器实现,`onSeqHeader / onEncoded` 两个回调照旧。
3. **MF 硬件异步编码器**:用 `MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE)` 枚举硬件 MFT,绑定 `IMFDXGIDeviceManager`,输入 D3D11 纹理。注意硬件 MFT 是**异步**的,要走 `IMFAsyncCallback` / `METransformNeedInput / METransformHaveOutput` 事件模型(与本文同步循环不同)。

---

## 10. 关键坑速查

| 现象 | 原因 / 解决 |
|---|---|
| 拉流端黑屏 | 没先发 **AVC sequence header**(SPS/PPS)。确保 `onSeqHeader` 在第一帧前触发并发出。 |
| 拉流端无声 | 没发 **AAC sequence header**(AudioSpecificConfig),或 ASC 取错偏移(必须跳过 12 字节)。 |
| 花屏 / 解不出 | NALU 用了 Annex-B 起始码而非 **AVCC 长度前缀**;或帧体里混进了 SPS/PPS。本文已在 `emitFrame` 处理。 |
| 音画不同步、越拖越远 | 音频不是恒定速率。确认 `WasapiLoopback` 的**补静音**逻辑在跑(无声时也持续输出)。 |
| 时间戳错乱 | 音视频没用同一 `StreamClock`;或 ts 非单调递增。 |
| 关键帧太稀疏导致拉流卡 | 在 H264 输出类型设 `MF_MT_MPEG2_PROFILE` 后,通过 `CODECAPI_AVEncMPVGOPSize` 设 GOP=`fps*2`。 |
| 设备非 48kHz | `WasapiLoopback::appendConverted` 需接重采样器(MF Resampler / soxr)。 |
| GUI 卡顿 | `writeFrame` 同步软编耗时,放到独立线程。 |

---

## 11. 落地顺序建议

1. **先只跑视频**:`SelfEncodeStreamer` 里 `captureSystemAudio=false`,确认能拉到画面(`ffplay rtmp://...` 验证)。
2. **加音频**:打开 loopback,确认有声、且音画同步。
3. **优化**:GL 直接产 NV12 → 干掉 `bgraToNV12`;必要时上 NVENC。

每一步都能独立验证,不会一团乱。
```
