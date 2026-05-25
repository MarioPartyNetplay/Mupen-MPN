/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "UserInterface/Dialog/Cheats/CheatsCommon.hpp"
#include "UserInterface/Dialog/Cheats/CheatsDialog.hpp"
#include "Utilities/QtMessageBox.hpp"
#include "NetplaySessionDialog.hpp"
#include "Netplay/NatTraversal/NatTraversalProtocol.hpp"

#include <QJsonDocument>
#include <QPushButton>
#include <QMessageBox>
#include <QShowEvent>
#include <QAbstractButton>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalBlocker>

#include <RMG-Core/Error.hpp>
#include <RMG-Core/Directories.hpp>
#include <RMG-Core/Settings.hpp>
#include <RMG-Core/CachedRomHeaderAndSettings.hpp>
#include <QTimer>
#include <RMG-Core/Netplay.hpp>
#include <RMG-Core/Rom.hpp>

using namespace UserInterface::Dialog;
using namespace Utilities;

namespace {

const QStringList& netplaySaveExtensions()
{
    static const QStringList extensions = { ".eep", ".sra", ".srm", ".fla", ".mpk" };
    return extensions;
}

QString sanitizeSaveBaseName(QString name)
{
    const QString invalidChars = QStringLiteral(":<>\"/\\|?*");
    for (const QChar ch : invalidChars) {
        name.replace(ch, '_');
    }
    return name;
}

QString buildMupenSaveBaseName(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    const int format = CoreSettingsGetIntValue(SettingsID::Core_SaveFileNameFormat);
    if (format == 0) {
        return sanitizeSaveBaseName(QString::fromStdString(header.Name));
    }

    QString base;
    const QString goodName = QString::fromStdString(settings.GoodName);
    if (!goodName.contains(QStringLiteral("(unknown rom)"))) {
        base = goodName.left(32);
    } else if (!header.Name.empty()) {
        base = QString::fromStdString(header.Name);
    } else {
        base = QStringLiteral("unknown");
    }

    const QString md5Prefix = QString::fromStdString(settings.MD5).left(8);
    return sanitizeSaveBaseName(base + "-" + md5Prefix);
}

void appendSaveFileIfExists(QJsonArray& saveFiles, const QDir& directory, const QString& filename)
{
    for (const auto& value : saveFiles) {
        if (value.toObject().value("filename").toString() == filename) {
            return;
        }
    }

    const QString filePath = directory.filePath(filename);
    QFile file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return;
    }

    const QByteArray data = file.readAll();
    file.close();

    QJsonObject saveFile;
    saveFile["filename"] = filename;
    saveFile["size"] = static_cast<qint64>(data.size());
    saveFile["data"] = QString::fromLatin1(data.toBase64());
    saveFiles.append(saveFile);
}

QJsonArray buildEnabledCheatsSnapshot(const QString& romFile)
{
    std::vector<CoreCheat> cheats;
    QJsonArray hostCheats;

    if (!CoreGetCurrentCheats(romFile.toStdU32String(), cheats))
    {
        return hostCheats;
    }

    for (const auto& cheat : cheats)
    {
        if (CoreIsCheatEnabled(romFile.toStdU32String(), cheat))
        {
            CheatsCommon::EnableCheat(true, hostCheats, romFile, cheat, true);
        }
    }

    return hostCheats;
}

QJsonArray buildSaveSyncFiles(const QString& romFile)
{
    QJsonArray saveFiles;
    const auto saveDirectory = CoreGetSaveDirectory();
    const QDir directory(QString::fromStdString(saveDirectory.string()));

    QStringList saveBaseNames;
    saveBaseNames << QFileInfo(romFile).completeBaseName();

    CoreRomType type = {};
    CoreRomHeader header = {};
    CoreRomSettings defaultSettings = {};
    CoreRomSettings settings = {};
    if (CoreGetCachedRomHeaderAndSettings(romFile.toStdU32String(), &type, &header, &defaultSettings, &settings)) {
        saveBaseNames << buildMupenSaveBaseName(header, settings);
        if (!settings.GoodName.empty()) {
            saveBaseNames << sanitizeSaveBaseName(QString::fromStdString(settings.GoodName));
        }
        if (!header.Name.empty()) {
            saveBaseNames << sanitizeSaveBaseName(QString::fromStdString(header.Name));
        }
    }

    saveBaseNames.removeDuplicates();

    for (const QString& baseName : saveBaseNames) {
        for (const QString& extension : netplaySaveExtensions()) {
            appendSaveFileIfExists(saveFiles, directory, baseName + extension);
        }
    }

    return saveFiles;
}

}

