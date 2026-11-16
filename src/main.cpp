#include <QApplication>
#include <QIcon>

#include "log.h"
#include "mainwindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("djlivestreamer");
    QApplication::setApplicationName("djlivestreamer");
    QApplication::setApplicationDisplayName("DJ Live Streamer");
    QApplication::setDesktopFileName("djlivestreamer");
    QApplication::setWindowIcon(QIcon::fromTheme("media-record"));
    logInit();
    MainWindow w;
    w.show();
    return app.exec();
}
