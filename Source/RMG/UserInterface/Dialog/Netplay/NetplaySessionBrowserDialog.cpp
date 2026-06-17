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
#include "NetplayCommon.hpp"
#include "UserInterface/Widget/Netplay/NetplaySessionBrowserWidget.hpp"
#include "NetplaySessionPasswordDialog.hpp"
#include "Utilities/QtMessageBox.hpp"

#include <QRegularExpressionValidator>
#include <QRegularExpression>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QPushButton>
#include <QFileDialog>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QSharedPointer>
#include <QDateTime>
#include <QTimer>

#include <RMG-Core/Settings.hpp>
#include <RMG-Core/Rom.hpp>

#include <initializer_list>

namespace Netplay = UserInterface::Netplay;

using namespace UserInterface::Dialog;
using namespace Utilities;

namespace {

QString firstNonEmpty(std::initializer_list<QString> values)
{
    for (const QString& value : values) {
        if (!value.isEmpty()) {
            return value;
        }
    }
    return {};
}

QString hostNameFromPlayers(const QJsonArray& players)
{
    for (const QJsonValue& value : players) {
        const QString name = value.toObject().value(QStringLiteral("name")).toString().trimmed();
        if (!name.isEmpty()) {
            return name;
        }
    }
    return {};
}

struct RoomListingFields
{
    QString hostName;
    QString gameName;
    QString lobbySize;
};

RoomListingFields resolveRoomListingFields(const QJsonObject& room, const QJsonObject& indexSession = {})
{
    const QString hostCode = room.value(QStringLiteral("hostCode")).toString();
    QString hostName = room.value(QStringLiteral("hostName")).toString().trimmed();
    QString gameName = room.value(QStringLiteral("gameName")).toString().trimmed();
    QString lobbySize = room.value(QStringLiteral("lobbySize")).toString().trimmed();

    if (!indexSession.isEmpty()) {
        hostName = firstNonEmpty({
            indexSession.value(QStringLiteral("host_name")).toString().trimmed(),
            indexSession.value(QStringLiteral("player_name")).toString().trimmed(),
            indexSession.value(QStringLiteral("room_name")).toString().trimmed(),
            hostNameFromPlayers(indexSession.value(QStringLiteral("players")).toArray()),
            hostName,
            hostNameFromPlayers(room.value(QStringLiteral("players")).toArray()),
        });

        gameName = firstNonEmpty({
            indexSession.value(QStringLiteral("game_name")).toString().trimmed(),
            gameName,
        });

        if (lobbySize.isEmpty()) {
            const int playerCount = indexSession.value(QStringLiteral("player_count")).toInt(
                room.value(QStringLiteral("playerCount")).toInt(1));
            const int maxPlayers = indexSession.value(QStringLiteral("max_players")).toInt(
                room.value(QStringLiteral("maxPlayers")).toInt(4));
            lobbySize = QStringLiteral("%1/%2").arg(playerCount).arg(maxPlayers > 0 ? maxPlayers : 4);
        }
    } else if (hostName.isEmpty() || Netplay::looksLikeTraversalCode(hostName)) {
        hostName = hostNameFromPlayers(room.value(QStringLiteral("players")).toArray());
    }

    if ((hostName.isEmpty() || Netplay::looksLikeTraversalCode(hostName)) && !hostCode.isEmpty()) {
        hostName = hostCode;
    }

    if (lobbySize.isEmpty()) {
        const int playerCount = room.value(QStringLiteral("playerCount")).toInt(1);
        const int maxPlayers = room.value(QStringLiteral("maxPlayers")).toInt(4);
        lobbySize = QStringLiteral("%1/%2").arg(playerCount).arg(maxPlayers > 0 ? maxPlayers : 4);
    }

    return {hostName, gameName, lobbySize};
}

void addResolvedRoom(UserInterface::Widget::NetplaySessionBrowserWidget* widget,
                     const QJsonObject& room,
                     const QJsonObject& indexSession = {})
{
    if (!indexSession.isEmpty() &&
        indexSession.contains(QStringLiteral("show_in_browser")) &&
        !indexSession.value(QStringLiteral("show_in_browser")).toBool(true)) {
        return;
    }

    const RoomListingFields fields = resolveRoomListingFields(room, indexSession);
    if (fields.gameName.isEmpty()) {
        return;
    }

    QString address = room.value(QStringLiteral("address")).toString();
    int port = room.value(QStringLiteral("port")).toInt(Netplay::kDefaultNetplayHostingPort);
    QString connectAddress;
    int connectPort = 0;
    if (Netplay::sessionConnectEndpoint(indexSession, &connectAddress, &connectPort)) {
        address = connectAddress;
        port = connectPort;
    } else if (Netplay::sessionConnectEndpoint(room, &connectAddress, &connectPort)) {
        address = connectAddress;
        port = connectPort;
    }

    widget->AddSessionData(
        fields.hostName,
        fields.gameName,
        room.value(QStringLiteral("hostCode")).toString(),
        fields.lobbySize,
        port,
        address);
}

} // namespace

