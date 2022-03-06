#include "btrfs-assistant.h"
#include "config.h"
#include "ui_btrfs-assistant.h"
#include <QDebug>

/*
 *
 * static free utility functions
 *
 */

// A simple wrapper to QMessageBox for creating consistent error messages
static void displayError(const QString &errorText) { QMessageBox::critical(0, "Error", errorText); }

// returns true if the end users confirms that they want to restore the snapshot that they are currnetly booted into
static bool askSnapshotBoot(const QString &subvol) {
    return QMessageBox::question(0, QObject::tr("Snapshot boot detected"),
                                 QObject::tr("You are currently booted into snapshot ") + subvol + "\n\n" +
                                     QObject::tr("Would you like to restore it?")) == QMessageBox::Yes;
}

// Util function for getting bash command output and error code
static const Result runCmd(const QString &cmd, bool includeStderr, int timeout = 60) {
    QProcess proc;

    if (includeStderr)
        proc.setProcessChannelMode(QProcess::MergedChannels);

    proc.start("/bin/bash", QStringList() << "-c" << cmd);

    proc.waitForFinished(1000 * 60);
    return {proc.exitCode(), proc.readAllStandardOutput().trimmed()};
}

// An overloaded version that takes a list so multiple commands can be executed at once
static const Result runCmd(const QStringList &cmdList, bool includeStderr, int timeout = 60) {
    QString fullCommand;
    for (const QString &command : qAsConst(cmdList)) {
        if (fullCommand == "")
            fullCommand = command;
        else
            fullCommand += "; " + command;
    }

    // Run the composite command as a single command
    return runCmd(fullCommand, includeStderr, timeout);
}

// Returns a list of btrfs filesystems
static const QStringList getBTRFSFilesystems() {
    return runCmd("btrfs filesystem show -m | grep uuid | awk -F':' '{gsub(/ /,\"\");print $3}'", false).output.split('\n');
}

// Returns one of the mountpoints for a given UUID
static const QString findMountpoint(const QString &uuid) {
    return runCmd("findmnt --real -rno target,uuid | grep " + uuid + " | head -n 1 | awk '{print $1}'", false).output;
}

// Finds the direct children of a given subvolid
static const QStringList findBtrfsChildren(const QString &subvolid, const QString &uuid) {
    QString output = runCmd("sudo btrfs subvolume list / | awk '{print $7, $9}' | grep " + subvolid, false).output;
    if (output.isEmpty())
        return QStringList();

    QStringList subvols;
    const QStringList outputList = output.split('\n');
    for (const QString &subvolEntry : outputList) {
        if (subvolEntry.isEmpty())
            continue;

        if (subvolEntry.split(' ').at(0).trimmed() == subvolid)
            subvols.append(subvolEntry.split(' ').at(1).trimmed());
    }

    return subvols;
}

// Returns name of the subvol mounted at /. If no subvol is found, returns a default constructed string
static const QString findRootSubvol() {
    const QString output = runCmd("LANG=C findmnt -no uuid,options /", false).output;
    if (output.isEmpty())
        return QString();

    const QString uuid = output.split(' ').at(0).trimmed();
    const QString options = output.right(output.length() - uuid.length()).trimmed();
    if (options.isEmpty() || uuid.isEmpty())
        return QString();

    QString subvol;
    const QStringList optionsList = options.split(',');
    for (const QString &option : optionsList) {
        if (option.startsWith("subvol="))
            subvol = option.split("subvol=").at(1);
    }

    // Make sure subvolume doesn't have a leading slash
    if (subvol.startsWith("/"))
        subvol = subvol.right(subvol.length() - 1);

    // At this point subvol will either contain nothing or the name of the subvol
    return subvol;
}

// Converts a double to a human readable string for displaying data storage amounts
static const QString toHumanReadable(double number) {
    int i = 0;
    const QVector<QString> units = {"B", "kiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB"};
    while (number > 1024) {
        number /= 1024;
        i++;
    }
    return QString::number(number) + " " + units[i];
}

// Returns the list of subvolume mountpoints
static const QStringList gatherBtrfsMountpoints() {
    QStringList mountpoints;

    const QStringList output = runCmd("findmnt --real -lno fstype,target", false).output.trimmed().split('\n');
    for (const QString &line : output) {
        if (line.startsWith("btrfs")) {
            QString mountpoint = line.simplified().split(' ').at(1).trimmed();
            QStringList crap = line.split(' ');
            qDebug() << crap;
            if (!mountpoint.isEmpty()) {
                mountpoints.append(mountpoint);
            }
        }
    }

    mountpoints.sort();

    return mountpoints;
}

// Finds the mountpoint of a given btrfs volume.  If it isn't mounted, it will first mount it.
// returns the mountpoint or a default constructed string if it fails
static const QString mountRoot(const QString &uuid) {
    // Check to see if it is already mounted
    QStringList findmntOutput = runCmd("findmnt -nO subvolid=5 -o uuid,target | head -n 1", false).output.split('\n');
    QString mountpoint;
    for (const QString &line : qAsConst(findmntOutput)) {
        if (line.split(' ').at(0).trimmed() == uuid)
            mountpoint = line.split(' ').at(1).trimmed();
    }

    // If it isn't mounted we need to mount it
    if (mountpoint.isEmpty()) {
        // Format a temp mountpoint using a GUID
        mountpoint = QDir::cleanPath(QDir::tempPath() + QDir::separator() + QUuid::createUuid().toString());

        // Create the mountpoint and mount the volume if successful
        QDir tempMount;
        if (tempMount.mkpath(mountpoint))
            runCmd("mount -t btrfs -o subvolid=5 UUID=" + uuid + " " + mountpoint, false);
        else
            return QString();
    }

    return mountpoint;
}

// Returns true if a given subvolume is a timeshift snapshot
static bool isTimeshift(const QString &subvolume) { return subvolume.contains("timeshift-btrfs"); }

// Returns true if a given subvolume is a snapper snapshot
static bool isSnapper(const QString &subvolume) { return subvolume.contains(".snapshots") && !subvolume.endsWith(".snapshots"); }

// Returns true if a given btrfs subvolume is mounted
static bool isMounted(const QString &uuid, const QString &subvolid) {
    return uuid == runCmd("findmnt -nO subvolid=" + subvolid.trimmed() + " -o uuid | head -n 1", false).output.trimmed();
}

// Renames a btrfs subvolume from source to target.  Both should be absolute paths
static bool renameSubvolume(const QString &source, const QString &target) {
    QDir dir;
    return dir.rename(source, target);
}

