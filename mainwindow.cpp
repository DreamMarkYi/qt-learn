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
    delete ui;
}
