/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "NetplaySessionBrowserDialog.hpp"
#include "NetplaySessionPasswordDialog.hpp"
#include "Utilities/QtMessageBox.hpp"

#include <QRegularExpressionValidator>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QInputDialog>
#include <QPushButton>
#include <QFileDialog>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>

#include <RMG-Core/Settings.hpp>
#include <RMG-Core/Rom.hpp>

using namespace UserInterface::Dialog;
using namespace Utilities;

//
// Exported Functions
//

NetplaySessionBrowserDialog::NetplaySessionBrowserDialog(QWidget *parent, Netplay::NetplayCoordinator* coordinator, QMap<QString, CoreRomSettings> modelData) 
    : QDialog(parent), coordinator(coordinator), romData(modelData), isWaitingForConnection(false)
{
    this->setupUi(this);

    // Set validator for nickname
    QRegularExpression re("^[a-zA-Z0-9_-]{1,16}$");
    this->nickNameLineEdit->setValidator(new QRegularExpressionValidator(re, this));
    this->nickNameLineEdit->setText(QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Netplay_Nickname)));

    // Change OK button to "Join"
    QPushButton* joinButton = this->buttonBox->button(QDialogButtonBox::Ok);
    joinButton->setText("Join");

    // Connect text change signals
    connect(this->ipAddressLineEdit, &QLineEdit::textChanged, 
            this, &NetplaySessionBrowserDialog::validateJoinButton);
    connect(this->nickNameLineEdit, &QLineEdit::textChanged,
            this, &NetplaySessionBrowserDialog::validateJoinButton);

    // Connect coordinator signals for connection status
    connect(this->coordinator, &Netplay::NetplayCoordinator::connected,
            this, &NetplaySessionBrowserDialog::onCoordinatorConnected);
    connect(this->coordinator, &Netplay::NetplayCoordinator::connectionError,
            this, &NetplaySessionBrowserDialog::onCoordinatorConnectionError);
    connect(this->coordinator, &Netplay::NetplayCoordinator::roomJoined,
            this, &NetplaySessionBrowserDialog::onCoordinatorRoomJoined);

    this->validateJoinButton();
}

NetplaySessionBrowserDialog::~NetplaySessionBrowserDialog(void)
{
    QString nickname = this->nickNameLineEdit->text();
    if (!nickname.isEmpty())
    {
        CoreSettingsSetValue(SettingsID::Netplay_Nickname, nickname.toStdString());
    }
}

QJsonObject NetplaySessionBrowserDialog::GetSessionJson(void)
{
    return this->sessionJson;
}

QString NetplaySessionBrowserDialog::GetSessionFile(void)
{
    return this->sessionFile;
}

QString NetplaySessionBrowserDialog::showROMDialog(QString name, QString md5)
{
    QString title = "Open " + name;
    QString file = QFileDialog::getOpenFileName(this, title, "", "N64 ROMs (*.n64 *.z64 *.v64 *.zip *.7z)");
    CoreRomSettings romSettings;

    if (!file.isEmpty())
    {
        if (!CoreOpenRom(file.toStdU32String()) ||
            !CoreGetCurrentRomSettings(romSettings))
        {
            CoreCloseRom();
            return "";
        }

        CoreCloseRom();

        if (!md5.isEmpty() && md5.toStdString() != romSettings.MD5)
        {
            QString details = "Expected MD5: " + md5 + "\n";
            details        += "Received MD5: " + QString::fromStdString(romSettings.MD5);
            QtMessageBox::Error(this, "Incorrect ROM Selected", details);
            return "";
        }
    }

    return file;
}

bool NetplaySessionBrowserDialog::validate(void)
{
    if (this->nickNameLineEdit->text().isEmpty() ||
        this->nickNameLineEdit->text().contains(' ') ||
        this->nickNameLineEdit->text().size() > 128)
    {
        return false;
    }

    QString ip = this->ipAddressLineEdit->text().trimmed();
    if (ip.isEmpty())
    {
        return false;
    }

    return true;
}

void NetplaySessionBrowserDialog::validateJoinButton(void)
{
    QPushButton* joinButton = this->buttonBox->button(QDialogButtonBox::Ok);
    joinButton->setEnabled(this->validate());
}

void NetplaySessionBrowserDialog::on_nickNameLineEdit_textChanged(void)
{
    this->validateJoinButton();

    if (!this->nickNameLineEdit->text().isEmpty())
    {
        this->coordinator->setPlayerName(this->nickNameLineEdit->text());
    }
}

void NetplaySessionBrowserDialog::onCoordinatorConnected(void)
{
    if (isWaitingForConnection)
    {
        qDebug() << "Connected to server successfully, waiting for room join...";
    }
}