// Read a snapper snapshot meta file and return the data
static SnapperSnapshots getSnapperMeta(const QString &filename) {
    SnapperSnapshots snap;
    snap.number = 0;
    QFile metaFile(filename);
    if (!metaFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return snap;

    while (!metaFile.atEnd()) {
        QString line = metaFile.readLine();
        if (line.trimmed().startsWith("<num>"))
            snap.number = line.trimmed().split("<num>").at(1).split("</num>").at(0).trimmed().toInt();
        else if (line.trimmed().startsWith("<date>"))
            snap.time = line.trimmed().split("<date>").at(1).split("</date>").at(0).trimmed();
        else if (line.trimmed().startsWith("<description>"))
            snap.desc = line.trimmed().split("<description>").at(1).split("</description>").at(0).trimmed();
    }

    return snap;
}

// Selects all rows in @p listWidget that match an item in @p items
static void setListWidgetSelections(const QStringList &items, QListWidget *listWidget) {
    QAbstractItemModel *model = listWidget->model();
    QItemSelectionModel *selectModel = listWidget->selectionModel();
    for (int i = 0; i < model->rowCount(); i++) {
        QModelIndex index = model->index(i, 0);
        if (items.contains(model->data(index).toString())) {
            selectModel->select(index, QItemSelectionModel::Select);
        }
    }
}

/*
 *
 * QSettings read/write functions
 *
 */

// Called by QSetting to read the contents of the Btrfs maintenance config file.
// Populates @p with all the keys and values from the file.  Also populates map
// with a key named "raw" that contains the original contents
bool readBmFile(QIODevice &device, QSettings::SettingsMap &map) {
    QStringList rawList;
    while (!device.atEnd()) {
        QString line = device.readLine();
        rawList.append(line);
        if (!line.trimmed().isEmpty() && !line.trimmed().startsWith("#")) {
            const QStringList lineList = line.simplified().trimmed().split("=");
            map.insert(lineList.at(0).trimmed(), lineList.at(1).trimmed().remove("\""));
        }
    }

    map.insert("raw", rawList);

    return true;
}

// Called by QSetting to write the contents of the Btrfs maintenance config file.
// Reads the contents of the "raw" key in map and writes a new file based on it
// and the other key, value pairs in @p map.
bool writeBmFile(QIODevice &device, const QSettings::SettingsMap &map) {
    QByteArray data;

    if (!map.contains("raw")) {
        return false;
    }

    const QStringList rawList = map.value("raw").toStringList();
    for (const QString &line : rawList) {
        if (line.trimmed().startsWith("#")) {
            data += line.toUtf8();
        } else {
            const QString key = line.simplified().split("=").at(0).trimmed();
            if (map.contains(key)) {
                data += key.toUtf8() + "=\"" + map.value(key).toString().toUtf8() + "\"\n";
            }
        }
    }

    device.write(data);

    return true;
}

/*
 *
 * BtrfsAssistant functions
 *
 */

BtrfsAssistant::BtrfsAssistant(QWidget *parent) : QMainWindow(parent), ui(new Ui::BtrfsAssistant) {
    ui->setupUi(this);

    this->setWindowTitle(tr("BTRFS Assistant"));
}

BtrfsAssistant::~BtrfsAssistant() { delete ui; }

// setup various items first time program runs
bool BtrfsAssistant::setup(bool skipSnapshotPrompt, bool snapshotBoot) {
    settings = new QSettings("/etc/btrfs-assistant.conf", QSettings::NativeFormat);

    bool restoreSnapshotSelected = false;

    // We should ask if we should restore before we ask for root permissions for usability reasons
    auto sbResult = getSnapshotBoot();
    if (isSnapBoot && !skipSnapshotPrompt && sbResult.contains("uuid") && sbResult.contains("subvol")) {
        restoreSnapshotSelected = askSnapshotBoot(sbResult.value("subvol"));
        if (!restoreSnapshotSelected && snapshotBoot)
            return false;
    }

    // If the application wasn't luanched with root access, relaunch it
    if (runCmd("id -u", false).output != "0") {
        auto args = QCoreApplication::arguments();
        QString cmd = "pkexec btrfs-assistant";
        cmd += " --xdg-desktop \"" + qEnvironmentVariable("XDG_CURRENT_DESKTOP", "") + "\"";
        if (isSnapBoot)
            cmd += " --skip-snapshot-prompt";

        for (const QString &arg : args)
            cmd += " " + arg;
        cmd += "; true";
        execlp("sh", "sh", "-c", cmd.toUtf8().constData(), NULL);
        QApplication::exit(1);
        return false;
    }

    // Save the state of snapper and btrfsmaintenance being installed since we have to check them so often
    QString snapperPath = settings->value("snapper", "/usr/bin/snapper").toString();
    hasSnapper = QFile::exists(snapperPath);

    btrfsmaintenanceConfig = settings->value("btrfsmaintenance", "/etc/default/btrfsmaintenance").toString();
    hasBtrfsmaintenance = QFile::exists(btrfsmaintenanceConfig);

    // If snapper isn't installed, hide the snapper-related elements of the UI
    if (!hasSnapper) {
        ui->tabWidget->setTabVisible(ui->tabWidget->indexOf(ui->tab_snapper_general), false);
        ui->tabWidget->setTabVisible(ui->tabWidget->indexOf(ui->tab_snapper_settings), false);
    } else {
        ui->groupBox_snapper_config_edit->hide();
    }

    // Populate the UI
    refreshInterface();
    loadBTRFS();
    loadSnapper();
    if (snapperConfigs.contains("root"))
        ui->comboBox_snapper_configs->setCurrentText("root");
    populateSnapperGrid();
    populateSnapperConfigSettings();
    ui->pushButton_restore_snapshot->setEnabled(false);

    if (hasBtrfsmaintenance) {
        bmFormat = QSettings::registerFormat("btrfsmaintenance", readBmFile, writeBmFile);
        bmSettings = new QSettings(btrfsmaintenanceConfig, bmFormat);
        populateBmTab();
    } else {
        // Hide the btrfs maintenance tab
        ui->tabWidget->setTabVisible(ui->tabWidget->indexOf(ui->tab_btrfsmaintenance), false);
    }

    if (isSnapBoot)
        switchToSnapperRestore();
    if ((restoreSnapshotSelected || skipSnapshotPrompt) && sbResult.contains("uuid") && sbResult.contains("subvol")) {
        restoreSnapshot(sbResult.value("uuid"), sbResult.value("subvol"));
    }

    return true;
}

// Populates servicesEnabledSet with a list of enabled services
void BtrfsAssistant::loadEnabledUnits() {
    this->unitsEnabledSet.clear();

    QString bashOutput = runCmd("systemctl list-unit-files --state=enabled -q --no-pager | awk '{print $1}'", false).output;
    QStringList serviceList = bashOutput.split('\n');
    this->unitsEnabledSet = QSet<QString>(serviceList.begin(), serviceList.end());

    return;
}

// Updates the checkboxes and comboboxes with values from the system
void BtrfsAssistant::refreshInterface() {
    loadEnabledUnits();

    // Loop through the checkboxes
    const QList<QCheckBox *> checkboxes =
        ui->scrollArea_bm->findChildren<QCheckBox *>() + ui->groupBox_snapperUnits->findChildren<QCheckBox *>();
    for (QCheckBox *checkbox : checkboxes) {
        if (checkbox->property("actionType") == "service") {
            checkbox->setChecked(this->unitsEnabledSet.contains(checkbox->property("actionData").toString()));
        }
    }
}

// Populates the btrfs fsMap with statistics from the btrfs filesystems
void BtrfsAssistant::loadBTRFS() {
    fsMap.clear();
    ui->comboBox_btrfsdevice->clear();

    QStringList uuidList = getBTRFSFilesystems();

    for (const QString &uuid : qAsConst(uuidList)) {
        QString mountpoint = findMountpoint(uuid);
        if (!mountpoint.isEmpty()) {
            Btrfs btrfs = {};
            btrfs.mountPoint = mountpoint;
            QStringList usageLines = runCmd("LANG=C ; btrfs fi usage -b " + mountpoint, false).output.split('\n');
            for (const QString &line : qAsConst(usageLines)) {
                QString type = line.split(':').at(0).trimmed();
                if (type == "Device size") {
                    btrfs.totalSize = line.split(':').at(1).trimmed().toLong();
                } else if (type == "Device allocated") {
                    btrfs.allocatedSize = line.split(':').at(1).trimmed().toLong();
                } else if (type == "Used") {
                    btrfs.usedSize = line.split(':').at(1).trimmed().toLong();
                } else if (type == "Free (estimated)") {
                    btrfs.freeSize = line.split(':').at(1).split(QRegExp("\\s+"), Qt::SkipEmptyParts).at(0).trimmed().toLong();
                } else if (type.startsWith("Data,")) {
                    btrfs.dataSize = line.split(':').at(2).split(',').at(0).trimmed().toLong();
                    btrfs.dataUsed = line.split(':').at(3).split(' ').at(0).trimmed().toLong();
                } else if (type.startsWith("Metadata,")) {
                    btrfs.metaSize = line.split(':').at(2).split(',').at(0).trimmed().toLong();
                    btrfs.metaUsed = line.split(':').at(3).split(' ').at(0).trimmed().toLong();
                } else if (type.startsWith("System,")) {
                    btrfs.sysSize = line.split(':').at(2).split(',').at(0).trimmed().toLong();
                    btrfs.sysUsed = line.split(':').at(3).split(' ').at(0).trimmed().toLong();
                }
            }
            fsMap[uuid] = btrfs;
            ui->comboBox_btrfsdevice->addItem(uuid);
        }
    }

    populateBtrfsUi(ui->comboBox_btrfsdevice->currentText());
    reloadSubvolList(ui->comboBox_btrfsdevice->currentText());
}

// Populates the UI for the BTRFS tab
void BtrfsAssistant::populateBtrfsUi(const QString &uuid) {
    // For the tools section
    int dataPercent = ((double)fsMap[uuid].dataUsed / fsMap[uuid].dataSize) * 100;
    ui->progressBar_btrfsdata->setValue(dataPercent);
    ui->progressBar_btrfsmeta->setValue(((double)fsMap[uuid].metaUsed / fsMap[uuid].metaSize) * 100);
    ui->progressBar_btrfssys->setValue(((double)fsMap[uuid].sysUsed / fsMap[uuid].sysSize) * 100);

    // The information section
    ui->label_btrfsallocated->setText(toHumanReadable(fsMap[uuid].allocatedSize));
    ui->label_btrfsused->setText(toHumanReadable(fsMap[uuid].usedSize));
    ui->label_btrfssize->setText(toHumanReadable(fsMap[uuid].totalSize));
    ui->label_btrfsfree->setText(toHumanReadable(fsMap[uuid].freeSize));
    float freePercent = (double)fsMap[uuid].allocatedSize / fsMap[uuid].totalSize;
    if (freePercent < 0.70) {
        ui->label_btrfsmessage->setText(tr("You have lots of free space, did you overbuy?"));
    } else if (freePercent > 0.95) {
        ui->label_btrfsmessage->setText(tr("Situation critical!  Time to delete some data or buy more disk"));
    } else {
        ui->label_btrfsmessage->setText(tr("Your disk space is well utilized"));
    }
}

void BtrfsAssistant::on_pushButton_load_clicked() {
    loadBTRFS();

    ui->pushButton_load->clearFocus();
}

void BtrfsAssistant::on_pushButton_loadsubvol_clicked() {
    QString uuid = ui->comboBox_btrfsdevice->currentText();

    if (uuid.isEmpty()) {
        displayError(tr("No device selected") + "\n" + tr("Please Select a device first"));
        return;
    }

    reloadSubvolList(uuid);

    ui->pushButton_loadsubvol->clearFocus();
}

// Reloads the list of subvolumes on the BTRFS Details tab
void BtrfsAssistant::reloadSubvolList(const QString &uuid) {
    if (!fsMap.contains(uuid))
        return;

    fsMap[uuid].subVolumes.clear();

    QString mountpoint = findMountpoint(uuid);

    QStringList output = runCmd("btrfs subvolume list " + mountpoint, false).output.split('\n');
    QMap<QString, QString> subvols;
    for (const QString &line : qAsConst(output)) {
        if (!line.isEmpty())
            subvols[line.split(' ').at(1)] = line.split(' ').at(8);
    }

    fsMap[uuid].subVolumes = subvols;

    populateSubvolList(uuid);
}

// Populate the btrfsmaintenance tab using the settings loaded from the config file
void BtrfsAssistant::populateBmTab() {
    ui->comboBox_bmBalanceFreq->clear();
    ui->comboBox_bmBalanceFreq->insertItems(0, bmFreqValues);
    ui->comboBox_bmBalanceFreq->setCurrentText(bmSettings->value("BTRFS_BALANCE_PERIOD").toString());
    ui->comboBox_bmScrubFreq->clear();
    ui->comboBox_bmScrubFreq->insertItems(0, bmFreqValues);
    ui->comboBox_bmScrubFreq->setCurrentText(bmSettings->value("BTRFS_SCRUB_PERIOD").toString());
    ui->comboBox_bmDefragFreq->clear();
    ui->comboBox_bmDefragFreq->insertItems(0, bmFreqValues);
    ui->comboBox_bmDefragFreq->setCurrentText(bmSettings->value("BTRFS_DEFRAG_PERIOD").toString());

    const QStringList mountpoints = gatherBtrfsMountpoints();
    ui->listWidget_bmBalance->clear();
    ui->listWidget_bmBalance->insertItems(0, mountpoints);
    QStringList balanceMounts = bmSettings->value("BTRFS_BALANCE_MOUNTPOINTS").toString().trimmed().split(":");
    if (balanceMounts.contains("auto")) {
        ui->checkBox_bmBalance->setChecked(true);
        ui->listWidget_bmBalance->setDisabled(true);
    } else {
        ui->checkBox_bmBalance->setChecked(false);
        setListWidgetSelections(balanceMounts, ui->listWidget_bmBalance);
    }
    ui->listWidget_bmScrub->clear();
    ui->listWidget_bmScrub->insertItems(0, mountpoints);
    QStringList scrubMounts = bmSettings->value("BTRFS_SCRUB_MOUNTPOINTS").toString().trimmed().split(":");
    if (scrubMounts.contains("auto")) {
        ui->checkBox_bmScrub->setChecked(true);
        ui->listWidget_bmScrub->setDisabled(true);
    } else {
        ui->checkBox_bmScrub->setChecked(false);
        setListWidgetSelections(scrubMounts, ui->listWidget_bmScrub);
    }
    ui->listWidget_bmDefrag->clear();
    ui->listWidget_bmDefrag->insertItems(0, mountpoints);
    QStringList defragMounts = bmSettings->value("BTRFS_DEFRAG_PATHS").toString().trimmed().split(":");
    if (defragMounts.contains("auto")) {
        ui->checkBox_bmDefrag->setChecked(true);
        ui->listWidget_bmDefrag->setDisabled(true);
    } else {
        ui->checkBox_bmDefrag->setChecked(false);
        setListWidgetSelections(defragMounts, ui->listWidget_bmDefrag);
    }
}

// Populates the UI for the BTRFS details tab
void BtrfsAssistant::populateSubvolList(const QString &uuid) {
    ui->listWidget_subvols->clear();

    if (uuid.isEmpty() || fsMap[uuid].subVolumes.size() <= 0)
        return;

    QMapIterator<QString, QString> i(fsMap[uuid].subVolumes);

    bool includeSnaps = ui->checkBox_includesnapshots->isChecked();

    while (i.hasNext()) {
        i.next();
        if (includeSnaps || !(isTimeshift(i.value()) || isSnapper(i.value())))
            ui->listWidget_subvols->addItem(i.value());
    }
    ui->listWidget_subvols->sortItems();
}

void BtrfsAssistant::on_checkBox_includesnapshots_clicked() { populateSubvolList(ui->comboBox_btrfsdevice->currentText()); }

void BtrfsAssistant::on_checkBox_bmBalance_clicked(bool checked) { ui->listWidget_bmBalance->setDisabled(checked); }

void BtrfsAssistant::on_checkBox_bmScrub_clicked(bool checked) { ui->listWidget_bmScrub->setDisabled(checked); }

void BtrfsAssistant::on_checkBox_bmDefrag_clicked(bool checked) { ui->listWidget_bmDefrag->setDisabled(checked); }

void BtrfsAssistant::updateServices(QList<QCheckBox *> checkboxList) {
    QStringList cmdList;

    for (auto checkbox : checkboxList) {
        QString service = checkbox->property("actionData").toString();
        if (service != "" && unitsEnabledSet.contains(service) != checkbox->isChecked()) {
            if (checkbox->isChecked())
                cmdList.append("systemctl enable --now " + service);
            else
                cmdList.append("systemctl disable --now " + service);
        }
    }

    runCmd(cmdList, false);
    loadEnabledUnits();
}

void BtrfsAssistant::on_pushButton_bmApply_clicked() {

    // First, update the services per the checkboxes
    updateServices(ui->scrollArea_bm->findChildren<QCheckBox *>());

    // Read and set the Btrfs maintenance settings
    bmSettings->setValue("BTRFS_BALANCE_PERIOD", ui->comboBox_bmBalanceFreq->currentText());
    bmSettings->setValue("BTRFS_SCRUB_PERIOD", ui->comboBox_bmScrubFreq->currentText());
    bmSettings->setValue("BTRFS_DEFRAG_PERIOD", ui->comboBox_bmDefragFreq->currentText());

    if (ui->checkBox_bmBalance->isChecked()) {
        bmSettings->setValue("BTRFS_BALANCE_MOUNTPOINTS", "auto");
    } else {
        const QList<QListWidgetItem *> balanceItems = ui->listWidget_bmBalance->selectedItems();
        QStringList balancePaths;
        for (const QListWidgetItem *item : balanceItems) {
            balancePaths.append(item->text());
        }
        bmSettings->setValue("BTRFS_BALANCE_MOUNTPOINTS", balancePaths.join(":"));
    }

    if (ui->checkBox_bmScrub->isChecked()) {
        bmSettings->setValue("BTRFS_SCRUB_MOUNTPOINTS", "auto");
    } else {
        const QList<QListWidgetItem *> scrubItems = ui->listWidget_bmScrub->selectedItems();
        QStringList scrubPaths;
        for (const QListWidgetItem *item : scrubItems) {
            scrubPaths.append(item->text());
        }
        bmSettings->setValue("BTRFS_SCRUB_MOUNTPOINTS", scrubPaths.join(":"));
    }

    if (ui->checkBox_bmDefrag->isChecked()) {
        bmSettings->setValue("BTRFS_DEFRAG_PATHS", "auto");
    } else {
        const QList<QListWidgetItem *> defragItems = ui->listWidget_bmDefrag->selectedItems();
        QStringList defragPaths;
        for (const QListWidgetItem *item : defragItems) {
            defragPaths.append(item->text());
        }
        bmSettings->setValue("BTRFS_DEFRAG_PATHS", defragPaths.join(":"));
    }

    bmSettings->sync();

    QMessageBox::information(0, tr("BTRFS Assistant"), tr("Changes applied successfully"));

    ui->pushButton_bmApply->clearFocus();
}

void BtrfsAssistant::on_pushButton_SnapperUnitsApply_clicked() {

    updateServices(ui->groupBox_snapperUnits->findChildren<QCheckBox *>());

    QMessageBox::information(0, tr("BTRFS Assistant"), tr("Changes applied successfully"));

    ui->pushButton_SnapperUnitsApply->clearFocus();
}

// Delete a subvolume after checking for a variety of errors
void BtrfsAssistant::on_pushButton_deletesubvol_clicked() {
    QString subvol = ui->listWidget_subvols->currentItem()->text();
    QString uuid = ui->comboBox_btrfsdevice->currentText();

    // Make sure the everything is good in the UI
    if (subvol.isEmpty() || uuid.isEmpty()) {
        displayError(tr("Nothing to delete!"));
        ui->pushButton_deletesubvol->clearFocus();
        return;
    }

    // get the subvolid, if it isn't found abort
    QString subvolid = fsMap[uuid].subVolumes.key(subvol);
    if (subvolid.isEmpty()) {
        displayError(tr("Failed to delete subvolume!") + "\n\n" + tr("subvolid missing from map"));
        ui->pushButton_deletesubvol->clearFocus();
        return;
    }

    // ensure the subvol isn't mounted, btrfs will delete a mounted subvol but we probably shouldn't
    if (isMounted(uuid, subvolid)) {
        displayError(tr("You cannot delete a mounted subvolume") + "\n\n" + tr("Please unmount the subvolume before continuing"));
        ui->pushButton_deletesubvol->clearFocus();
        return;
    }

    Result result;

    // Check to see if the subvolume is a snapper snapshot
    if (isSnapper(subvol) && hasSnapper) {
        QMessageBox::information(0, tr("Snapshot Delete"),
                                 tr("That subvolume is a snapper shapshot") + "\n\n" + tr("Please use the snapper tab to remove it"));
        return;
    } else {
        // Everything looks good so far, now we put up a confirmation box
        if (QMessageBox::question(0, tr("Confirm"), tr("Are you sure you want to delete ") + subvol) != QMessageBox::Yes)
            return;

        QString mountpoint = mountRoot(uuid);

        // Everything checks out, lets delete the subvol
        if (mountpoint.right(1) != "/")
            mountpoint += "/";
        result = runCmd("btrfs subvolume delete " + mountpoint + subvol, true);
    }

    if (result.exitCode == 0) {
        reloadSubvolList(uuid);
    } else
        displayError(tr("Process failed with output:") + "\n\n");

    ui->pushButton_deletesubvol->clearFocus();
}

// When a change is detected on the dropdown of btrfs devices, repopulate the UI based on the new selection
void BtrfsAssistant::on_comboBox_btrfsdevice_activated(int) {
    QString device = ui->comboBox_btrfsdevice->currentText();
    if (!device.isEmpty() && fsMap[device].totalSize != 0) {
        populateBtrfsUi(device);
        reloadSubvolList(device);
    }
    ui->comboBox_btrfsdevice->clearFocus();
}

// Restores a snapper snapshot after extensive error checking
void BtrfsAssistant::restoreSnapshot(const QString &uuid, QString subvolume) {
    // Make sure subvolume doesn't have a leading slash
    if (subvolume.startsWith("/"))
        subvolume = subvolume.right(subvolume.length() - 1);

    if (!isSnapper(subvolume)) {
        displayError(tr("This is not a snapshot that can be restored by this application"));
        return;
    }

    // get the subvolid, if it isn't found abort
    QString subvolid = fsMap[uuid].subVolumes.key(subvolume);
    if (subvolid.isEmpty()) {
        displayError(tr("Failed to restore snapshot!"));
        return;
    }

    // Now we need to find out what the target for the restore is
    QString prefix = subvolume.split(".snapshots").at(0);

    QString targetSubvolume;
    // If the prefix is empty, that means that we are trying to restore the subvolume mounted as /
    if (prefix.isEmpty()) {
        targetSubvolume = findRootSubvol();
    } else {
        // Strip the trailing /
        targetSubvolume = prefix.left(prefix.length() - 1);
    }

    // Get the subvolid of the target and do some additional error checking
    QString targetSubvolid = fsMap[uuid].subVolumes.key(targetSubvolume);
    if (targetSubvolid.isEmpty()) {
        displayError(tr("Target not found"));
        return;
    }

    // We are out of errors to check for, time to ask for confirmation
    if (QMessageBox::question(0, tr("Confirm"),
                              tr("Are you sure you want to restore ") + subvolume + tr(" to ", "as in from/to") + targetSubvolume) !=
        QMessageBox::Yes)
        return;

    // Ensure the root of the partition is mounted and get the mountpoint
    QString mountpoint = mountRoot(uuid);

    // Make sure we have a trailing /
    if (mountpoint.right(1) != "/")
        mountpoint += "/";

    // We are out of excuses, time to do the restore....carefully
    QString targetBackup = "restore_backup_" + targetSubvolume + "_" + QTime::currentTime().toString("HHmmsszzz");

    QDir dirWorker;

    // Find the children before we start
    const QStringList subvols = findBtrfsChildren(targetSubvolid, uuid);

    // Rename the target
    if (!renameSubvolume(QDir::cleanPath(mountpoint + targetSubvolume), QDir::cleanPath(mountpoint + targetBackup))) {
        displayError(tr("Failed to make a backup of target subvolume"));
        return;
    }

    // We moved the snapshot so we need to change the location
    QString newSubvolume;
    if (subvolume.startsWith(targetSubvolume))
        newSubvolume = targetBackup + subvolume.right(subvolume.length() - targetSubvolume.length());
    else
        newSubvolume = targetBackup + "/" + subvolume;

    // Place a snapshot of the source where the target was
    runCmd("btrfs subvolume snapshot " + mountpoint + newSubvolume + " " + mountpoint + targetSubvolume, false);

    // Make sure it worked
    if (!dirWorker.exists(mountpoint + targetSubvolume)) {
        // That failed, try to put the old one back
        renameSubvolume(QDir::cleanPath(mountpoint + targetBackup), QDir::cleanPath(mountpoint + targetSubvolume));
        displayError(tr("Failed to restore subvolume!") + "\n\n" +
                     tr("Snapshot restore failed.  Please verify the status of your system before rebooting"));
        return;
    }

    // The restore was successful, now we need to move any child subvolumes into the target
    QString childSubvolPath;
    for (const QString &childSubvol : subvols) {
        if (childSubvol.startsWith(targetSubvolume)) {
            // Strip the old subvolname
            childSubvolPath = childSubvol.right(childSubvol.length() - (targetSubvolume.length() + 1));
        } else {
            childSubvolPath = childSubvol;
        }

        // rename snapshot
        QString sourcePath = QDir::cleanPath(mountpoint + targetBackup + QDir::separator() + childSubvolPath);
        QString destinationPath = QDir::cleanPath(mountpoint + targetSubvolume + "/.");
        if (!renameSubvolume(sourcePath, destinationPath)) {
            // If this fails, not much can be done except let the user know
            displayError(tr("The restore was successful but the migration of the nested subvolumes failed") + "\n\n" +
                         tr("Please migrate the those subvolumes manually"));
            return;
        }
    }

    // If we get here I guess it worked
    QMessageBox::information(0, tr("Snapshot Restore"),
                             tr("Snapshot restoration complete.") + "\n\n" + tr("A copy of the original subvolume has been saved as ") +
                                 targetBackup + "\n\n" + tr("Please reboot immediately"));
}

// Loads the snapper configs and snapshots
void BtrfsAssistant::loadSnapper() {
    // If snapper isn't installed, no need to continue
    if (!hasSnapper)
        return;

    // Load the list of valid configs
    ui->comboBox_snapper_configs->clear();
    ui->comboBox_snapper_config_settings->clear();
    snapperConfigs.clear();
    snapperSnapshots.clear();
    QString outputList = runCmd("snapper list-configs | tail -n +3", false).output;

    if (outputList.isEmpty())
        return;

    const QStringList outputAsList = outputList.split('\n');
    for (const QString &line : outputAsList) {
        // for each config, add to the map and add it's snapshots to the vector
        QString name = line.split('|').at(0).trimmed();
        snapperConfigs[name] = line.split('|').at(1).trimmed();
        ui->comboBox_snapper_configs->addItem(name);
        ui->comboBox_snapper_config_settings->addItem(name);

        // If we are booted off the snapshot we need to handle the root snapshots manually
        if (name == "root" && isSnapBoot) {
            QString output = runCmd("LANG=C findmnt -no uuid,options /", false).output;
            if (output.isEmpty())
                continue;

            QString uuid = output.split(' ').at(0).trimmed();
            QString options = output.right(output.length() - uuid.length()).trimmed();
            if (options.isEmpty() || uuid.isEmpty())
                continue;

            QString subvol;
            const QStringList optionsList = options.split(',');
            for (const QString &option : optionsList) {
                if (option.startsWith("subvol="))
                    subvol = option.split("subvol=").at(1);
            }

            if (subvol.isEmpty() || !subvol.contains(".snapshots"))
                continue;

            // Make sure subvolume doesn't have a leading slash
            if (subvol.startsWith("/"))
                subvol = subvol.right(subvol.length() - 1);

            if (!isSnapper(subvol))
                continue;

            // get the subvolid, if it isn't found abort
            QString subvolid = fsMap[uuid].subVolumes.key(subvol);
            if (subvolid.isEmpty())
                continue;

            // Now we need to find out where the snapshots are actually stored
            QString prefix = subvol.split(".snapshots").at(0);

            // It shouldn't be possible for the prefix to empty when booted off a snapshot but we check anyway
            if (prefix.isEmpty())
                continue;

            // Make sure the root of the partition is mounted
            QString mountpoint = mountRoot(uuid);

            // Make sure we have a trailing /
            if (mountpoint.right(1) != "/")
                mountpoint += "/";

            QString findOutput = runCmd("find " + mountpoint + prefix + ".snapshots -maxdepth 2 -name info.xml", false).output;

            const QStringList findOutputList = findOutput.split('\n');
            for (const QString &fileName : findOutputList) {
                SnapperSnapshots snap = getSnapperMeta(fileName);
                if (snap.number == 0)
                    snapperSnapshots[name].append(snap);
            }
        } else {
            QString list = runCmd("snapper -c " + name + " list --columns number,date,description | tail -n +4", false).output;
            if (list.isEmpty())
                continue;
            const QStringList snapperList = list.split('\n');
            for (const QString &snap : snapperList)
                snapperSnapshots[name].append(
                    {snap.split('|').at(0).trimmed().toInt(), snap.split('|').at(1).trimmed(), snap.split('|').at(2).trimmed()});
        }
    }
}

// Populates the main grid on the Snapper tab
void BtrfsAssistant::populateSnapperGrid() {
    if (ui->checkBox_snapper_restore->isChecked()) {
        QString config = ui->comboBox_snapper_configs->currentText();

        // Clear the table and set the headers
        ui->tableWidget_snapper->clear();
        ui->tableWidget_snapper->setColumnCount(3);
        ui->tableWidget_snapper->setHorizontalHeaderItem(0, new QTableWidgetItem(tr("Subvolume")));
        ui->tableWidget_snapper->setHorizontalHeaderItem(1, new QTableWidgetItem(tr("Date/Time")));
        ui->tableWidget_snapper->setHorizontalHeaderItem(2, new QTableWidgetItem(tr("Description")));

        // Make sure there is something to populate
        if (snapperSubvolumes[config].isEmpty())
            return;

        // Populate the table
        ui->tableWidget_snapper->setRowCount(snapperSubvolumes[config].size());
        for (int i = 0; i < snapperSubvolumes[config].size(); i++) {
            QTableWidgetItem *subvol = new QTableWidgetItem(snapperSubvolumes[config].at(i).subvol);
            ui->tableWidget_snapper->setItem(i, 0, subvol);
            ui->tableWidget_snapper->setItem(i, 1, new QTableWidgetItem(snapperSubvolumes[config].at(i).time));
            ui->tableWidget_snapper->setItem(i, 2, new QTableWidgetItem(snapperSubvolumes[config].at(i).desc));
        }
    } else {
        QString config = ui->comboBox_snapper_configs->currentText();

        // Clear the table and set the headers
        ui->tableWidget_snapper->clear();
        ui->tableWidget_snapper->setColumnCount(3);
        ui->tableWidget_snapper->setHorizontalHeaderItem(0, new QTableWidgetItem(tr("Number", "The number associated with a snapshot")));
        ui->tableWidget_snapper->setHorizontalHeaderItem(1, new QTableWidgetItem(tr("Date/Time")));
        ui->tableWidget_snapper->setHorizontalHeaderItem(2, new QTableWidgetItem(tr("Description")));

        // Make sure there is something to populate
        if (snapperSnapshots[config].isEmpty())
            return;

        // Populate the table
        ui->tableWidget_snapper->setRowCount(snapperSnapshots[config].size());
        for (int i = 0; i < snapperSnapshots[config].size(); i++) {
            QTableWidgetItem *number = new QTableWidgetItem(snapperSnapshots[config].at(i).number);
            number->setData(Qt::DisplayRole, snapperSnapshots[config].at(i).number);
            ui->tableWidget_snapper->setItem(i, 0, number);
            ui->tableWidget_snapper->setItem(i, 1, new QTableWidgetItem(snapperSnapshots[config].at(i).time));
            ui->tableWidget_snapper->setItem(i, 2, new QTableWidgetItem(snapperSnapshots[config].at(i).desc));
        }
    }

    // Resize the colums to make everything fit
    ui->tableWidget_snapper->resizeColumnsToContents();
    ui->tableWidget_snapper->sortItems(0, Qt::DescendingOrder);
}

// Repopulate the grid when a different config is selected
void BtrfsAssistant::on_comboBox_snapper_configs_activated(int) {
    populateSnapperGrid();
    ui->comboBox_snapper_configs->clearFocus();
}

// When the create config button is clicked, use the inputted data to create the config
void BtrfsAssistant::on_pushButton_snapper_create_clicked() {
    QString config = ui->comboBox_snapper_configs->currentText();

    // If snapper isn't installed, we should bail
    if (!hasSnapper)
        return;

    // This shouldn't be possible but we check anyway
    if (config.isEmpty()) {
        displayError(tr("No config selected for snapshot"));
        return;
    }

    // OK, let's go ahead and take the snapshot
    runCmd("snapper -c " + config + " create -d 'Manual Snapshot'", false);

    loadSnapper();
    ui->comboBox_snapper_configs->setCurrentText(config);
    populateSnapperGrid();

    ui->pushButton_snapper_create->clearFocus();
}

// When the snapper delete config button is clicked, call snapper to remove the config
void BtrfsAssistant::on_pushButton_snapper_delete_clicked() {
    if (ui->tableWidget_snapper->currentRow() == -1) {
        displayError(tr("Nothing selected!"));
        return;
    }

    // Get all the rows that were selected
    const QList<QTableWidgetItem *> list = ui->tableWidget_snapper->selectedItems();

    QSet<QString> numbers;

    // Get the snapshot numbers for the selected rows
    for (const QTableWidgetItem *item : list) {
        numbers.insert(ui->tableWidget_snapper->item(item->row(), 0)->text());
    }

    // Ask for confirmation
    if (QMessageBox::question(0, tr("Confirm"), tr("Are you sure you want to delete the selected snapshot(s)?")) != QMessageBox::Yes)
        return;

    QString config = ui->comboBox_snapper_configs->currentText();

    // Delete each selected snapshot
    for (const QString &number : qAsConst(numbers)) {
        // This shouldn't be possible but we check anyway
        if (config.isEmpty() || number.isEmpty()) {
            displayError(tr("Cannot delete snapshot"));
            return;
        }

        // Delete the snapshot
        runCmd("snapper -c " + config + " delete " + number, false);
    }

    // Reload the UI since something changed
    loadSnapper();
    ui->comboBox_snapper_configs->setCurrentText(config);
    populateSnapperGrid();

    ui->pushButton_snapper_delete->clearFocus();
}

// Populates a selected config on the Snapper Settings tab
void BtrfsAssistant::populateSnapperConfigSettings() {
    QString name = ui->comboBox_snapper_config_settings->currentText();
    if (name.isEmpty())
        return;

    QString output = runCmd("snapper -c " + name + " get-config | tail -n +3", false).output;

    if (output.isEmpty())
        return;

    ui->label_snapper_config_name->setText(name);
    const QStringList outputList = output.split('\n');
    for (const QString &line : outputList) {
        if (line.isEmpty())
            continue;
        QString key = line.split('|').at(0).trimmed();
        QString value = line.split('|').at(1).trimmed();
        if (key == "SUBVOLUME")
            ui->label_snapper_backup_path->setText(value);
        else if (key == "TIMELINE_CREATE")
            ui->checkBox_snapper_enabletimeline->setChecked(value.toStdString() == "yes");
        else if (key == "TIMELINE_LIMIT_HOURLY")
            ui->spinBox_snapper_hourly->setValue(value.toInt());
        else if (key == "TIMELINE_LIMIT_DAILY")
            ui->spinBox_snapper_daily->setValue(value.toInt());
        else if (key == "TIMELINE_LIMIT_WEEKLY")
            ui->spinBox_snapper_weekly->setValue(value.toInt());
        else if (key == "TIMELINE_LIMIT_MONTHLY")
            ui->spinBox_snapper_monthly->setValue(value.toInt());
        else if (key == "TIMELINE_LIMIT_YEARLY")
            ui->spinBox_snapper_yearly->setValue(value.toInt());
        else if (key == "NUMBER_LIMIT")
            ui->spinBox_snapper_pacman->setValue(value.toInt());
    }

    snapperTimelineEnable(ui->checkBox_snapper_enabletimeline->isChecked());
}

// Enables or disables the timeline spinboxes to match the timeline checkbox
void BtrfsAssistant::snapperTimelineEnable(bool enable) {
    ui->spinBox_snapper_hourly->setEnabled(enable);
    ui->spinBox_snapper_daily->setEnabled(enable);
    ui->spinBox_snapper_weekly->setEnabled(enable);
    ui->spinBox_snapper_monthly->setEnabled(enable);
    ui->spinBox_snapper_yearly->setEnabled(enable);
}

void BtrfsAssistant::on_checkBox_snapper_enabletimeline_clicked(bool checked) { snapperTimelineEnable(checked); }

// When a new config is selected repopulate the UI
void BtrfsAssistant::on_comboBox_snapper_config_settings_activated(int) {
    populateSnapperConfigSettings();

    ui->comboBox_snapper_config_settings->clearFocus();
}

// Uses snapper to create a new config or save an existing config when the save button is pressed
void BtrfsAssistant::on_pushButton_snapper_save_config_clicked() {
    QString name;

    // If the settings box is visible we are changing settings on an existing config
    if (ui->groupBox_snapper_config_settings->isVisible()) {
        name = ui->comboBox_snapper_config_settings->currentText();
        if (name.isEmpty()) {
            displayError(tr("Failed to save changes"));
            ui->pushButton_snapper_save_config->clearFocus();
            return;
        }

        QString command = "snapper -c " + name + " set-config ";
        command += "\"TIMELINE_CREATE=" + QString(ui->checkBox_snapper_enabletimeline->isChecked() ? "yes" : "no") + "\"";
        command += " \"TIMELINE_LIMIT_HOURLY=" + QString::number(ui->spinBox_snapper_hourly->value()) + "\"";
        command += " \"TIMELINE_LIMIT_DAILY=" + QString::number(ui->spinBox_snapper_daily->value()) + "\"";
        command += " \"TIMELINE_LIMIT_WEEKLY=" + QString::number(ui->spinBox_snapper_weekly->value()) + "\"";
        command += " \"TIMELINE_LIMIT_MONTHLY=" + QString::number(ui->spinBox_snapper_monthly->value()) + "\"";
        command += " \"TIMELINE_LIMIT_YEARLY=" + QString::number(ui->spinBox_snapper_yearly->value()) + "\"";
        command += " \"NUMBER_LIMIT=" + QString::number(ui->spinBox_snapper_pacman->value()) + "\"";

        runCmd(command, false);

        QMessageBox::information(0, tr("Snapper"), tr("Changes saved"));
    } else { // This is new config we are creating
        name = ui->lineEdit_snapper_name->text();

        // Remove any whitespace from name
        name = name.simplified().replace(" ", "");

        if (name.isEmpty()) {
            displayError(tr("Please enter a valid name"));
            ui->pushButton_snapper_save_config->clearFocus();
            return;
        }

        if (snapperConfigs.contains(name)) {
            displayError(tr("That name is already in use!"));
            ui->pushButton_snapper_save_config->clearFocus();
            return;
        }

        // Create the new config
        runCmd("snapper -c " + name + " create-config " + ui->comboBox_snapper_path->currentText(), false);

        // Reload the UI
        loadSnapper();
        ui->comboBox_snapper_config_settings->setCurrentText(name);
        populateSnapperGrid();
        populateSnapperConfigSettings();

        // Put the ui back in edit mode
        ui->groupBox_snapper_config_display->show();
        ui->groupBox_snapper_config_edit->hide();
        ui->groupBox_snapper_config_settings->show();
    }

    ui->pushButton_snapper_save_config->clearFocus();
}

// Switches the snapper config between edit config and new config mode
void BtrfsAssistant::on_pushButton_snapper_new_config_clicked() {
    if (ui->groupBox_snapper_config_edit->isVisible()) {
        ui->lineEdit_snapper_name->clear();

        // Put the ui back in edit mode
        ui->groupBox_snapper_config_display->show();
        ui->groupBox_snapper_config_edit->hide();
        ui->groupBox_snapper_config_settings->show();

        ui->pushButton_snapper_new_config->setText(tr("New Config"));
        ui->pushButton_snapper_new_config->clearFocus();
    } else {
        // Get a list of btrfs mountpoints that could be backed up
        output = runCmd("findmnt --real -nlo FSTYPE,TARGET | grep \"^btrfs\" | awk '{print $2}'", false).output;

        if (output.isEmpty()) {
            displayError(tr("No btrfs subvolumes found"));
            return;
        }

        // Populate the list of mountpoints after checking that their isn't already a config
        ui->comboBox_snapper_path->clear();
        QStringList outputList = output.split('\n');
        for (const QString &line : outputList)
            if (snapperConfigs.key(line.trimmed()).isEmpty())
                ui->comboBox_snapper_path->addItem(line.trimmed());

        // Put the UI in create config mode
        ui->groupBox_snapper_config_display->hide();
        ui->groupBox_snapper_config_edit->show();
        ui->groupBox_snapper_config_settings->hide();

        ui->pushButton_snapper_new_config->setText(tr("Cancel New Config"));
        ui->pushButton_snapper_new_config->clearFocus();
    }
}

// Use snapper to delete a config when the delete config button is pressed
void BtrfsAssistant::on_pushButton_snapper_delete_config_clicked() {
    QString name = ui->comboBox_snapper_config_settings->currentText();

    if (name.isEmpty()) {
        displayError(tr("No config selected"));
        ui->pushButton_snapper_delete_config->clearFocus();
        return;
    }

    if (name == "root") {
        displayError(tr("You may not don't delete the root config"));
        ui->pushButton_snapper_delete_config->clearFocus();
        return;
    }

    // Ask for confirmation
    if (QMessageBox::question(0, tr("Please Confirm"),
                              tr("Are you sure you want to delete ") + name + "\n\n" + tr("This action cannot be undone")) !=
        QMessageBox::Yes) {
        ui->pushButton_snapper_delete_config->clearFocus();
        return;
    }

    // Delete the config
    runCmd("snapper -c " + name + " delete-config", false);

    // Reload the UI with the new list of configs
    loadSnapper();
    populateSnapperGrid();
    populateSnapperConfigSettings();

    ui->pushButton_snapper_delete_config->clearFocus();
}

void BtrfsAssistant::on_checkBox_snapper_restore_clicked(bool checked) {
    enableRestoreMode(checked);

    ui->checkBox_snapper_restore->clearFocus();
}

// Puts the snapper tab in restore mode
void BtrfsAssistant::enableRestoreMode(bool enable) {
    ui->pushButton_snapper_create->setEnabled(!enable);
    ui->pushButton_snapper_delete->setEnabled(!enable);
    ui->pushButton_restore_snapshot->setEnabled(enable);

    if (enable) {
        ui->label_snapper_combo->setText(tr("Select Subvolume:"));
        ui->comboBox_snapper_configs->clear();
        ui->tableWidget_snapper->clear();
        loadSnapperRestoreMode();
        populateSnapperGrid();
    } else {
        ui->label_snapper_combo->setText(tr("Select Config:"));
        loadSnapper();
        populateSnapperGrid();
    }
}

void BtrfsAssistant::on_pushButton_restore_snapshot_clicked() {
    // First lets double check to ensure we are in restore mode
    if (!ui->checkBox_snapper_restore->isChecked()) {
        displayError(tr("Please enter restore mode before trying to restore a snapshot"));
        return;
    }

    if (ui->tableWidget_snapper->currentRow() == -1) {
        displayError(tr("Nothing selected!"));
        return;
    }

    QString subvolName = ui->comboBox_snapper_configs->currentText();
    QString subvol = ui->tableWidget_snapper->item(ui->tableWidget_snapper->currentRow(), 0)->text();

    // These shouldn't be possible but check anyway
    if (!snapperSubvolumes.contains(subvolName) || snapperSubvolumes[subvolName].size() == 0) {
        displayError(tr("Failed to restore snapshot"));
        return;
    }

    // For a given subvol they all have the same uuid so we can just use the first one
    QString uuid = snapperSubvolumes[subvolName].at(0).uuid;

    restoreSnapshot(uuid, subvol);

    ui->pushButton_restore_snapshot->clearFocus();
}

// Verify we are booted off a snapshot and then offer to restore it
QMap<QString, QString> BtrfsAssistant::getSnapshotBoot() {
    isSnapBoot = false;

    QFile inputFile("/proc/cmdline");
    if (!inputFile.open(QIODevice::ReadOnly))
        return QMap<QString, QString>();
    QTextStream in(&inputFile);
    QString cmdline = in.readAll();
    QString uuid = QRegularExpression("(?<=root=UUID=)([\\S]*)").match(cmdline).captured(0);
    QString subvol = QRegularExpression("rootflags=.*subvol=([^,|\\s]*)").match(cmdline).captured(1);
    if (uuid.isNull() || subvol.isNull() || !subvol.contains(".snapshots"))
        return QMap<QString, QString>();

    // If we get to here we should be booted off the snapshot stored in subvol
    isSnapBoot = true;

    return {{"uuid", uuid}, {"subvol", subvol}};
}

// When booting off a snapshot this forcibly switches to the correct tab and enables the restore mode
void BtrfsAssistant::switchToSnapperRestore() {
    ui->tabWidget->setTabVisible(ui->tabWidget->indexOf(ui->tab_snapper_general), true);
    ui->tabWidget->setCurrentIndex(ui->tabWidget->indexOf(ui->tab_snapper_general));
    ui->checkBox_snapper_restore->setChecked(true);
    enableRestoreMode(true);

    return;
}

// Populates the UI for the restore mode of the snapper tab
void BtrfsAssistant::loadSnapperRestoreMode() {
    // Sanity check
    if (!ui->checkBox_snapper_restore->isChecked())
        return;

    // Clear the existing info
    snapperSubvolumes.clear();
    ui->comboBox_snapper_configs->clear();

    // Get a list of the btrfs filesystems and loop over them
    const QStringList btrfsFilesystems = getBTRFSFilesystems();
    for (const QString &uuid : btrfsFilesystems) {
        // First get a mountpoint associated with uuid
        QString output = runCmd("findmnt --real -nlo UUID,TARGET | grep " + uuid + " | head -n 1", false).output;

        if (output.isEmpty())
            continue;

        QString target = output.split(' ').at(1);

        if (target.isEmpty())
            continue;

        // Now we can get all the subvolumes tied to that mountpoint
        output = runCmd("btrfs subvolume list " + target, false).output;

        if (output.isEmpty())
            continue;

        // We need to ensure the root is mounted and get the mountpoint
        QString mountpoint = mountRoot(uuid);

        // Ensure it has a trailing /
        if (mountpoint.right(1) != "/")
            mountpoint += "/";

        QStringList outputList = output.split('\n');
        for (const QString &line : outputList) {
            SnapperSubvolume subvol;
            if (line.isEmpty())
                continue;

            subvol.uuid = uuid;
            subvol.subvolid = line.split(' ').at(1).trimmed();
            subvol.subvol = line.split(' ').at(8).trimmed();

            // Check if it is snapper snapshot
            if (!isSnapper(subvol.subvol))
                continue;

            // It is a snapshot so now we parse it and read the snapper XML
            QString end = "snapshot";
            QString filename = subvol.subvol.left(subvol.subvol.length() - end.length()) + "info.xml";

            // If the normal root is mounted the root snapshots will be at /.snapshots
            if (subvol.subvol.startsWith(".snapshots"))
                filename = QDir::cleanPath(QDir::separator() + filename);
            else
                filename = QDir::cleanPath(mountpoint + filename);

            SnapperSnapshots snap = getSnapperMeta(filename);

            if (snap.number == 0)
                continue;

            subvol.desc = snap.desc;
            subvol.time = snap.time;

            QString prefix = subvol.subvol.split(".snapshots").at(0).trimmed();

            if (prefix == "") {
                QString optionsOutput = runCmd("LANG=C findmnt -no options /", false).output.trimmed();
                if (optionsOutput.isEmpty())
                    return;

                QString subvolOption;
                const QStringList optionsList = optionsOutput.split(',');
                for (const QString &option : optionsList) {
                    if (option.startsWith("subvol="))
                        subvolOption = option.split("subvol=").at(1);
                }
                if (subvolOption.startsWith("/"))
                    subvolOption = subvolOption.right(subvolOption.length() - 1);

                if (subvolOption.isEmpty())
                    prefix = "root";
                else
                    prefix = subvolOption;
            } else
                prefix = prefix.left(prefix.length() - 1);

            snapperSubvolumes[prefix].append(subvol);
        }
    }

    const QStringList snapperKeys = snapperSubvolumes.keys();
    for (const QString &key : snapperKeys)
        ui->comboBox_snapper_configs->addItem(key);
}
