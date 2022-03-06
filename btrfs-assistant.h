#ifndef BTRFSASSISTANT_H
#define BTRFSASSISTANT_H

#include <QDir>
#include <QFile>
#include <QMainWindow>
#include <QMap>
#include <QMessageBox>
#include <QProcess>
#include <QSet>
#include <QSettings>
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

    QStringList bmFreqValues = {"none", "daily", "weekly", "monthly"};

    QSet<QCheckBox *> changedCheckBoxes;
    QMap<QString, QString> snapperConfigs;
    QMap<QString, QVector<SnapperSnapshots>> snapperSnapshots;
    QMap<QString, QVector<SnapperSubvolume>> snapperSubvolumes;
    bool hasSnapper = false;
    bool hasBtrfsmaintenance = false;
    bool isSnapBoot = false;
    QSettings *settings;
    QSettings *bmSettings;
    QString btrfsmaintenanceConfig;
    QSettings::Format bmFormat;

    void refreshInterface();
    void loadEnabledUnits();
    void setupConfigBoxes();
    void apply();
    void loadBTRFS();
    void populateBtrfsUi(const QString &uuid);
    void populateSubvolList(const QString &uuid);
    void reloadSubvolList(const QString &uuid);
    void loadSnapper();
    void populateSnapperGrid();
    void populateSnapperConfigSettings();
    void restoreSnapshot(const QString &uuid, QString subvolume);
    void switchToSnapperRestore();
    QMap<QString, QString> getSnapshotBoot();
    void enableRestoreMode(bool enable);
    void loadSnapperRestoreMode();
    void snapperTimelineEnable(bool enable);
    void populateBmTab();
    void updateServices(QList<QCheckBox *>);

  public:
    explicit BtrfsAssistant(QWidget *parent = 0);
    ~BtrfsAssistant();

    QString getVersion(QString name);

    QString version;
    QString output;

    bool setup(bool skipSnapshotPrompt, bool snapshotBoot);

  private slots:
    void on_checkBox_bmBalance_clicked(bool checked);
    void on_checkBox_bmDefrag_clicked(bool checked);
    void on_checkBox_bmScrub_clicked(bool checked);
    void on_checkBox_includesnapshots_clicked();
    void on_checkBox_snapper_enabletimeline_clicked(bool checked);
    void on_checkBox_snapper_restore_clicked(bool checked);
    void on_comboBox_btrfsdevice_activated(int);
    void on_comboBox_snapper_configs_activated(int);
    void on_comboBox_snapper_config_settings_activated(int);
    void on_pushButton_bmApply_clicked();
    void on_pushButton_deletesubvol_clicked();
    void on_pushButton_load_clicked();
    void on_pushButton_loadsubvol_clicked();
    void on_pushButton_restore_snapshot_clicked();
    void on_pushButton_snapper_create_clicked();
    void on_pushButton_snapper_delete_clicked();
    void on_pushButton_snapper_delete_config_clicked();
    void on_pushButton_snapper_new_config_clicked();
    void on_pushButton_snapper_save_config_clicked();
    void on_pushButton_SnapperUnitsApply_clicked();

  private:
    Ui::BtrfsAssistant *ui;
};
#endif // BTRFSASSISTANT_H