NetplaySessionDialog::NetplaySessionDialog(QWidget *parent, Netplay::NetplayCoordinator* coordinator, QString sessionFile) 
    : QDialog(parent), coordinator(coordinator), sessionFile(sessionFile)
{
    this->setupUi(this);
    this->setWindowIcon(QIcon(":Resource/RMG.png"));
    this->setWindowFlags(this->windowFlags() | Qt::WindowMinimizeButtonHint);

    // Parse session JSON
    QJsonDocument sessionDoc = QJsonDocument::fromJson(sessionFile.toUtf8());
    QJsonObject sessionJson = sessionDoc.object();
    this->romFile = sessionJson.value("rom_path").toString();
    this->sessionSlot = sessionJson.value("slot").toInt(sessionJson.value("slotIndex").toInt(-1));
    if (this->sessionSlot < 0 && this->coordinator) {
        this->sessionSlot = this->coordinator->getGameSession().localSlot;
    }

    const QString hostCode = sessionJson.value("host_code").toString();
    if (this->sessionSlot == 0 && !hostCode.isEmpty())
    {
        const int hostingPort = sessionJson.value("server_port").toInt(Netplay::kDefaultNetplayHostingPort);
        this->natTraversalClient = std::make_unique<Netplay::NatTraversalClient>(this);
        this->natTraversalClient->resumeHosting(hostCode, static_cast<uint16_t>(hostingPort));

        this->natIndexClient = std::make_unique<Netplay::NatTraversalIndexClient>(this);
        connect(this->natIndexClient.get(), &Netplay::NatTraversalIndexClient::published,
                this, [](const QString& key) {
            qDebug() << "Updated session index:" << key;
        });
        connect(this->natIndexClient.get(), &Netplay::NatTraversalIndexClient::publishFailed,
                this, [](const QString& reason) {
            qWarning() << "Failed to update session index:" << reason;
        });
    }

    // Connect coordinator signals
    connect(this->coordinator, &Netplay::NetplayCoordinator::playersUpdated, this,
            [this](const QList<Netplay::SocketIOClient::PlayerInfo>& players) {
        QStringList names;
        for (const auto& player : players) {
            names << player.name;
        }
        this->on_coordinator_playersUpdated(names);
        this->publishHostSessionIndex(this->coordinator->isInGame());
    });
    connect(this->coordinator, &Netplay::NetplayCoordinator::gameStarted, this,
            [this](const Netplay::NetplayCoordinator::GameSession& session) {
        this->on_coordinator_gameStarted(session.localSlot);
        this->publishHostSessionIndex(true);
    });
    connect(this->coordinator, &Netplay::NetplayCoordinator::chatMessageReceived,
            this, &NetplaySessionDialog::on_coordinator_chatMessageReceived);
    connect(this->coordinator, &Netplay::NetplayCoordinator::motdReceived, 
            this, &NetplaySessionDialog::on_coordinator_motdReceived);
    connect(this->coordinator, &Netplay::NetplayCoordinator::cheatsUpdated,
            this, &NetplaySessionDialog::on_coordinator_cheatsUpdated);
    connect(this->coordinator, &Netplay::NetplayCoordinator::saveSyncReceived,
            this, &NetplaySessionDialog::on_coordinator_saveSyncReceived);
    connect(this->coordinator, &Netplay::NetplayCoordinator::inputDelayChanged,
            this, [this](int frames) {
        QSignalBlocker blocker(this->bufferDelaySpinBox);
        this->bufferDelaySpinBox->setValue(frames);
        this->coordinator->setInputDelayFrames(frames); 
    });
    
    // Auto-enable pre-toggled cheats for host
    if (this->sessionSlot == 0)
    {
        this->syncHostSessionState();
    }

    // Connect to connection state signals to enable/disable chat
    connect(this->coordinator, &Netplay::NetplayCoordinator::connected, this, &NetplaySessionDialog::on_netplay_connected);
    connect(this->coordinator, &Netplay::NetplayCoordinator::disconnected, this, &NetplaySessionDialog::on_netplay_disconnected);
    connect(this->coordinator, &Netplay::NetplayCoordinator::stateChanged, this, &NetplaySessionDialog::on_coordinator_stateChanged);
    const int initialBufferDelay = sessionJson.value("buffer_delay").toInt(this->coordinator->getInputDelayFrames());
    this->bufferDelaySpinBox->setValue(initialBufferDelay);
    this->coordinator->setInputDelayFrames(initialBufferDelay);
    connect(this->coordinator, &Netplay::NetplayCoordinator::roomJoined,
            this, [this](const QString&, int slot) {
        this->sessionSlot = slot;
        QJsonDocument sessionDoc = QJsonDocument::fromJson(this->sessionFile.toUtf8());
        QJsonObject sessionJson = sessionDoc.object();
        sessionJson.insert("slot", slot);
        sessionJson.insert("slotIndex", slot);
        this->sessionFile = QJsonDocument(sessionJson).toJson(QJsonDocument::Compact);
        this->applyHostOnlyControlsVisibility();
    });

    const bool isHost = this->isLocalSessionHost();
    this->bufferDelaySpinBox->setReadOnly(!isHost);
    connect(this->bufferDelaySpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int value) {
        if (!this->isLocalSessionHost()) {
            return;
        }
        this->coordinator->sendInputDelayUpdate(value);
        QJsonDocument sessionDoc = QJsonDocument::fromJson(this->sessionFile.toUtf8());
        QJsonObject sessionJson = sessionDoc.object();
        sessionJson.insert("buffer_delay", value);
        this->sessionFile = QJsonDocument(sessionJson).toJson(QJsonDocument::Compact);
    });
    if (this->isLocalSessionHost()) {
        this->coordinator->sendInputDelayUpdate(initialBufferDelay);
    }
    connect(this->coordinator, &Netplay::NetplayCoordinator::inputDelayChanged,
            this, [this](int frames) {
        QSignalBlocker blocker(this->bufferDelaySpinBox);
        this->bufferDelaySpinBox->setValue(frames);
        this->coordinator->setInputDelayFrames(frames); 
    });


    // Disable chat input and send button by default
    this->chatLineEdit->setEnabled(false);
    this->sendPushButton->setEnabled(false);

    // Enable chat immediately if we're already in an active session state.
    const auto currentState = this->coordinator->getCurrentState();
    if (currentState == Netplay::NetplayCoordinator::Connected ||
        currentState == Netplay::NetplayCoordinator::InLobby ||
        currentState == Netplay::NetplayCoordinator::InGame) {
        this->chatLineEdit->setEnabled(true);
        this->sendPushButton->setEnabled(!this->chatLineEdit->text().isEmpty());
    }

    this->publishHostSessionIndex(currentState == Netplay::NetplayCoordinator::InGame);

    // Setup UI
    QPushButton* startButton = this->buttonBox->button(QDialogButtonBox::Ok);
    startButton->setText("Start");
    startButton->setEnabled(false);

    QPushButton* cheatsButton = this->buttonBox->button(QDialogButtonBox::RestoreDefaults);
    cheatsButton->setText("Cheats");
    cheatsButton->setIcon(QIcon::fromTheme("code-box-line"));

    // Display session information
    if (!sessionJson.isEmpty()) {
        // Set session name with room code and port
        QLineEdit* sessionNameEdit = nullptr;
        if (this->sessionNameLineEdit) {
            sessionNameEdit = this->sessionNameLineEdit;
        } else {
            sessionNameEdit = this->findChild<QLineEdit*>("sessionNameLineEdit");
        }
        
        if (sessionNameEdit) {
            QString roomName = sessionJson.value("room_name").toString();
            int port = sessionJson.value("server_port").toInt();
            QString address = sessionJson.value("server_address").toString("127.0.0.1");
            sessionNameEdit->setText(roomName);
            sessionNameEdit->setReadOnly(true);
        }
        
        // Set game name
        QLineEdit* gameNameEdit = nullptr;
        if (this->gameNameLineEdit) {
            gameNameEdit = this->gameNameLineEdit;
        } else {
            gameNameEdit = this->findChild<QLineEdit*>("gameNameLineEdit");
        }
        
        if (gameNameEdit) {
            QString gameName = sessionJson.value("game_name").toString();
            gameNameEdit->setText(gameName);
            gameNameEdit->setReadOnly(true);
        }
        
        // Set public IP address display
        QLineEdit* publicIpEdit = this->findChild<QLineEdit*>("publicIpLineEdit");
        if (!publicIpEdit) {
            // Create a new line edit if it doesn't exist in the UI
            publicIpEdit = new QLineEdit(this);
            publicIpEdit->setObjectName("publicIpLineEdit");
            publicIpEdit->setReadOnly(true);
            
            // Find the parent layout and insert it after gameNameEdit
            QWidget* parent = gameNameEdit ? gameNameEdit->parentWidget() : nullptr;
            if (!parent) {
                parent = this;
            }
            
            // For now, add it to the main layout
            QVBoxLayout* mainLayout = qobject_cast<QVBoxLayout*>(this->layout());
            if (mainLayout) {
                // Create a horizontal layout for the label and edit
                QHBoxLayout* publicIpLayout = new QHBoxLayout();
                QLabel* publicIpLabel = new QLabel("Public Address:", this);
                publicIpLayout->addWidget(publicIpLabel);
                publicIpLayout->addWidget(publicIpEdit);
                
                // Insert before the chat/players section (find the position)
                for (int i = 0; i < mainLayout->count(); ++i) {
                    QLayoutItem* item = mainLayout->itemAt(i);
                    if (item && item->widget()) {
                        if (item->widget()->objectName() == "chatPlainTextEdit" || 
                            item->widget()->objectName() == "label_5") {
                            mainLayout->insertLayout(i, publicIpLayout);
                            break;
                        }
                    }
                }
            }
        }
        
        if (publicIpEdit) {
            const QString hostCode = sessionJson.value("host_code").toString();

            QLabel* publicIpLabel = this->findChild<QLabel*>("label_3");
            if (!hostCode.isEmpty()) {
                if (publicIpLabel) {
                    publicIpLabel->setText("Host Code");
                }
                publicIpEdit->setText(hostCode);
            } else {
                const QString publicAddress = sessionJson.value("public_address").toString();
                const int publicPort = sessionJson.value("public_port").toInt(Netplay::kDefaultNetplayHostingPort);
                if (publicIpLabel) {
                    publicIpLabel->setText("Connect");
                }
                publicIpEdit->setText(QString("%1:%2").arg(publicAddress, QString::number(publicPort)));
            }
        }
    }

    this->updateCheatsTreeWidget();
    // Populate initial player list from coordinator snapshot to avoid missing
    // early join updates that may have fired before this dialog connected.
    QList<Netplay::SocketIOClient::PlayerInfo> currentPlayers = this->coordinator->getPlayerList();
    if (!currentPlayers.isEmpty())
    {
        QStringList names;
        for (const auto& player : currentPlayers)
        {
            if (!player.name.isEmpty())
                names << player.name;
        }
        if (!names.isEmpty())
            this->on_coordinator_playersUpdated(names);
    }
    else
    {
        QString hostName = sessionJson.value("player_name").toString("Host");
        QStringList initialPlayers;
        initialPlayers << hostName;
        this->on_coordinator_playersUpdated(initialPlayers);
    }

    qDebug() << "Netplay Session created:" << sessionJson.value("player_name").toString("Host")
             << "at" << sessionJson.value("server_address").toString() << ":"
             << sessionJson.value("server_port").toInt();

    this->applyHostOnlyControlsVisibility();
}

