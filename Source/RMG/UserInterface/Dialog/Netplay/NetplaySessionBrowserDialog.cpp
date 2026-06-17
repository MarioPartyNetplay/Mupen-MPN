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
#include "UserInterface/Widget/Netplay/NetplaySessionBrowserWidget.hpp"
#include "Netplay/NatTraversal/NatTraversalProtocol.hpp"
#include "NetplaySessionPasswordDialog.hpp"
#include "Utilities/QtMessageBox.hpp"

#include <QRegularExpressionValidator>
#include <QRegularExpression>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QInputDialog>
#include <QPushButton>
#include <QFileDialog>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>

#include <RMG-Core/Settings.hpp>
#include <RMG-Core/Rom.hpp>

using namespace UserInterface::Dialog;
using namespace Utilities;

//
// Exported Functions
//

NetplaySessionBrowserDialog::NetplaySessionBrowserDialog(QWidget *parent, Netplay::NetplayCoordinator* coordinator, const QMap<QString, CoreRomSettings>& modelData) 
    : QWidget(parent), coordinator(coordinator), romData(modelData), isWaitingForConnection(false)
{
    this->setupUi(this);

    this->ipAddressLineEdit->setPlaceholderText("Host code (7 hex) or IP:Port");

    // Set validator for nickname
    QRegularExpression re("^[a-zA-Z0-9_-]{1,16}$");
    this->nickNameLineEdit->setValidator(new QRegularExpressionValidator(re, this));
    this->nickNameLineEdit->setText(QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Netplay_Nickname)));

    // Connect text change signals
    connect(this->ipAddressLineEdit, &QLineEdit::textChanged, 
            this, &NetplaySessionBrowserDialog::validateJoinButton);
    connect(this->nickNameLineEdit, &QLineEdit::textChanged,
            this, &NetplaySessionBrowserDialog::validateJoinButton);
    connect(this->refreshPushButton, &QPushButton::clicked,
            this, &NetplaySessionBrowserDialog::on_refreshPushButton_clicked);
    connect(this->sessionBrowserWidget, &Widget::NetplaySessionBrowserWidget::OnSessionChanged,
            this, &NetplaySessionBrowserDialog::on_sessionBrowserWidget_OnSessionChanged);

    // Connect coordinator signals for connection status
    connect(this->coordinator, &Netplay::NetplayCoordinator::connected,
            this, &NetplaySessionBrowserDialog::onCoordinatorConnected);
    connect(this->coordinator, &Netplay::NetplayCoordinator::connectionError,
            this, &NetplaySessionBrowserDialog::onCoordinatorConnectionError);
    connect(this->coordinator, &Netplay::NetplayCoordinator::roomJoined,
            this, &NetplaySessionBrowserDialog::onCoordinatorRoomJoined);

    this->networkManager = new QNetworkAccessManager(this);
    connect(this->networkManager, &QNetworkAccessManager::finished,
            this, &NetplaySessionBrowserDialog::onRoomsReplyFinished);

    this->validateJoinButton();
}

void NetplaySessionBrowserDialog::setEmbeddedMode(bool embedded)
{
    this->embeddedMode = embedded;

    if (this->label_4) {
        this->label_4->setVisible(!embedded);
    }
    if (this->nickNameLineEdit) {
        this->nickNameLineEdit->setVisible(!embedded);
    }
}

void NetplaySessionBrowserDialog::setNickname(const QString& nickname)
{
    if (this->nickNameLineEdit == nullptr || this->coordinator == nullptr) {
        return;
    }

    if (this->nickNameLineEdit->text() == nickname) {
        return;
    }

    this->nickNameLineEdit->setText(nickname);
    this->coordinator->setPlayerName(nickname);
}

