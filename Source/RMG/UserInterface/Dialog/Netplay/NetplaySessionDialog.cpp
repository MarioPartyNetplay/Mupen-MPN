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

#include <QJsonDocument>
#include <QPushButton>
#include <QMessageBox>
#include <QShowEvent>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <RMG-Core/Error.hpp>
#include <RMG-Core/Directories.hpp>
#include <RMG-Core/Netplay.hpp>
#include <RMG-Core/Rom.hpp>

using namespace UserInterface::Dialog;
using namespace Utilities;

namespace {

const QStringList& netplaySaveExtensions()
{
    static const QStringList extensions = { ".eep", ".sra", ".srm" };
    return extensions;
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
    const QString romBaseName = QFileInfo(romFile).completeBaseName();

    for (const QString& extension : netplaySaveExtensions())
    {
        const QString filename = romBaseName + extension;
        const QString filePath = directory.filePath(filename);
        QFile file(filePath);
        if (!file.exists() || !file.open(QIODevice::ReadOnly))
        {
            continue;
        }

        const QByteArray data = file.readAll();
        QJsonObject saveFile;
        saveFile["filename"] = filename;
        saveFile["size"] = static_cast<qint64>(data.size());
        saveFile["data"] = QString::fromLatin1(data.toBase64());
        saveFiles.append(saveFile);
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
    this->sessionSlot = sessionJson.value("slot").toInt(-1);

    // Connect coordinator signals
    connect(this->coordinator, &Netplay::NetplayCoordinator::playersUpdated, this,
            [this](const QList<Netplay::SocketIOClient::PlayerInfo>& players) {
        QStringList names;
        for (const auto& player : players) {
            names << player.name;
        }
        this->on_coordinator_playersUpdated(names);
    });
    connect(this->coordinator, &Netplay::NetplayCoordinator::gameStarted, this,
            [this](const Netplay::NetplayCoordinator::GameSession& session) {
        this->on_coordinator_gameStarted(session.localSlot);
    });
    connect(this->coordinator, &Netplay::NetplayCoordinator::chatMessageReceived,
            this, &NetplaySessionDialog::on_coordinator_chatMessageReceived);
    connect(this->coordinator, &Netplay::NetplayCoordinator::motdReceived, 
            this, &NetplaySessionDialog::on_coordinator_motdReceived);
    connect(this->coordinator, &Netplay::NetplayCoordinator::cheatsUpdated,
            this, &NetplaySessionDialog::on_coordinator_cheatsUpdated);
        connect(this->coordinator, &Netplay::NetplayCoordinator::saveSyncReceived,
            this, &NetplaySessionDialog::on_coordinator_saveSyncReceived);

    // Auto-enable pre-toggled cheats for host
    if (this->sessionSlot == 0)
    {
        this->syncHostSessionState();
    }

    // Connect to connection state signals to enable/disable chat
    connect(this->coordinator, &Netplay::NetplayCoordinator::connected, this, &NetplaySessionDialog::on_netplay_connected);
    connect(this->coordinator, &Netplay::NetplayCoordinator::disconnected, this, &NetplaySessionDialog::on_netplay_disconnected);
    connect(this->coordinator, &Netplay::NetplayCoordinator::stateChanged, this, &NetplaySessionDialog::on_coordinator_stateChanged);

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
            QString publicAddress = sessionJson.value("public_address").toString();
            int publicPort = sessionJson.value("public_port").toInt(27886);
            publicIpEdit->setText(QString("%1:%2").arg(publicAddress, QString::number(publicPort)));
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
}

NetplaySessionDialog::~NetplaySessionDialog(void)
{
}

bool NetplaySessionDialog::getCheats(std::vector<CoreCheat>& cheats, QJsonArray& cheatsArray)
{
    QJsonDocument sessionDoc = QJsonDocument::fromJson(this->sessionFile.toUtf8());
    QJsonObject sessionJson = sessionDoc.object();

    // Host uses local enabled cheats as authoritative source
    if (this->sessionSlot == 0)
    {
        cheatsArray = buildEnabledCheatsSnapshot(this->romFile);
    }
    else
    {
        // Clients ONLY use synced session cheats
        cheatsArray = sessionJson.value("cheats").toArray();
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

    const bool isP1 = (this->sessionSlot == 0);
    QPushButton* startButton = this->buttonBox->button(QDialogButtonBox::Ok);
    QPushButton* cheatsButton = this->buttonBox->button(QDialogButtonBox::RestoreDefaults);
    startButton->setEnabled(false);
    cheatsButton->setEnabled(isP1);
    
    for (int i = 0; i < playerNames.size() && i < 4; i++)
    {
        const QString& name = playerNames.at(i);

        // add read-only item to UI
        QListWidgetItem* item = new QListWidgetItem();
        item->setText(name);
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        this->listWidget->addItem(item);

        // Enable start button only when we're the host (first player) and everyone is ready
        if (i == 0 && !name.isEmpty() && isP1)
        {
            startButton->setEnabled(true);
            cheatsButton->setEnabled(true);
        }
    }

    if (isP1 && playerNames.size() > 1)
    {
        this->syncHostSessionState();
    }
}

void NetplaySessionDialog::on_coordinator_gameStarted(int playerSlot)
{
    // Apply cheats before starting game
    this->applyCheats();
    this->syncHostSessionState();

    QJsonDocument sessionDoc = QJsonDocument::fromJson(this->sessionFile.toUtf8());
    QJsonObject sessionJson = sessionDoc.object();

    QString address = sessionJson.value("server_address").toString(this->coordinator->getPeerAddress());
    int port = sessionJson.value("server_port").toInt(this->coordinator->getGamePort());

    // Prefer session-assigned slot when available to avoid duplicate slot claims.
    int selectedSlot = (this->sessionSlot >= 0 ? this->sessionSlot : playerSlot);
    if (selectedSlot < 0) {
        selectedSlot = 0;
    } else if (selectedSlot > 3) {
        selectedSlot = 3;
    }

    // Core netplay uses player index range [1, 4].
    int corePlayer = selectedSlot + 1;
    if (corePlayer < 1) {
        corePlayer = 1;
    } else if (corePlayer > 4) {
        corePlayer = 4;
    }

    // P2P netplay is handled by coordinator/socket lockstep; do not trigger
    // legacy core netplay init (M64CMD_NETPLAY_*), which causes slot assertions.
    Q_UNUSED(address);
    Q_UNUSED(port);
    Q_UNUSED(corePlayer);

    CoreSetEmbeddedNetplayState(true, selectedSlot);
    emit OnPlayGame(this->romFile, "", 0, selectedSlot);
}

void NetplaySessionDialog::on_coordinator_cheatsUpdated(const QJsonArray& cheats)
{
    if (!this->setCheats(cheats))
    {
        return;
    }

    this->updateCheatsTreeWidget();
}

void NetplaySessionDialog::on_coordinator_saveSyncReceived(const QJsonArray& saveFiles)
{
    const auto saveDirectory = CoreGetSaveDirectory();
    QDir directory(QString::fromStdString(saveDirectory.string()));
    if (!directory.exists())
    {
        directory.mkpath(".");
    }

    const QString romBaseName = QFileInfo(this->romFile).completeBaseName();
    for (const QString& extension : netplaySaveExtensions())
    {
        QFile::remove(directory.filePath(romBaseName + extension));
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
}

void NetplaySessionDialog::syncHostSessionState(void)
{
    if (!this->coordinator || this->sessionSlot != 0)
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
    if (this->sessionSlot != 0)
    {
        return;
    }

    QPushButton* startButton = this->buttonBox->button(QDialogButtonBox::Ok);
    QPushButton* cheatsButton = this->buttonBox->button(QDialogButtonBox::RestoreDefaults);
    startButton->setEnabled(false);
    cheatsButton->setEnabled(false);

    this->syncHostSessionState();

    // Signal coordinator to start the game
    this->coordinator->startGame();

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
}