NetplaySessionDialog::~NetplaySessionDialog(void)
{
}

bool NetplaySessionDialog::isLocalSessionHost(void) const
{
    QJsonDocument sessionDoc = QJsonDocument::fromJson(this->sessionFile.toUtf8());
    const QJsonObject sessionJson = sessionDoc.object();
    if (sessionJson.value("is_hosting").toBool(false)) {
        return true;
    }

    const int jsonSlot = sessionJson.value("slot").toInt(sessionJson.value("slotIndex").toInt(-1));
    if (jsonSlot >= 0) {
        return jsonSlot == 0;
    }

    if (this->sessionSlot >= 0) {
        return this->sessionSlot == 0;
    }

    if (this->coordinator) {
        if (this->coordinator->isHostingServer()) {
            return true;
        }
        return this->coordinator->isHost();
    }

    return false;
}

void NetplaySessionDialog::setLayoutWidgetsVisible(QLayout* layout, bool visible)
{
    if (!layout) {
        return;
    }

    for (int i = 0; i < layout->count(); ++i) {
        if (QLayoutItem* item = layout->itemAt(i)) {
            if (QWidget* widget = item->widget()) {
                widget->setVisible(visible);
            } else if (QLayout* childLayout = item->layout()) {
                this->setLayoutWidgetsVisible(childLayout, visible);
            }
        }
    }
}

