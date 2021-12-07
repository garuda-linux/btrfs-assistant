#include "btrfs-assistant.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDesktopWidget>
#include <QDebug>

// We have to manually parse argv into QStringList because when QCoreApplication initializes it's alr
QStringList parseArgs(int argc, char *argv[])
{
    QStringList list;
    const int ac = argc;
    char ** const av = argv;
    for (int a = 0; a < ac; ++a) {
        list << QString::fromLocal8Bit(av[a]);
    }
    return list;
}

int main(int argc, char *argv[]) {
    QCommandLineParser cmdline;
    QCommandLineOption xdg_desktop("xdg-desktop", "Set XDG_CURRENT_DESKTOP via params", "desktop");
    QCommandLineOption skip_snapshot_prompt("skip-snapshot-prompt", "Assume yes for the initial snapshot restore prompt");
    cmdline.addOption(xdg_desktop);
    cmdline.addOption(skip_snapshot_prompt);
    cmdline.process(parseArgs(argc, argv));
    if (cmdline.isSet(xdg_desktop))
        qputenv("XDG_CURRENT_DESKTOP", cmdline.value(xdg_desktop).toUtf8());

    QApplication a(argc, argv);
    QTranslator myappTranslator;
    myappTranslator.load("btrfsassistant_" + QLocale::system().name(), "/usr/share/btrfs-assistant/translations");
    a.installTranslator(&myappTranslator);

    BtrfsAssistant w;
    if (!w.setup(cmdline.isSet(skip_snapshot_prompt)))
        return 0;
    w.show();
    return a.exec();
}
