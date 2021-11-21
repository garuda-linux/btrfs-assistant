#ifndef BTRFSASSISTANT_H
#define BTRFSASSISTANT_H

#include <QDir>
#include <QFile>
#include <QMainWindow>
#include <QMap>
#include <QMessageBox>
#include <QProcess>
#include <QSet>
#include <QSignalMapper>
#include <QThread>
#include <QTime>
#include <QTranslator>
#include <QUuid>
#include <QXmlStreamReader>

QT_BEGIN_NAMESPACE
namespace Ui {
class BtrfsAssistant;
}
QT_END_NAMESPACE

struct Result {
    int exitCode;
    QString output;
};

struct Btrfs {
    QString mountPoint;
    long totalSize;
    long allocatedSize;
    long usedSize;
    long freeSize;
    long dataSize;
    long dataUsed;
    long metaSize;
    long metaUsed;
    long sysSize;
    long sysUsed;
    QMap<QString, QString> subVolumes;
};

struct SnapperSnapshots {
    int number;
    QString time;
    QString desc;
};

struct SnapperSubvolume {
    QString subvol;
    QString subvolid;
    QString time;
    QString desc;
    QString uuid;
};

class BtrfsAssistant : public QMainWindow {
    Q_OBJECT

  protected:
    QSet<QString> unitsEnabledSet;
    QHash<QString, QCheckBox *> configCheckBoxes;
    QMap<QString, Btrfs> fsMap;

    QSet<QCheckBox *> changedCheckBoxes;
    QMap<QString, QString> snapperConfigs;
    QMap<QString, QVector<SnapperSnapshots>> snapperSnapshots;
    QMap<QString, QVector<SnapperSubvolume>> snapperSubvolumes;
    bool hasSnapper = false;
    bool isSnapBoot = false;

    void refreshInterface();
    void displayError(QString errorText);
    void loadEnabledUnits();
    bool isInstalled(QString packageName);
    void setupConfigBoxes();
    void apply();
    Result runCmd(QString cmd, bool includeStderr, int timeout = 60);
    Result runCmd(QStringList cmdList, bool includeStderr, int timeout = 60);
    QStringList getBTRFSFilesystems();
    QString findMountpoint(QString uuid);
    void loadBTRFS();
    void populateBtrfsUi(QString uuid);
    QString toHumanReadable(double number);
    void populateSubvolList(QString uuid);
    void reloadSubvolList(QString uuid);
    void loadSnapper();
    void populateSnapperGrid();
    void populateSnapperConfigSettings();
    void restoreSnapshot(QString uuid, QString subvolume);
    bool isSnapper(QString subvolume);
    bool isTimeshift(QString subvolume);
    bool isMounted(QString uuid, QString subvolid);
    QString mountRoot(QString uuid);
    bool handleSnapshotBoot(bool checkOnly, bool restore);
    void enableRestoreMode(bool enable);
    void loadSnapperRestoreMode();
    SnapperSnapshots getSnapperMeta(QString filename);
    void snapperTimelineEnable(bool enable);

  public:
    explicit BtrfsAssistant(QWidget *parent = 0);
    ~BtrfsAssistant();

    QString getVersion(QString name);

    QString version;
    QString output;

    bool setup();

  private slots:
    void on_pushButton_balance_clicked();
    void on_pushButton_applybtrfs_clicked();
    void on_pushButton_load_clicked();
    void on_pushButton_loadsubvol_clicked();
    void on_pushButton_deletesubvol_clicked();
    void on_comboBox_btrfsdevice_activated();
    void on_checkBox_includesnapshots_clicked();
    void on_checkBox_show_subvolume_clicked(bool checked);
    void on_comboBox_snapper_configs_activated();
    void on_pushButton_snapper_create_clicked();
    void on_pushButton_snapper_delete_clicked();
    void on_comboBox_snapper_config_settings_activated();
    void on_pushButton_snapper_save_config_clicked();
    void on_pushButton_snapper_new_config_clicked();
    void on_pushButton_snapper_delete_config_clicked();
    void on_checkBox_snapper_advanced_clicked(bool checked);
    void on_pushButton_restore_snapshot_clicked();
    void on_checkBox_snapper_restore_clicked(bool checked);
    void on_checkBox_snapper_enabletimeline_clicked(bool checked);

  private:
    Ui::BtrfsAssistant *ui;
};
#endif // BTRFSASSISTANT_H