void NetplaySessionDialog::applyHostOnlyControlsVisibility(void)
{
    const bool isHost = this->isLocalSessionHost();

    this->setLayoutWidgetsVisible(this->horizontalLayout_buffer, isHost);
    if (this->bufferLabel) {
        this->bufferLabel->setVisible(isHost);
    }
    if (this->bufferDelaySpinBox) {
        this->bufferDelaySpinBox->setVisible(isHost);
    }
    this->groupBox_3->setVisible(isHost);
    if (!isHost) {
        this->groupBox_3->hide();
        this->verticalLayout_3->setStretch(1, 0);
    } else {
        this->groupBox_3->show();
        this->verticalLayout_3->setStretch(1, 1);
    }

    if (isHost) {
        this->buttonBox->setStandardButtons(QDialogButtonBox::Cancel | QDialogButtonBox::Ok |
                                            QDialogButtonBox::RestoreDefaults);
        if (QPushButton* startButton = this->buttonBox->button(QDialogButtonBox::Ok)) {
            startButton->setText("Start");
        }
        if (QPushButton* cheatsButton = this->buttonBox->button(QDialogButtonBox::RestoreDefaults)) {
            cheatsButton->setText("Cheats");
            cheatsButton->setIcon(QIcon::fromTheme("code-box-line"));
        }
    } else {
        this->buttonBox->setStandardButtons(QDialogButtonBox::Cancel);
    }
}

