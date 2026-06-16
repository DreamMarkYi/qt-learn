#include <QApplication>
#include <QSurfaceFormat>
#include "Live2DWidget.h"

// 独立的 Live2D 模型查看器：用自定义 OpenGL 渲染器把模型静止显示出来，
// 不播动画、不接推流。模型路径写死为用户的 "我" 模型。
int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    // 请求 OpenGL 3.3 Core —— 与渲染器的 #version 330 core 着色器匹配。
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setVersion(3, 3);
    fmt.setSamples(4);                 // MSAA，边缘更顺滑
    QSurfaceFormat::setDefaultFormat(fmt);

    const QString dir  = QStringLiteral("D:/BaiduNetdiskDownload/My project (5)/Assets/海洋 模型");
    const QString file = QStringLiteral("海洋.model3.json");

    Live2DWidget w(dir, file);
    w.show();
    return app.exec();
}
