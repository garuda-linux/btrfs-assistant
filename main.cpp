#include "btrfs-assistant.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDesktopWidget>
#include <QDebug>

// We have to manually parse argv into QStringList because by the time QCoreApplication initializes it's already too late because Qt already picked the theme
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
    QCommandLineOption xdgDesktop("xdg-desktop", "Set XDG_CURRENT_DESKTOP via params", "desktop");
    QCommandLineOption skipSnapshotPrompt("skip-snapshot-prompt", "Assume yes for the initial snapshot restore prompt");
    QCommandLineOption snapBootAutostart("snap-boot-autostart");
    cmdline.addOption(xdgDesktop);
    cmdline.addOption(skipSnapshotPrompt);
    cmdline.addOption(snapBootAutostart);
    cmdline.process(parseArgs(argc, argv));
    if (cmdline.isSet(xdgDesktop))
        qputenv("XDG_CURRENT_DESKTOP", cmdline.value(xdgDesktop).toUtf8());

    QApplication a(argc, argv);
    QTranslator myappTranslator;
    myappTranslator.load("btrfsassistant_" + QLocale::system().name(), "/usr/share/btrfs-assistant/translations");
    a.installTranslator(&myappTranslator);

    BtrfsAssistant w;
    if (!w.setup(cmdline.isSet(skipSnapshotPrompt), cmdline.isSet(snapBootAutostart)))
        return 0;
    w.show();
    return a.exec();
}