bool NetplaySessionDialog::getCheats(std::vector<CoreCheat>& cheats, QJsonArray& cheatsArray)
{
    QJsonDocument sessionDoc = QJsonDocument::fromJson(this->sessionFile.toUtf8());
    QJsonObject sessionJson = sessionDoc.object();

    cheatsArray = sessionJson.value("cheats").toArray();
    if (cheatsArray.isEmpty() && this->sessionSlot == 0)
    {
        // Host falls back to locally enabled cheats until the first sync.
        cheatsArray = buildEnabledCheatsSnapshot(this->romFile);
    }

    if (cheatsArray.isEmpty())
    {
        return true;
    }

    if (!CheatsCommon::ParseCheatJson(cheatsArray, cheats))
    {
        QString error = "Failed to parse cheats json";
        QtMessageBox::Error(this, "CheatsCommon::ParseCheatJson() Failed", error);
        return false;
    }

    return true;
}

bool NetplaySessionDialog::setCheats(const QJsonArray& cheatsArray)
{
    QJsonDocument sessionDoc = QJsonDocument::fromJson(this->sessionFile.toUtf8());
    QJsonObject sessionJson = sessionDoc.object();

    if (sessionJson.isEmpty())
    {
        QtMessageBox::Error(this, "Cheats Error", "Failed to update cheats: invalid session data");
        return false;
    }

    sessionJson.insert("cheats", cheatsArray);
    this->sessionFile = QJsonDocument(sessionJson).toJson(QJsonDocument::Compact);
    this->romFile = sessionJson.value("rom_path").toString();
    return true;
}

bool NetplaySessionDialog::applyCheats(void)
{
    std::vector<CoreCheat> cheats;
    QJsonArray cheatsArray;

    if (!this->getCheats(cheats, cheatsArray))
    {
        return false;
    }

    if (!CoreSetNetplayCheats(cheats))
    {
        QtMessageBox::Error(this, "CoreSetNetplayCheats() Failed", QString::fromStdString(CoreGetError()));
        return false;
    }

    return true;
}

void NetplaySessionDialog::updateCheatsTreeWidget(void)
{
    std::vector<CoreCheat> cheats;
    QJsonArray cheatsArray;

    if (!this->getCheats(cheats, cheatsArray))
    {
        // always clear UI on failure
        this->cheatsTreeWidget->clear();
        return;
    }

    CheatsCommon::AddCheatsToTreeWidget(true, cheatsArray, this->romFile, cheats, this->cheatsTreeWidget, true);
}

