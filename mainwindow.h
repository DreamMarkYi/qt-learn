#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include "DesktopCapturer.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    Ui::MainWindow *ui;

    // 主窗口只需抱紧这一个总控类的指针
    DesktopCapturer* m_capturer = nullptr;

    // 预览画布：把合成帧 setPixmap 到这个 label 上
    QLabel* m_previewLabel = nullptr;
};
#endif // MAINWINDOW_H
