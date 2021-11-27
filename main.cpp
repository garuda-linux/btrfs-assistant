#include "btrfs-assistant.h"

#include <QApplication>

#include <QDesktopWidget>
int main(int argc, char *argv[]) {
    QApplication a(argc, argv);
    QTranslator myappTranslator;
    myappTranslator.load("btrfsassistant_" + QLocale::system().name(), "/usr/share/btrfs-assistant/translations");
    a.installTranslator(&myappTranslator);
    BtrfsAssistant w;
    if (!w.setup())
        return 0;
    w.show();
    return a.exec();
}