void NetplaySessionBrowserDialog::onCoordinatorConnectionError(const QString& error)
{
    if (isWaitingForConnection)
    {
        isWaitingForConnection = false;
        QtMessageBox::Error(this, "Connection Failed", "Failed to connect to server: " + error);
    }
}

void NetplaySessionBrowserDialog::onCoordinatorRoomJoined(const QString& roomId, int slot)
{
    qDebug() << "NetplaySessionBrowserDialog::onCoordinatorRoomJoined - roomId:" << roomId << "slot:" << slot << "waiting:" << isWaitingForConnection;
    
    if (isWaitingForConnection)
    {
        isWaitingForConnection = false;
        qDebug() << "Joined room successfully:" << roomId << "slot:" << slot;
        
        // Build session data from the room metadata received during auto-join.
        QJsonObject roomData = this->coordinator->getAutoJoinRoomData();
        QString roomName = roomData.value("roomName").toString();
        if (roomName.isEmpty()) {
            roomName = roomId;
        }

        QString gameName = roomData.value("gameName").toString();
        if (gameName.isEmpty()) {
            gameName = roomData.value("gameId").toString("Unknown");
        }
        QString romPath;
        for (auto it = this->romData.constBegin(); it != this->romData.constEnd(); ++it)
        {
            const QString candidatePath = it.key();
            const QString candidateGoodName = QString::fromStdString(it.value().GoodName);
            const QString candidateFileName = QFileInfo(candidatePath).fileName();

            if (candidateGoodName == gameName || candidateFileName == gameName)
            {
                romPath = candidatePath;
                break;
            }
        }

        if (romPath.isEmpty())
        {
            romPath = this->showROMDialog(gameName, "");
            if (romPath.isEmpty())
            {
                QtMessageBox::Error(this, "ROM Required", "Please select a ROM to join this netplay game.");
                return;
            }
        }

        QString hostName = roomData.value("hostId").toString();
        int maxPlayers = roomData.value("maxPlayers").toInt(4);
        int currentPlayers = roomData.value("playerCount").toInt(1);
        
        QJsonObject sessionJson;
        sessionJson.insert("roomId", roomId);
        sessionJson.insert("slot", slot);
        sessionJson.insert("room_name", roomName);
        sessionJson.insert("player_name", this->nickNameLineEdit->text());
        sessionJson.insert("game_name", gameName);
        sessionJson.insert("gameId", gameName);
        sessionJson.insert("host_name", hostName);
        sessionJson.insert("maxPlayers", maxPlayers);
        sessionJson.insert("currentPlayers", currentPlayers);
        sessionJson.insert("server_address", this->targetAddress);
        sessionJson.insert("server_port", this->targetPort);
        sessionJson.insert("public_address", this->targetAddress);
        sessionJson.insert("public_port", this->targetPort);
        sessionJson.insert("rom_path", romPath);
        
        this->sessionJson = sessionJson;
        this->sessionFile = QJsonDocument(sessionJson).toJson(QJsonDocument::Compact);
        
        qDebug() << "Session JSON created:" << this->sessionFile;
        
        QDialog::accept();
    }
    else
    {
        qDebug() << "NetplaySessionBrowserDialog: roomJoined received but not waiting for connection";
    }
}

void NetplaySessionBrowserDialog::accept(void)
{
    // Direct connection mode - connect to IP:Port
    QString addressInput = this->ipAddressLineEdit->text().trimmed();
    
    if (addressInput.isEmpty())
    {
        addressInput = "localhost";
    }
    
    // Parse IP:Port (format: "192.168.1.100:27886" or "192.168.1.100")
    QString ipAddress = addressInput;
    int port = 27886; // Default port
    
    int colonIndex = addressInput.lastIndexOf(':');
    if (colonIndex != -1)
    {
        ipAddress = addressInput.left(colonIndex);
        QString portStr = addressInput.mid(colonIndex + 1);
        bool ok;
        int parsedPort = portStr.toInt(&ok);
        if (ok && parsedPort >= 1024 && parsedPort <= 65535)
        {
            port = parsedPort;
        }
        else
        {
            QtMessageBox::Error(this, "Invalid Port", "Port must be between 1024 and 65535");
            return;
        }
    }
    
    if (ipAddress.isEmpty())
    {
        QtMessageBox::Error(this, "Invalid IP", "Please enter a valid IP address");
        return;
    }
    
    qDebug() << "Connecting to" << ipAddress << ":" << port;

    // Save target endpoint for the session dialog metadata.
    this->targetAddress = ipAddress;
    this->targetPort = port;
    
    // Mark that we're waiting for connection and room join
    isWaitingForConnection = true;
    
    // Connect to the server at the direct IP address
    // The actual dialog accept will happen when roomJoined() signal is received
    this->coordinator->connectToDirectIPServer(ipAddress, port, this->nickNameLineEdit->text());
}