bool NetplaySessionBrowserDialog::canSubmit(void) const
{
    return this->validate();
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

bool NetplaySessionBrowserDialog::validate(void) const
{
    if (!this->embeddedMode) {
        if (this->nickNameLineEdit->text().isEmpty() ||
            this->nickNameLineEdit->text().contains(' ') ||
            this->nickNameLineEdit->text().size() > 128)
        {
            return false;
        }
    }

    if (this->sessionBrowserWidget->IsCurrentSessionValid()) {
        return true;
    }

    const QString addressInput = this->ipAddressLineEdit->text().trimmed();
    if (addressInput.isEmpty()) {
        return false;
    }

    const int colonIndex = addressInput.lastIndexOf(':');
    const QString addressPart = colonIndex == -1 ? addressInput : addressInput.left(colonIndex);
    if (Netplay::looksLikeTraversalCode(addressPart)) {
        return true;
    }

    return !addressPart.isEmpty();
}

void NetplaySessionBrowserDialog::validateJoinButton(void)
{
    emit this->canSubmitChanged(this->validate());
}

void NetplaySessionBrowserDialog::on_nickNameLineEdit_textChanged(void)
{
    this->validateJoinButton();

    if (!this->nickNameLineEdit->text().isEmpty())
    {
        this->coordinator->setPlayerName(this->nickNameLineEdit->text());
    }
}

void NetplaySessionBrowserDialog::on_refreshPushButton_clicked(void)
{
    this->refreshRoomList();
}

void NetplaySessionBrowserDialog::on_sessionBrowserWidget_OnSessionChanged(bool valid)
{
    Q_UNUSED(valid);
    this->validateJoinButton();
}

void NetplaySessionBrowserDialog::refreshRoomList(void)
{
    this->sessionBrowserWidget->StartRefresh();

    QNetworkRequest request(Netplay::natTraversalRoomsUrl());
    request.setTransferTimeout(10000);
    this->networkManager->get(request);
}

void NetplaySessionBrowserDialog::onRoomsReplyFinished(QNetworkReply* reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError)
    {
        qWarning() << "Failed to fetch netplay room list:" << reply->errorString();
        this->sessionBrowserWidget->RefreshDone();
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject())
    {
        this->sessionBrowserWidget->RefreshDone();
        return;
    }

    const QJsonArray rooms = doc.object().value(QStringLiteral("rooms")).toArray();
    for (const QJsonValue& value : rooms)
    {
        if (!value.isObject()) {
            continue;
        }

        const QJsonObject room = value.toObject();
        this->sessionBrowserWidget->AddSessionData(
            room.value(QStringLiteral("hostName")).toString(),
            room.value(QStringLiteral("gameName")).toString(),
            room.value(QStringLiteral("hostCode")).toString(),
            room.value(QStringLiteral("lobbySize")).toString(),
            room.value(QStringLiteral("port")).toInt(Netplay::kDefaultNetplayHostingPort),
            room.value(QStringLiteral("address")).toString());
    }

    this->sessionBrowserWidget->RefreshDone();
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
    if (isWaitingForConnection || isResolvingHostCode)
    {
        isWaitingForConnection = false;
        isResolvingHostCode = false;
        this->validateJoinButton();
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

        if (!this->pendingHostCode.isEmpty() && !this->pendingIndexSession.isEmpty()) {
            const QString indexGame = this->pendingIndexSession.value("game_name").toString();
            if (!indexGame.isEmpty()) {
                gameName = indexGame;
            }
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

        if (romPath.isEmpty() && !this->pendingIndexSession.isEmpty()) {
            const QString indexedRomPath = this->pendingIndexSession.value("rom_path").toString();
            if (!indexedRomPath.isEmpty() && QFileInfo::exists(indexedRomPath)) {
                romPath = indexedRomPath;
            }
        }

        if (romPath.isEmpty())
        {
            const QString md5 = this->pendingIndexSession.value("md5_hash").toString();
            romPath = this->showROMDialog(gameName, md5);
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
        if (!this->pendingHostCode.isEmpty()) {
            sessionJson.insert("host_code", this->pendingHostCode);
        }
        
        this->sessionJson = sessionJson;
        this->pendingHostCode.clear();
        this->pendingIndexSession = QJsonObject();
        this->sessionFile = QJsonDocument(sessionJson).toJson(QJsonDocument::Compact);
        
        qDebug() << "Session JSON created:" << this->sessionFile;
        
        emit this->sessionAccepted();
    }
    else
    {
        qDebug() << "NetplaySessionBrowserDialog: roomJoined received but not waiting for connection";
    }
}

void NetplaySessionBrowserDialog::connectToResolvedHost(const QString& address, int port)
{
    QString roomId;
    if (!this->pendingIndexSession.isEmpty()) {
        roomId = this->pendingIndexSession.value(QStringLiteral("room_id")).toString();
    }

    qDebug() << "Connecting to" << address << ":" << port << "room:" << roomId;

    this->targetAddress = address;
    this->targetPort = port;
    this->isWaitingForConnection = true;
    this->coordinator->connectToDirectIPServer(address, port, this->nickNameLineEdit->text(), roomId);
}

void NetplaySessionBrowserDialog::beginHostCodeJoin(const QString& hostCode)
{
    this->pendingHostCode = Netplay::normalizeTraversalCode(hostCode).toUpper();
    this->pendingIndexSession = QJsonObject();
    this->pendingIndexReady = false;
    this->pendingLookupReady = false;
    this->pendingLookupFailed = false;
    this->pendingLookupAddress.clear();
    this->pendingLookupPort = Netplay::kDefaultNetplayHostingPort;
    this->isResolvingHostCode = true;

    emit this->canSubmitChanged(false);

    this->natTraversalClient = std::make_unique<Netplay::NatTraversalClient>(this);
    connect(this->natTraversalClient.get(), &Netplay::NatTraversalClient::hostLookupSucceeded,
            this, [this](const QString& address, int resolvedPort) {
        this->pendingLookupAddress = address;
        this->pendingLookupPort = resolvedPort;
        this->pendingLookupReady = true;
        this->tryCompleteHostCodeJoin();
    });
    connect(this->natTraversalClient.get(), &Netplay::NatTraversalClient::hostLookupFailed,
            this, [this](const QString& reason) {
        qDebug() << "TRAV lookup failed:" << reason << "- will try session index endpoint";
        this->pendingLookupFailed = true;
        this->pendingLookupReady = false;
        this->tryCompleteHostCodeJoin();
    });

    this->natIndexClient = std::make_unique<Netplay::NatTraversalIndexClient>(this);
    connect(this->natIndexClient.get(), &Netplay::NatTraversalIndexClient::fetched,
            this, [this](const QString& key, const QByteArray& data) {
        Q_UNUSED(key);
        const QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isObject()) {
            this->pendingIndexSession = doc.object();
            qDebug() << "Loaded session index for" << this->pendingHostCode;
        }
        this->pendingIndexReady = true;
        this->tryCompleteHostCodeJoin();
    });
    connect(this->natIndexClient.get(), &Netplay::NatTraversalIndexClient::fetchFailed,
            this, [this](const QString& reason) {
        qDebug() << "Session index unavailable:" << reason;
        this->pendingIndexReady = true;
        this->tryCompleteHostCodeJoin();
    });

    this->natIndexClient->fetchSession(this->pendingHostCode);
    this->natTraversalClient->lookupHost(this->pendingHostCode);
}

void NetplaySessionBrowserDialog::tryCompleteHostCodeJoin()
{
    if (!this->pendingIndexReady) {
        return;
    }

    if (this->pendingLookupReady) {
        this->isResolvingHostCode = false;
        this->connectToResolvedHost(this->pendingLookupAddress, this->pendingLookupPort);
        return;
    }

    if (!this->pendingLookupFailed) {
        return;
    }

    QString indexAddress;
    int indexPort = 0;
    if (Netplay::sessionConnectEndpoint(this->pendingIndexSession, &indexAddress, &indexPort)) {
        qDebug() << "Joining via session index endpoint" << indexAddress << indexPort;
        this->isResolvingHostCode = false;
        this->connectToResolvedHost(indexAddress, indexPort);
        return;
    }

    this->isResolvingHostCode = false;
    this->pendingHostCode.clear();
    this->validateJoinButton();
    QtMessageBox::Error(this, "Host Lookup Failed",
                        "Host code is not registered and the session index has no connect address.");
}

void NetplaySessionBrowserDialog::submit(void)
{
    NetplaySessionData selectedSession;
    if (this->sessionBrowserWidget->GetCurrentSession(selectedSession))
    {
        this->beginHostCodeJoin(selectedSession.HostCode);
        return;
    }

    QString addressInput = this->ipAddressLineEdit->text().trimmed();
    if (addressInput.isEmpty()) {
        addressInput = "localhost";
    }

    QString addressPart = addressInput;
    int port = Netplay::kDefaultNetplayHostingPort;

    const int colonIndex = addressInput.lastIndexOf(':');
    if (colonIndex != -1) {
        addressPart = addressInput.left(colonIndex);
        const QString portStr = addressInput.mid(colonIndex + 1);
        bool ok = false;
        const int parsedPort = portStr.toInt(&ok);
        if (!ok || parsedPort < 1024 || parsedPort > 65535) {
            QtMessageBox::Error(this, "Invalid Port", "Port must be between 1024 and 65535");
            return;
        }
        port = parsedPort;
    }

    if (Netplay::looksLikeTraversalCode(addressPart)) {
        this->beginHostCodeJoin(addressPart);
        return;
    }

    if (addressPart.isEmpty()) {
        QtMessageBox::Error(this, "Invalid Address", "Please enter a valid host code or IP address");
        return;
    }

    this->connectToResolvedHost(addressPart, port);
}