void NetplaySessionDialog::on_coordinator_playersUpdated(const QStringList& playerNames)
{
    this->listWidget->clear();

    const bool isHost = this->isLocalSessionHost();
    QPushButton* startButton = this->buttonBox->button(QDialogButtonBox::Ok);
    QPushButton* cheatsButton = this->buttonBox->button(QDialogButtonBox::RestoreDefaults);
    if (startButton) {
        startButton->setEnabled(false);
    }
    if (cheatsButton && isHost) {
        cheatsButton->setEnabled(false);
    }
    
    for (int i = 0; i < playerNames.size() && i < 4; i++)
    {
        const QString& name = playerNames.at(i);

        // add read-only item to UI
        QListWidgetItem* item = new QListWidgetItem();
        item->setText(name);
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        this->listWidget->addItem(item);

        // Enable start button only when we're the host (first player) and everyone is ready
        if (i == 0 && !name.isEmpty() && isHost)
        {
            if (startButton) {
                startButton->setEnabled(true);
            }
            if (cheatsButton) {
                cheatsButton->setEnabled(true);
            }
        }
    }

    if (isHost && playerNames.size() > 1)
    {
        this->syncHostSessionState();
    }
}

void NetplaySessionDialog::tryStartPendingGame(void)
{
    if (!this->m_pendingGameStart) {
        return;
    }

    if (!this->isLocalSessionHost() && !this->m_sessionSavesApplied) {
        return;
    }

    this->m_pendingGameStart = false;

    const QString romFile = this->romFile;
    const int selectedSlot = this->m_pendingPlayerSlot;

    if (romFile.isEmpty() || !QFileInfo::exists(romFile)) {
        QtMessageBox::Error(this, "ROM Missing", "The ROM path from this netplay session does not exist locally. Please reselect the ROM.");
        this->m_pendingGameStart = true;
        return;
    }

    this->applyCheats();
    CoreSetEmbeddedNetplayState(true, selectedSlot);
    emit OnPlayGame(romFile, "", 0, selectedSlot);
}

void NetplaySessionDialog::on_coordinator_gameStarted(int playerSlot)
{
    if (this->isLocalSessionHost()) {
        this->syncHostSessionState();
    }

    int selectedSlot = playerSlot;
    if (this->coordinator) {
        const int coordinatorSlot = this->coordinator->getGameSession().localSlot;
        if (coordinatorSlot >= 0) {
            selectedSlot = coordinatorSlot;
        }
    }
    if (selectedSlot < 0 && this->sessionSlot >= 0) {
        selectedSlot = this->sessionSlot;
    }
    if (selectedSlot < 0) {
        selectedSlot = 0;
    } else if (selectedSlot > 3) {
        selectedSlot = 3;
    }

    this->m_pendingGameStart = true;
    this->m_pendingPlayerSlot = selectedSlot;

    if (this->isLocalSessionHost()) {
        this->m_sessionSavesApplied = true;
        this->tryStartPendingGame();
        return;
    }

    if (this->m_sessionSavesApplied) {
        this->tryStartPendingGame();
        return;
    }

    // Saves travel on their own save-sync message (too large for game-started).
    QTimer::singleShot(750, this, [this]() {
        if (!this->m_pendingGameStart) {
            return;
        }
        this->m_sessionSavesApplied = true;
        this->tryStartPendingGame();
    });
}

void NetplaySessionDialog::on_coordinator_cheatsUpdated(const QJsonArray& cheats)
{
    if (!this->setCheats(cheats))
    {
        return;
    }

    this->updateCheatsTreeWidget();

    // Always push synced cheats into core state immediately so frame 0 starts with the right set.
    this->applyCheats();
}

