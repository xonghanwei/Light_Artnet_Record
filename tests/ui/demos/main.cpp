// main.cpp — ArtNet Recorder SCADA 功能测试面板入口
#include <QApplication>
#include "ArtNetRecorderMainWindow.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Light Artnet Record — SCADA 功能测试面板");
    QApplication::setApplicationVersion("1.0.0");

    ArtNetRecorderMainWindow w;
    w.show();

    return app.exec();
}