NetplaySessionBrowserDialog::NetplaySessionBrowserDialog(QWidget *parent, Netplay::NetplayCoordinator* coordinator, const QMap<QString, CoreRomSettings>& modelData) 
    : QWidget(parent), coordinator(coordinator), romData(modelData)
{
    this->setupUi(this);

    this->ipAddressLineEdit->setPlaceholderText("IP address or IP:Port");

    QRegularExpression re("^[a-zA-Z0-9 _-]{1,16}$");
    this->nickNameLineEdit->setValidator(new QRegularExpressionValidator(re, this));
    this->nickNameLineEdit->setText(QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Netplay_Nickname)));

    connect(this->ipAddressLineEdit, &QLineEdit::textChanged, 
            this, &NetplaySessionBrowserDialog::validateJoinButton);
    connect(this->nickNameLineEdit, &QLineEdit::textChanged,
            this, &NetplaySessionBrowserDialog::validateJoinButton);
    connect(this->refreshPushButton, &QPushButton::clicked,
            this, &NetplaySessionBrowserDialog::on_refreshPushButton_clicked);
    connect(this->sessionBrowserWidget, &Widget::NetplaySessionBrowserWidget::OnSessionChanged,
            this, &NetplaySessionBrowserDialog::on_sessionBrowserWidget_OnSessionChanged);

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
        if (!NetplayCommon::IsValidNickname(this->nickNameLineEdit->text()))
        {
            return false;
        }
    }

    if (this->sessionBrowserWidget->IsCurrentSessionValid()) {
        return true;
    }

    return !this->ipAddressLineEdit->text().trimmed().isEmpty();
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

    QNetworkRequest request(Netplay::netplayRoomsUrl());
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
    if (rooms.isEmpty()) {
        this->sessionBrowserWidget->RefreshDone();
        return;
    }

    QList<QJsonObject> roomObjects;
    roomObjects.reserve(rooms.size());
    for (const QJsonValue& value : rooms) {
        if (value.isObject()) {
            roomObjects.append(value.toObject());
        }
    }

    if (roomObjects.isEmpty()) {
        this->sessionBrowserWidget->RefreshDone();
        return;
    }

    const QSharedPointer<int> remaining = QSharedPointer<int>::create(roomObjects.size());
    const auto finishRoom = [this, remaining]() {
        if (--(*remaining) <= 0) {
            this->sessionBrowserWidget->RefreshDone();
        }
    };

    for (const QJsonObject& room : roomObjects) {
        const QString hostCode = room.value(QStringLiteral("hostCode")).toString();
        const QUrl indexUrl = Netplay::netplaySessionIndexUrl(hostCode);
        if (indexUrl.isEmpty()) {
            addResolvedRoom(this->sessionBrowserWidget, room);
            finishRoom();
            continue;
        }

        QNetworkRequest request(indexUrl);
        request.setTransferTimeout(5000);
        QNetworkReply* indexReply = this->networkManager->get(request);
        connect(indexReply, &QNetworkReply::finished, this, [this, indexReply, room, finishRoom]() {
            QJsonObject indexSession;
            if (indexReply->error() == QNetworkReply::NoError) {
                const QJsonDocument indexDoc = QJsonDocument::fromJson(indexReply->readAll());
                if (indexDoc.isObject()) {
                    indexSession = indexDoc.object();
                }
            }
            indexReply->deleteLater();
            addResolvedRoom(this->sessionBrowserWidget, room, indexSession);
            finishRoom();
        });
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
    if (joinRetryActive && QDateTime::currentMSecsSinceEpoch() < joinDeadlineMs) {
        qDebug() << "Join connect failed, retrying:" << error;
        isWaitingForConnection = true;
        QTimer::singleShot(1000, this, [this]() {
            if (this->joinRetryActive) {
                this->attemptJoinConnect();
            }
        });
        return;
    }

    if (isWaitingForConnection)
    {
        finishJoinFailure(error);
    }
}

