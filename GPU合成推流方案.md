# GPU 合成推流方案

在 GPU 上把"桌面 + 相机"两路画面合成为一张,再读回 CPU 喂给 ffmpeg 编码推流(或直接走硬件编码器,见末尾"全 GPU 路径")。

## 与 CPU 版的核心差别

| 步骤 | CPU 版(QPainter) | GPU 版(QRhi/OpenGL) |
|---|---|---|
| 取帧 | `QVideoFrame::toImage()` 强制回读到 CPU | 直接把 `QVideoFrame` 当作 GPU 纹理 |
| 合成 | `QPainter::drawImage` 在内存里画 | 着色器把两个纹理画到同一个 FBO |
| 输出 | 已是 `QImage`,直接写 ffmpeg | 从 FBO `readPixels` 回读到 CPU,再写 ffmpeg |
| 单帧瓶颈 | 每帧都 GPU→CPU 回读 + CPU 绘制 | 仅最后一步 GPU→CPU 回读一次 |

**关键洞察:** 只要最终还是用 ffmpeg(libx264)软编,**回读这步省不掉**。GPU 合成省的是"多次 toImage + CPU 绘制"的开销,不是回读本身。如果想连回读都省掉,就得上硬件编码器(末尾方案 B)。

## 选哪个 API

Qt 6 推荐用 **QRhi**(Qt 自己的图形抽象层,底层自动选 D3D11/Vulkan/Metal/OpenGL)。它和 `QVideoFrame` 的纹理接口集成最好,也是 Qt 官方多媒体后端用的同一套。

直接用 OpenGL 也行,但 `QVideoFrame` 在 Windows 上多半是 D3D11 纹理,要从 D3D11 转 OpenGL 得借 `WGL_NV_DX_interop`,反而麻烦。**用 QRhi 最顺**。

## 整体链路

```
QScreenCapture ─→ session ─→ screenSink.videoFrameChanged
                                     │
                                     ▼
                          QVideoFrame.toImage()? ❌
                          QVideoFrame::rhiTexture(QRhi*) ✅
                                     │
QCamera ───────→ session ─→ cameraSink.videoFrameChanged ──┐
                                     │                       │
                                     ▼                       ▼
                          ┌──────────────────────────────────────┐
                          │  QRhi 渲染管线                       │
                          │  - 创建 RT 纹理(1920x1080, RGBA8)    │
                          │  - draw 桌面纹理铺满                  │
                          │  - draw 相机纹理到右下角             │
                          └──────────────────────────────────────┘
                                     │
                                     ▼
                          QRhiReadbackResult(异步回读到 CPU)
                                     │ QImage
                          ┌──────────┴──────────┐
                          ▼                     ▼
                    QLabel 预览           FFmpegStreamer.writeFrame
```

## 关键代码骨架

### 1. 持有 QRhi 实例

```cpp
#include <rhi/qrhi.h>          // Qt 6.6+ 公开头
#include <QOffscreenSurface>

class GpuCompositor : public QObject {
    Q_OBJECT
public:
    explicit GpuCompositor(QObject *parent = nullptr) : QObject(parent) {
        QRhi::Implementation backend;
#ifdef Q_OS_WIN
        backend = QRhi::D3D11;     // Windows 首选
#else
        backend = QRhi::OpenGL;
#endif
        QRhiD3D11InitParams params;
        m_rhi.reset(QRhi::create(backend, &params));

        m_screenSink = new QVideoSink(this);
        m_cameraSink = new QVideoSink(this);

        connect(m_screenSink, &QVideoSink::videoFrameChanged,
                this, &GpuCompositor::onScreenFrame);
        connect(m_cameraSink, &QVideoSink::videoFrameChanged,
                this, [this](const QVideoFrame &f){ m_lastCamera = f; });

        buildPipeline();   // 创建 RT、shader、pipeline
    }

    QVideoSink* screenSink() const { return m_screenSink; }
    QVideoSink* cameraSink() const { return m_cameraSink; }

signals:
    void frameReady(const QImage &frame);

private:
    std::unique_ptr<QRhi> m_rhi;
    QVideoSink *m_screenSink, *m_cameraSink;
    QVideoFrame m_lastCamera;
    // ... pipeline、shader、target texture、readback 等成员
};
```

### 2. 建立渲染管线(简化伪代码)

```cpp
void GpuCompositor::buildPipeline() {
    // a) 离屏渲染目标:1920x1080 RGBA8 纹理 + 配套 RT
    m_rt = m_rhi->newTexture(QRhiTexture::RGBA8, QSize(1920, 1080),
                              1, QRhiTexture::RenderTarget|QRhiTexture::UsedAsTransferSource);
    m_rt->create();

    QRhiTextureRenderTargetDescription desc({m_rt});
    m_rtRT.reset(m_rhi->newTextureRenderTarget(desc));
    m_rpDesc.reset(m_rtRT->newCompatibleRenderPassDescriptor());
    m_rtRT->setRenderPassDescriptor(m_rpDesc.get());
    m_rtRT->create();

    // b) 顶点缓冲(全屏四边形)、采样器、shader resource bindings
    //    着色器:简单 textured quad,vert + frag
    //    片元着色器从 sampler2D 读颜色直接输出即可
    // c) 创建两个 pipeline:一个画桌面(全屏),一个画相机(右下角小矩形)
    //    或者只用一个 pipeline 改 viewport/uniform 画两次
    // 详细 QRhi 套路参考 Qt 官方 examples/rhi
}
```