void NetplaySessionDialog::on_coordinator_saveSyncReceived(const QJsonArray& saveFiles)
{
    if (saveFiles.isEmpty()) {
        return;
    }

    const auto saveDirectory = CoreGetSaveDirectory();
    QDir directory(QString::fromStdString(saveDirectory.string()));
    if (!directory.exists())
    {
        directory.mkpath(".");
    }

    QStringList filenamesToReplace;
    for (const auto& value : saveFiles) {
        const QString filename = value.toObject().value("filename").toString();
        if (!filename.isEmpty()) {
            filenamesToReplace << filename;
        }
    }

    for (const QString& filename : filenamesToReplace) {
        QFile::remove(directory.filePath(filename));
    }

    for (const auto& value : saveFiles)
    {
        const QJsonObject saveFile = value.toObject();
        const QString filename = saveFile.value("filename").toString();
        const qint64 expectedSize = saveFile.value("size").toVariant().toLongLong();
        const QString encodedData = saveFile.value("data").toString();
        if (filename.isEmpty() || expectedSize < 0)
        {
            continue;
        }

        const QByteArray decodedData = QByteArray::fromBase64(encodedData.toLatin1());
        if (expectedSize > 0 && decodedData.isEmpty() && !encodedData.isEmpty())
        {
            continue;
        }

        const QString filePath = directory.filePath(filename);
        QFile file(filePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            qWarning() << "NetplaySessionDialog: Failed to write save file" << filePath;
            continue;
        }

        if (expectedSize > 0 && file.write(decodedData) != expectedSize)
        {
            qWarning() << "NetplaySessionDialog: Failed to fully write save file" << filePath;
        }
        file.close();
    }

    this->m_sessionSavesApplied = true;
    this->tryStartPendingGame();
}

void NetplaySessionDialog::syncHostSessionState(void)
{
    if (!this->coordinator || !this->isLocalSessionHost())
    {
        return;
    }

    const QJsonArray hostCheats = buildEnabledCheatsSnapshot(this->romFile);
    if (!hostCheats.isEmpty())
    {
        this->coordinator->sendCheatsUpdate(hostCheats);
    }

    this->coordinator->sendSaveSync(buildSaveSyncFiles(this->romFile));
}

void NetplaySessionDialog::publishHostSessionIndex(bool started)
{
    if (!this->natIndexClient || this->sessionSlot != 0)
    {
        return;
    }

    QJsonDocument sessionDoc = QJsonDocument::fromJson(this->sessionFile.toUtf8());
    QJsonObject sessionJson = sessionDoc.object();
    if (sessionJson.isEmpty())
    {
        return;
    }

    const auto players = this->coordinator ? this->coordinator->getPlayerList() : QList<Netplay::SocketIOClient::PlayerInfo>();
    QJsonArray playersArray;
    for (const auto& player : players)
    {
        QJsonObject playerObj;
        playerObj["playerId"] = player.id;
        playerObj["name"] = player.name;
        playerObj["slotIndex"] = player.slot;
        playerObj["isReady"] = player.isReady;
        playerObj["isSpectator"] = player.isSpectator;
        playersArray.append(playerObj);
    }

    if (playersArray.isEmpty())
    {
        QJsonObject hostPlayer;
        hostPlayer["name"] = sessionJson.value("player_name").toString("Host");
        hostPlayer["slotIndex"] = 0;
        playersArray.append(hostPlayer);
    }

    const int playerCount = playersArray.size();
    const int maxPlayers = 4;

    sessionJson["started"] = started;
    sessionJson["player_count"] = playerCount;
    sessionJson["max_players"] = maxPlayers;
    sessionJson["lobby_size"] = QString("%1/%2").arg(playerCount).arg(maxPlayers);
    sessionJson["players"] = playersArray;
    sessionJson["host_name"] = sessionJson.value("player_name").toString("Host");

    this->sessionJson = sessionJson;
    this->sessionFile = QJsonDocument(sessionJson).toJson(QJsonDocument::Compact);

    const QString hostCode = sessionJson.value("host_code").toString();
    if (hostCode.isEmpty())
    {
        return;
    }

    const QByteArray payload = QJsonDocument(sessionJson).toJson(QJsonDocument::Compact);
    this->natIndexClient->publishSession(hostCode, payload);
}

void NetplaySessionDialog::on_coordinator_chatMessageReceived(const QString& playerName, const QString& message)
{
    QString displayMessage = playerName + ": " + message;
    this->chatPlainTextEdit->appendPlainText(displayMessage);
}

void NetplaySessionDialog::on_coordinator_motdReceived(const QString& message)
{
    QString motdMessage = "<b>MOTD:</b> " + message;
    this->chatPlainTextEdit->appendHtml(motdMessage);
    this->chatPlainTextEdit->setTextInteractionFlags(Qt::TextBrowserInteraction);
}

