/********************************************************************************
** Form generated from reading UI file 'mainwindow.ui'
**
** Created by: Qt User Interface Compiler version 6.11.1
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_MAINWINDOW_H
#define UI_MAINWINDOW_H

#include <QtCore/QVariant>
#include <QtMultimediaWidgets/QVideoWidget>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE

class Ui_MainWindow
{
public:
    QWidget *centralwidget;
    QVideoWidget *widget;
    QPushButton *captureBtn;
    QPushButton *pushBtn;
    QPushButton *switchBtn;
    QComboBox *cameraCombo;
    QMenuBar *menubar;
    QStatusBar *statusbar;

    void setupUi(QMainWindow *MainWindow)
    {
        if (MainWindow->objectName().isEmpty())
            MainWindow->setObjectName("MainWindow");
        MainWindow->resize(1950, 937);
        centralwidget = new QWidget(MainWindow);
        centralwidget->setObjectName("centralwidget");
        widget = new QVideoWidget(centralwidget);
        widget->setObjectName("widget");
        widget->setGeometry(QRect(200, 150, 1301, 581));
        captureBtn = new QPushButton(centralwidget);
        captureBtn->setObjectName("captureBtn");
        captureBtn->setGeometry(QRect(320, 790, 75, 23));
        pushBtn = new QPushButton(centralwidget);
        pushBtn->setObjectName("pushBtn");
        pushBtn->setGeometry(QRect(500, 790, 75, 23));
        switchBtn = new QPushButton(centralwidget);
        switchBtn->setObjectName("switchBtn");
        switchBtn->setGeometry(QRect(680, 790, 110, 23));
        cameraCombo = new QComboBox(centralwidget);
        cameraCombo->setObjectName("cameraCombo");
        cameraCombo->setGeometry(QRect(820, 790, 320, 23));
        cameraCombo->setCursor(QCursor(Qt::CursorShape::UpArrowCursor));
        MainWindow->setCentralWidget(centralwidget);
        menubar = new QMenuBar(MainWindow);
        menubar->setObjectName("menubar");
        menubar->setGeometry(QRect(0, 0, 1950, 21));
        MainWindow->setMenuBar(menubar);
        statusbar = new QStatusBar(MainWindow);
        statusbar->setObjectName("statusbar");
        MainWindow->setStatusBar(statusbar);

        retranslateUi(MainWindow);

        QMetaObject::connectSlotsByName(MainWindow);
    } // setupUi

    void retranslateUi(QMainWindow *MainWindow)
    {
        MainWindow->setWindowTitle(QCoreApplication::translate("MainWindow", "MainWindow", nullptr));
        captureBtn->setText(QCoreApplication::translate("MainWindow", "PushButton", nullptr));
        pushBtn->setText(QCoreApplication::translate("MainWindow", "PushButton", nullptr));
        switchBtn->setText(QCoreApplication::translate("MainWindow", "\345\210\207\346\215\242\345\210\260\346\221\204\345\203\217\345\244\264", nullptr));
    } // retranslateUi

};

namespace Ui {
    class MainWindow: public Ui_MainWindow {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_MAINWINDOW_H