void NetplaySessionBrowserDialog::onCoordinatorRoomJoined(const QString& roomId, int slot)
{
    if (!isWaitingForConnection)
    {
        return;
    }

    joinRetryActive = false;
    isWaitingForConnection = false;

    QJsonObject roomData = this->coordinator->getAutoJoinRoomData();
    QString roomName = roomData.value("roomName").toString();
    if (roomName.isEmpty()) {
        roomName = roomId;
    }

    QString gameName = roomData.value("gameName").toString();
    if (gameName.isEmpty()) {
        gameName = roomData.value("gameId").toString("Unknown");
    }

    if (!this->pendingIndexSession.isEmpty()) {
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

    QJsonObject sessionJson;
    sessionJson.insert("roomId", roomId);
    sessionJson.insert("slot", slot);
    sessionJson.insert("room_name", roomName);
    sessionJson.insert("player_name", this->nickNameLineEdit->text());
    sessionJson.insert("game_name", gameName);
    sessionJson.insert("gameId", gameName);
    sessionJson.insert("host_name", roomData.value("hostId").toString());
    sessionJson.insert("maxPlayers", roomData.value("maxPlayers").toInt(4));
    sessionJson.insert("currentPlayers", roomData.value("playerCount").toInt(1));
    sessionJson.insert("server_address", this->targetAddress);
    sessionJson.insert("server_port", this->targetPort);
    sessionJson.insert("public_address", this->targetAddress);
    sessionJson.insert("public_port", this->targetPort);
    sessionJson.insert("rom_path", romPath);

    this->sessionJson = sessionJson;
    this->pendingIndexSession = QJsonObject();
    this->sessionFile = QJsonDocument(sessionJson).toJson(QJsonDocument::Compact);

    emit this->sessionAccepted();
}

void NetplaySessionBrowserDialog::connectToResolvedHost(const QString& address, int port, const QString& roomId)
{
    beginJoinConnect(address, port, roomId);
}

void NetplaySessionBrowserDialog::beginJoinConnect(const QString& address, int port, const QString& roomId)
{
    this->targetAddress = address;
    this->targetPort = port;
    this->joinRoomId = roomId;
    this->joinRetryActive = true;
    this->joinDeadlineMs = QDateTime::currentMSecsSinceEpoch() + 30000;
    this->isWaitingForConnection = true;
    this->attemptJoinConnect();
}

void NetplaySessionBrowserDialog::attemptJoinConnect()
{
    if (!this->joinRetryActive) {
        return;
    }

    if (QDateTime::currentMSecsSinceEpoch() >= this->joinDeadlineMs) {
        finishJoinFailure(QStringLiteral("Timed out while connecting to host"));
        return;
    }

    this->coordinator->connectToDirectIPServer(
        this->targetAddress,
        this->targetPort,
        this->nickNameLineEdit->text(),
        this->joinRoomId);
}

void NetplaySessionBrowserDialog::finishJoinFailure(const QString& error)
{
    this->joinRetryActive = false;
    this->isWaitingForConnection = false;
    this->validateJoinButton();
    QtMessageBox::Error(this, "Connection Failed", "Failed to connect to server: " + error);
}

void NetplaySessionBrowserDialog::submit(void)
{
    NetplaySessionData selectedSession;
    if (this->sessionBrowserWidget->GetCurrentSession(selectedSession))
    {
        if (selectedSession.Address.isEmpty()) {
            QtMessageBox::Error(this, "Invalid Session", "Selected session has no connect address.");
            return;
        }

        const int port = selectedSession.Port > 0
            ? selectedSession.Port
            : Netplay::kDefaultNetplayHostingPort;

        this->pendingIndexSession = QJsonObject();
        const QUrl indexUrl = Netplay::netplaySessionIndexUrl(selectedSession.HostCode);
        if (!selectedSession.HostCode.isEmpty() && !indexUrl.isEmpty()) {
            QNetworkRequest request(indexUrl);
            request.setTransferTimeout(5000);
            emit this->canSubmitChanged(false);
            QNetworkReply* indexReply = this->networkManager->get(request);
            connect(indexReply, &QNetworkReply::finished, this,
                    [this, indexReply, selectedSession, port]() {
                if (indexReply->error() == QNetworkReply::NoError) {
                    const QJsonDocument indexDoc = QJsonDocument::fromJson(indexReply->readAll());
                    if (indexDoc.isObject()) {
                        this->pendingIndexSession = indexDoc.object();
                    }
                }
                indexReply->deleteLater();

                QString roomId = this->pendingIndexSession.value(QStringLiteral("room_id")).toString();
                QString address = selectedSession.Address;
                int connectPort = port;
                if (Netplay::sessionConnectEndpoint(this->pendingIndexSession, &address, &connectPort)) {
                    // prefer indexed direct endpoint when available
                }
                this->connectToResolvedHost(address, connectPort, roomId);
                this->validateJoinButton();
            });
            return;
        }

        this->connectToResolvedHost(selectedSession.Address, port);
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

    if (addressPart.isEmpty()) {
        QtMessageBox::Error(this, "Invalid Address", "Please enter a valid IP address");
        return;
    }

    this->connectToResolvedHost(addressPart, port);
}