### 3. 每帧合成 + 异步回读

```cpp
void GpuCompositor::onScreenFrame(const QVideoFrame &screenFrame) {
    // 1) 把两路 QVideoFrame 拿成 QRhiTexture
    QRhiTexture *texScreen = textureFromVideoFrame(screenFrame);
    QRhiTexture *texCamera = m_lastCamera.isValid()
                               ? textureFromVideoFrame(m_lastCamera)
                               : nullptr;

    QRhiCommandBuffer *cb = nullptr;
    m_rhi->beginOffscreenFrame(&cb);

    // 2) 渲染:先桌面铺满,再画相机到右下角
    cb->beginPass(m_rtRT.get(), Qt::black, {1.0f, 0});
    drawTexturedQuad(cb, texScreen, fullScreenQuad);
    if (texCamera && m_overlay) {
        // 右下角:x=W-w-20, y=H-h-20
        drawTexturedQuad(cb, texCamera, pipQuad);
    }
    cb->endPass();

    // 3) 异步回读到 CPU(关键:不阻塞 GPU)
    QRhiReadbackResult *rr = new QRhiReadbackResult;
    rr->completed = [this, rr]() {
        QImage img(reinterpret_cast<const uchar*>(rr->data.constData()),
                   rr->pixelSize.width(), rr->pixelSize.height(),
                   QImage::Format_RGBA8888);
        emit frameReady(img.copy());   // 复制一份脱离 rr 生命周期
        delete rr;
    };
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    u->readBackTexture({m_rt}, rr);
    cb->resourceUpdate(u);

    m_rhi->endOffscreenFrame();
}
```

### 4. `QVideoFrame` → `QRhiTexture` 的桥接

这是整个方案最棘手的一步。Qt 6 对此有公开 API,但比较新:

```cpp
// 6.6+ 提供 QVideoFrame::rhiTextures() / textureHandle(QRhi*) 之类接口
// 思路:
QRhiTexture* textureFromVideoFrame(const QVideoFrame &f) {
    // a) 如果 f.handleType() == QVideoFrame::RhiTextureHandle,
    //    直接拿到原生 GPU 句柄,用 QRhi::importTexture 包成 QRhiTexture
    // b) 否则(纯 CPU 帧,如某些相机),走 toImage() + 上传到 QRhiTexture
    //    虽然这条退化路径有 CPU→GPU 上传开销,但相机帧通常很小
}
```

**实测前要先打印 `f.handleType()` 看后端给的是什么**,Windows 桌面捕获多半是 D3D11 纹理,相机可能是 CPU 帧,两者要分别处理。

## 这个方案值不值得做

**适合上 GPU 合成的场景:**
- 输出分辨率 ≥ 1440p,或同时合成 ≥ 3 路视频源
- 需要复杂特效:动态模糊、色彩 LUT、转场动画、3D 透视
- CPU 已经吃满(比如同机还在跑游戏/编辑器)

**不值得上的场景:**
- 1080p 25fps 单画中画 —— CPU 版完全够用,QRhi 增加的复杂度不划算
- 团队不熟 QRhi/着色器 —— 调试 GPU 渲染问题门槛高得多

## 真想榨干 GPU:全 GPU 路径(方案 B)

把"读回 CPU 给 libx264"也省掉,改成 GPU 端硬件编码:

```
QRhi 合成 → 共享纹理 → ffmpeg 的 h264_nvenc/h264_qsv/h264_amf
```

实现路径有两种:
1. **改 ffmpeg 命令行**:`-hwaccel cuda -hwaccel_output_format cuda`,但输入要是它自己能读的源(回到 gdigrab+overlay_cuda),失去本地合成意义。
2. **直接调 NVENC SDK / Media Foundation**:跳过 ffmpeg 进程,在 C++ 里把 QRhi 的纹理直接喂给 NVENC,编码后通过 RTMP 库(如 librtmp)推出。**完全 GPU 流水线,延迟最低**,但工程量是上面所有方案的 3 倍以上。

## 建议路线

1. **先上 CPU 版**(已写在 [本地合成推流方案.md] 里)→ 验证整条链路通,看 CPU 占用是否能接受。
2. CPU 占用 < 30% 就停在 CPU 版,别折腾。
3. 占用真扛不住再上 QRhi 版,且优先考虑"只把合成搬 GPU、编码仍 libx264"的方案 A,工程量可控。
4. 方案 B(全 GPU + 硬件编码)只在做商业级低延迟直播时才值得投入。

## 一个现实数字

1920×1080 RGBA8 单帧 ≈ 8MB,25fps 回读 ≈ 200MB/s。这个量在 PCIe 3.0 ×16(理论 ~16GB/s 单向)上完全不构成瓶颈,GPU→CPU 回读真正的开销是同步等待,所以**回读必须用异步 `QRhiReadbackResult` 的 completed 回调**,不能 `m_rhi->finish()` 阻塞等。
