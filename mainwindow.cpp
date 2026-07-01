#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include <QCameraDevice>
#include <QPixmap>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    m_capturer = new DesktopCapturer(this);

    // 在 UI 原来那个 widget 的位置盖一个 QLabel 当预览画布
    m_previewLabel = new QLabel(ui->widget->parentWidget());
    m_previewLabel->setGeometry(ui->widget->geometry());
    m_previewLabel->setStyleSheet("background:#000;");
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->show();
    ui->widget->hide();   // 藏掉原来的 QVideoWidget（已不再使用）

    // 合成帧到达 → 缩放铺满 label 显示
    connect(m_capturer, &DesktopCapturer::previewFrame, this,
            [this](const QImage &frame) {
        m_previewLabel->setPixmap(
            QPixmap::fromImage(frame).scaled(m_previewLabel->size(),
                                             Qt::KeepAspectRatio,
                                             Qt::SmoothTransformation));
    });

    // 📋 把系统全部摄像头塞进下拉框（每项 userData 存设备 id）
    const auto cams = DesktopCapturer::availableCameras();
    if (cams.isEmpty()) {
        ui->cameraCombo->addItem("（未检测到摄像头）");
        ui->cameraCombo->setEnabled(false);
    } else {
        for (const QCameraDevice &c : cams)
            ui->cameraCombo->addItem(c.description(), c.id());
    }

    // 选中下拉框时切换摄像头设备
    connect(ui->cameraCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        const auto cams = DesktopCapturer::availableCameras();
        if (idx < 0 || idx >= cams.size()) return;
        m_capturer->setCameraDevice(cams.at(idx));
    });

    // 🎯 【开启/关闭 预览】
    connect(ui->captureBtn, &QPushButton::clicked, this, [this]() {
        static bool previewing = false;
        if (!previewing) {
            m_capturer->startAllPreviews();
            ui->captureBtn->setText("关闭预览");
        } else {
            m_capturer->stopAllPreviews();
            ui->captureBtn->setText("开启预览");
        }
        previewing = !previewing;
    });

    // 🎯 【开始/停止 推流】
    connect(ui->pushBtn, &QPushButton::clicked, this, [this]() {
        static bool pushing = false;
        if (!pushing) {
            m_capturer->startPush();
            ui->pushBtn->setText("停止推流");
        } else {
            m_capturer->stopPush();
            ui->pushBtn->setText("开始推流");
        }
        pushing = !pushing;
    });

    // 🎯 【画中画开关】切换推流时是否合成相机画面（实时生效）
    ui->switchBtn->setText(m_capturer->cameraOverlay() ? "推流含相机" : "仅推流桌面");
    connect(ui->switchBtn, &QPushButton::clicked, this, [this]() {
        bool on = !m_capturer->cameraOverlay();
        m_capturer->setCameraOverlay(on);
        ui->switchBtn->setText(on ? "推流含相机" : "仅推流桌面");
    });
}

MainWindow::~MainWindow()
{
    // ⚠️ 关闭顺序至关重要：必须先把后台管线（渲染线程）停掉，再删 UI。
    //
    // 原因：m_capturer 是 MainWindow 的子对象，按 Qt 规则它要等 ~MainWindow
    // 函数体执行完、进入 ~QObject 阶段才被销毁。若此处直接 `delete ui`，会先
    // 删掉 m_previewLabel，而渲染线程此刻仍在以 60fps 跑 renderTick →
    // emit frameReady → emit previewFrame，跨线程投递到 MainWindow 的 lambda
    // 里访问已成悬空指针的 m_previewLabel，导致关窗崩溃 / 卡死。
    //
    // 显式先 delete m_capturer：它的析构会 stopAllPreviews/stopPush，并对渲染
    // 线程 quit() + wait()，等线程彻底停下、不再有任何 previewFrame 回调之后，
    // 再删 UI 就安全了。delete 子对象会自动把它从父对象的 children 列表移除，
    // 不会被 ~QObject 二次删除。
    if (m_capturer) {
        delete m_capturer;
        m_capturer = nullptr;
    }
    delete ui;
}