void NetplaySessionDialog::on_chatLineEdit_textChanged(const QString& text)
{
    this->sendPushButton->setEnabled(!text.startsWith(' ') && !text.trimmed().isEmpty() && text.size() <= 256);
    this->sendPushButton->setDefault(this->sendPushButton->isEnabled());
}

void NetplaySessionDialog::on_sendPushButton_clicked(void)
{
    QString message = this->chatLineEdit->text();
    this->coordinator->sendChatMessage(message);
    this->chatLineEdit->clear();
}

void NetplaySessionDialog::on_buttonBox_clicked(QAbstractButton* button)
{
    QPushButton* pushButton = (QPushButton*)button;
    QPushButton* cheatsButton = this->buttonBox->button(QDialogButtonBox::RestoreDefaults);

    if (pushButton == cheatsButton)
    {
        // Get cheats from coordinator session data
        std::vector<CoreCheat> cheats;
        QJsonArray cheatsArray;
        this->getCheats(cheats, cheatsArray);

        // show cheats dialog to user
        Dialog::CheatsDialog dialog(this, this->romFile, true, cheatsArray);
        if (dialog.exec() == QDialog::Accepted)
        {
            if (!this->setCheats(dialog.GetJson()))
            {
                return;
            }

            this->updateCheatsTreeWidget();
            this->coordinator->sendCheatsUpdate(dialog.GetJson());
        }
    }
}

void NetplaySessionDialog::accept()
{
    if (!this->isLocalSessionHost())
    {
        return;
    }

    const int activePlayers = this->coordinator ? this->coordinator->getPlayerList().size() : 0;
    if (activePlayers < 2)
    {
        QMessageBox messageBox(this);
        messageBox.setIcon(QMessageBox::Warning);
        messageBox.setWindowTitle("Start Game");
        messageBox.setText("There are fewer than 2 players in the lobby.");
        messageBox.setInformativeText("Are you sure you want to start anyway?");
        messageBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        messageBox.setDefaultButton(QMessageBox::No);

        if (messageBox.exec() != QMessageBox::Yes)
        {
            return;
        }
    }

    if (QPushButton* startButton = this->buttonBox->button(QDialogButtonBox::Ok)) {
        startButton->setEnabled(false);
    }
    if (QPushButton* cheatsButton = this->buttonBox->button(QDialogButtonBox::RestoreDefaults)) {
        cheatsButton->setEnabled(false);
    }

    this->syncHostSessionState();

    // Let save-sync reach clients before game-started.
    QTimer::singleShot(200, this, [this]() {
        if (this->coordinator) {
            this->coordinator->startGame();
        }
    });

    // Keep session dialog open while game starts, per netplay UX.
}

void NetplaySessionDialog::reject(void)
{
    CoreSetEmbeddedNetplayState(false, 0);

    // Clean up netplay session when cancelling
    if (this->coordinator)
    {
        // If in game, end it first
        if (this->coordinator->isInGame())
        {
            this->coordinator->endGame();
        }
        
        // Leave the room to disconnect other players
        this->coordinator->leaveRoom();
        
        // If this instance was hosting, stop the server
        if (this->coordinator->isHostingServer())
        {
            this->coordinator->stopHosting();
        }
    }
    
    QDialog::reject();
}

void NetplaySessionDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    this->applyHostOnlyControlsVisibility();
    if (this->isLocalSessionHost()) {
        this->updateCheatsTreeWidget();
    }
}


// Slot: Enable chat input when connected
void NetplaySessionDialog::on_netplay_connected()
{
    this->chatLineEdit->setEnabled(true);
    this->sendPushButton->setEnabled(!this->chatLineEdit->text().isEmpty());
}

// Slot: Disable chat input when disconnected
void NetplaySessionDialog::on_netplay_disconnected()
{
    this->chatLineEdit->setEnabled(false);
    this->sendPushButton->setEnabled(false);
}

void NetplaySessionDialog::on_coordinator_stateChanged(Netplay::NetplayCoordinator::State state)
{
    const bool active = (state == Netplay::NetplayCoordinator::Connected ||
                         state == Netplay::NetplayCoordinator::InLobby ||
                         state == Netplay::NetplayCoordinator::InGame);
    this->chatLineEdit->setEnabled(active);
    this->sendPushButton->setEnabled(active && !this->chatLineEdit->text().isEmpty());
    this->publishHostSessionIndex(state == Netplay::NetplayCoordinator::InGame);
}