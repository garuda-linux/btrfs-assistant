#include "btrfs-assistant.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDesktopWidget>

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);
    QTranslator myappTranslator;
    myappTranslator.load("btrfsassistant_" + QLocale::system().name(), "/usr/share/btrfs-assistant/translations");
    a.installTranslator(&myappTranslator);

    QCommandLineParser cmdline;
    QCommandLineOption xdg_desktop("xdg-desktop", "Set XDG_CURRENT_DESKTOP via params");
    cmdline.addOption(xdg_desktop);
    cmdline.process(a);
    if (cmdline.isSet(xdg_desktop))
        qputenv("XDG_CURRENT_DESKTOP", cmdline.value(xdg_desktop).toUtf8());

    BtrfsAssistant w;
    if (!w.setup())
        return 0;
    w.show();
    return a.exec();
}
