/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "SocketIOClient.hpp"
#include "../NetplayEnet.hpp"
#include "../NetplayProtocol.hpp"

#include <QTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>
#include <QUrl>
#include <QUuid>
#include <QThread>
#include <QDateTime>
#include <algorithm>
#include <enet/enet.h>

using namespace UserInterface::Netplay;

namespace {
constexpr int kCheatsChunkSize = 32;
constexpr int kConnectTimeoutMs = 10000;
constexpr int kServiceIntervalMs = 16;
constexpr int kReconnectIntervalMs = 2000;
constexpr int kReconnectFirstAttemptMs = 500;
constexpr int kReconnectAckTimeoutMs = 15000;
constexpr int kBaseReconnectAttempts = 60;
constexpr int kMaxReconnectAttemptsCap = 120;

QJsonArray sliceJsonArray(const QJsonArray& source, int startIndex, int count)
{
    QJsonArray result;
    const int endIndex = std::min(startIndex + count, static_cast<int>(source.size()));
    for (int index = startIndex; index < endIndex; ++index)
    {
        result.append(source.at(index));
    }
    return result;
}

QJsonObject buildCheatsPayload(const QJsonArray& cheats, const QString& chunkId = QString(), int chunkIndex = -1, int chunkCount = 0)
{
    QJsonObject payload;
    payload["cheats"] = cheats;
    if (chunkCount > 1)
    {
        payload["chunkId"] = chunkId;
        payload["chunkIndex"] = chunkIndex;
        payload["chunkCount"] = chunkCount;
    }
    return payload;
}

bool isGameplaySignalingEvent(const QString& eventName)
{
    return eventName == QLatin1String("controller-input") ||
           eventName == QLatin1String("frame-sync") ||
           eventName == QLatin1String("reconnect-room");
}

int maxReconnectAttemptsForPing(int lastPingMs)
{
    int attempts = kBaseReconnectAttempts;
    if (lastPingMs > 200) {
        attempts += (lastPingMs - 200) / 100;
    }
    return std::min(kMaxReconnectAttemptsCap, attempts);
}
} // namespace

SocketIOClient::SocketIOClient(const QString& serverUrl, QObject* parent)
    : QObject(parent)
    , m_serverUrl(serverUrl)
    , m_connectionState(Disconnected)
{
    m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_persistentId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    m_serviceTimer = new QTimer(this);
    m_serviceTimer->setInterval(kServiceIntervalMs);
    connect(m_serviceTimer, &QTimer::timeout, this, &SocketIOClient::on_serviceTimer);

    m_connectTimer = new QTimer(this);
    m_connectTimer->setSingleShot(true);
    connect(m_connectTimer, &QTimer::timeout, this, &SocketIOClient::on_connectTimeout);

    m_pingTimer = new QTimer(this);
    m_pingTimer->setInterval(10000);
    connect(m_pingTimer, &QTimer::timeout, this, [this]() {
        if (m_serverPeer && m_connectionState == Connected) {
            enet_peer_ping(m_serverPeer);
            m_lastPingMs = static_cast<int>(m_serverPeer->roundTripTime);
            refreshSignalingPeerTimeout(m_serverPeer);
            emit pingUpdated(m_lastPingMs);
        }
    });

    m_punchTimer = new QTimer(this);
    m_punchTimer->setInterval(100);
    connect(m_punchTimer, &QTimer::timeout, this, [this]() {
        if (m_connectionState == Connecting && m_useTraversalPunch && m_enetHost) {
            sendConnectPunchBursts();
        }
    });

    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &SocketIOClient::on_reconnectTimer);
}

SocketIOClient::~SocketIOClient()
{
    disconnect();
}

bool SocketIOClient::parseServerEndpoint(const QString& serverUrl, QHostAddress* addressOut, quint16* portOut) const
{
    QString normalized = serverUrl.trimmed();
    if (normalized.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)) {
        normalized = QStringLiteral("udp://") + normalized.mid(7);
    } else if (normalized.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        normalized = QStringLiteral("udp://") + normalized.mid(8);
    } else if (!normalized.startsWith(QStringLiteral("udp://"), Qt::CaseInsensitive)) {
        normalized = QStringLiteral("udp://") + normalized;
    }

    const QUrl url(normalized);
    if (!url.isValid() || url.host().isEmpty()) {
        return false;
    }

    const int port = url.port(kDefaultNetplayHostingPort);
    if (port < 1 || port > 65535) {
        return false;
    }

    const QString host = url.host().toLower();
    if (host == QStringLiteral("localhost")) {
        addressOut->setAddress(QHostAddress::LocalHost);
    } else {
        addressOut->setAddress(url.host());
    }

    *portOut = static_cast<quint16>(port);
    return true;
}

void SocketIOClient::destroyEnetClient()
{
    if (m_serviceTimer) {
        m_serviceTimer->stop();
    }
    if (m_connectTimer) {
        m_connectTimer->stop();
    }
    if (m_pingTimer) {
        m_pingTimer->stop();
    }
    if (m_punchTimer) {
        m_punchTimer->stop();
    }
    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
    }

    if (m_enetHost) {
        if (m_serverPeer) {
            enet_peer_disconnect(m_serverPeer, 0);
            ENetEvent event;
            while (enet_host_service(m_enetHost, &event, 100) > 0) {
                if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    break;
                }
            }
            m_serverPeer = nullptr;
        }
    clearEnetSideChannel(m_enetHost);
    enet_host_destroy(m_enetHost);
    m_enetHost = nullptr;
    }

    shutdownEnetIfIdle();
}

bool SocketIOClient::setEnetPeerAddress(ENetAddress* addressOut) const
{
    if (!addressOut || m_serverHostname.isEmpty()) {
        return false;
    }

    addressOut->port = m_serverPort;
    const QByteArray hostBytes = m_serverHostname.toUtf8();
    return enet_address_set_host(addressOut, hostBytes.constData()) == 0;
}

void SocketIOClient::sendConnectPunchBursts()
{
    if (!m_enetHost || m_serverAddress.isNull() || m_serverPort == 0) {
        return;
    }

    sendEnetPunchBursts(m_enetHost, m_serverAddress, m_serverPort, 5);
}

void SocketIOClient::connectToServer(const QString& playerName, quint16 bindUdpPort, bool useTraversalPunch)
{
    m_playerName = playerName;
    m_savedBindUdpPort = bindUdpPort;
    m_savedUseTraversalPunch = useTraversalPunch;
    m_intentionalDisconnect = false;
    m_awaitingReconnectAck = false;
    m_reconnectAttempts = 0;
    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
    }

    destroyEnetClient();

    if (!parseServerEndpoint(m_serverUrl, &m_serverAddress, &m_serverPort)) {
        m_connectionState = Error;
        emit connectionError(QStringLiteral("Invalid signaling server address: %1").arg(m_serverUrl));
        return;
    }

    {
        const QUrl url(m_serverUrl.trimmed().startsWith(QStringLiteral("udp://"), Qt::CaseInsensitive)
                           ? m_serverUrl.trimmed()
                           : QStringLiteral("udp://") + m_serverUrl.trimmed());
        const QString host = url.host().toLower();
        m_serverHostname = host == QStringLiteral("localhost") ? QStringLiteral("127.0.0.1") : url.host();
    }

    ensureEnetInitialized();
    m_connectionState = Connecting;

    if (!startTransportConnect(bindUdpPort, useTraversalPunch)) {
        return;
    }

    qInfo() << "Connecting to UDP signaling server" << m_serverHostname << m_serverPort
            << "local UDP port" << (bindUdpPort > 0 ? bindUdpPort : m_enetHost->address.port)
            << "traversal punch" << useTraversalPunch;
}

bool SocketIOClient::startTransportConnect(quint16 bindUdpPort, bool useTraversalPunch)
{
    m_useTraversalPunch = useTraversalPunch;

    if (bindUdpPort > 0) {
        ENetAddress bindAddress;
        bindAddress.host = ENET_HOST_ANY;
        bindAddress.port = bindUdpPort;
        for (int attempt = 0; attempt < 5 && !m_enetHost; ++attempt) {
            m_enetHost = createSignalingEnetHost(&bindAddress, 1, 1, 0, 0);
            if (!m_enetHost && attempt < 4) {
                QThread::msleep(50);
            }
        }
        if (!m_enetHost) {
            destroyEnetClient();
            m_connectionState = Error;
            shutdownEnetIfIdle();
            emit connectionError(QStringLiteral(
                "Failed to bind traversal UDP port %1 after NAT punch — retry join").arg(bindUdpPort));
            return false;
        }
    }

    if (!m_enetHost) {
        m_enetHost = createSignalingEnetHost(nullptr, 1, 1, 0, 0);
    }
    if (!m_enetHost) {
        m_connectionState = Error;
        shutdownEnetIfIdle();
        emit connectionError(QStringLiteral("Failed to create UDP signaling client"));
        return false;
    }

    ENetAddress address;
    if (!setEnetPeerAddress(&address)) {
        destroyEnetClient();
        m_connectionState = Error;
        emit connectionError(QStringLiteral("Failed to resolve signaling server address: %1")
                                 .arg(m_serverAddress.toString()));
        return false;
    }

    m_serverPeer = enet_host_connect(m_enetHost, &address, 1, 0);
    if (!m_serverPeer) {
        destroyEnetClient();
        m_connectionState = Error;
        emit connectionError(QStringLiteral("Failed to initiate UDP signaling connection"));
        return false;
    }

    applySignalingPeerTimeout(m_serverPeer);

    if (m_useTraversalPunch) {
        sendConnectPunchBursts();
    }

    m_serviceTimer->start();
    m_connectTimer->start(m_useTraversalPunch ? 25000 : kConnectTimeoutMs);
    if (m_useTraversalPunch) {
        m_punchTimer->start();
    }

    return true;
}

void SocketIOClient::disconnect()
{
    m_intentionalDisconnect = true;
    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
    }
    destroyEnetClient();
    if (m_connectionState != Disconnected) {
        m_connectionState = Disconnected;
        m_lastSentFrameSync = 0;
        m_awaitingReconnectAck = false;
        emit disconnected();
    }
    m_roomId.clear();
    m_playerId.clear();
    m_reconnectToken.clear();
}

void SocketIOClient::beginReconnect()
{
    m_serverPeer = nullptr;
    if (m_pingTimer) {
        m_pingTimer->stop();
    }
    if (m_connectTimer) {
        m_connectTimer->stop();
    }
    if (m_punchTimer) {
        m_punchTimer->stop();
    }

    m_connectionState = Reconnecting;
    m_reconnectAttempts = 0;
    m_awaitingReconnectAck = false;
    m_reconnectAckSentAtMs = 0;
    qWarning() << "SocketIOClient: Signaling connection lost, attempting reconnect to room" << m_roomId;
    emit reconnecting();
    m_reconnectTimer->start(kReconnectFirstAttemptMs);
}

void SocketIOClient::failReconnect()
{
    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
    }
    destroyEnetClient();
    m_connectionState = Disconnected;
    m_awaitingReconnectAck = false;
    qWarning() << "SocketIOClient: Reconnect attempts exhausted";
    emit disconnected();
}

void SocketIOClient::sendReconnectRoom()
{
    if (m_roomId.isEmpty() || m_reconnectToken.isEmpty()) {
        failReconnect();
        return;
    }

    m_awaitingReconnectAck = true;
    m_reconnectAckSentAtMs = QDateTime::currentMSecsSinceEpoch();

    QJsonObject extra;
    extra["reconnectToken"] = m_reconnectToken;
    extra["persistentId"] = m_persistentId;
    extra["player_name"] = m_playerName;
    extra["roomId"] = m_roomId;

    QJsonObject payload;
    payload["roomId"] = m_roomId;
    payload["extra"] = extra;
    emitEvent("reconnect-room", payload);
}

void SocketIOClient::on_reconnectTimer()
{
    if (m_connectionState != Reconnecting) {
        return;
    }

    if (++m_reconnectAttempts > maxReconnectAttemptsForPing(m_lastPingMs)) {
        failReconnect();
        return;
    }

    destroyEnetClient();
    ensureEnetInitialized();
    m_connectionState = Reconnecting;

    if (!startTransportConnect(m_savedBindUdpPort, m_savedUseTraversalPunch)) {
        m_reconnectTimer->start(kReconnectIntervalMs);
    }
}

SocketIOClient::ConnectionState SocketIOClient::getConnectionState() const
{
    return m_connectionState;
}

void SocketIOClient::openRoom(const QString& roomName, const QString& gameId, int maxPlayers)
{
    if (m_connectionState != Connected) {
        qWarning() << "Cannot open room: not connected";
        return;
    }

    QJsonObject payload;
    payload["extra"] = buildOpenRoomPayload(roomName, gameId, maxPlayers);
    payload["maxPlayers"] = maxPlayers;

    emitEvent("open-room", payload);
}

void SocketIOClient::joinRoom(const QString& roomId, bool asSpectator)
{
    if (m_connectionState != Connected) {
        qWarning() << "Cannot join room: not connected";
        return;
    }

    m_roomId = roomId;
    QJsonObject payload = buildJoinRoomPayload(roomId, asSpectator);
    emitEvent("join-room", payload);
}

void SocketIOClient::leaveRoom()
{
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject payload;
    emitEvent("leave-room", payload);
    m_roomId.clear();
}

void SocketIOClient::setPlayerName(const QString& name)
{
    m_playerName = name;
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject payload;
    payload["name"] = name;
    emitEvent("set-name", payload);
}

void SocketIOClient::claimSlot(int slot)
{
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject payload;
    payload["slot"] = slot;
    emitEvent("claim-slot", payload);
}

void SocketIOClient::startGame(const QString& mode, bool resyncEnabled, const QString& romHash)
{
    if (m_connectionState != Connected) {
        return;
    }

    m_gameConfig.mode = mode;
    m_gameConfig.resyncEnabled = resyncEnabled;
    m_gameConfig.romHash = romHash;

    QJsonObject payload;
    payload["mode"] = mode;
    payload["resyncEnabled"] = resyncEnabled;
    payload["romHash"] = romHash;
    emitEvent("start-game", payload);
}

void SocketIOClient::endGame()
{
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject payload;
    emitEvent("end-game", payload);
}

void SocketIOClient::setGameMode(const QString& mode)
{
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject payload;
    payload["mode"] = mode;
    emitEvent("set-mode", payload);
}

void SocketIOClient::sendControllerInput(uint32_t frameNumber, uint32_t controllerState)
{
    if (m_connectionState != Connected && m_connectionState != Reconnecting) {
        return;
    }

    QJsonObject payload;
    payload["frame"] = static_cast<qint64>(frameNumber);
    payload["input"] = static_cast<qint64>(controllerState);
    emitEvent("controller-input", payload);
}

void SocketIOClient::sendFrameSync(uint32_t frameNumber, uint32_t stateHash)
{
    if ((m_connectionState != Connected && m_connectionState != Reconnecting) ||
        frameNumber == m_lastSentFrameSync || stateHash == 0) {
        return;
    }

    m_lastSentFrameSync = frameNumber;

    QJsonObject payload;
    payload["frame"] = static_cast<qint64>(frameNumber);
    payload["hash"] = static_cast<qint64>(stateHash);
    emitEvent("frame-sync", payload);
}

void SocketIOClient::sendOffer(const QString& targetPlayerId, const QString& sdpOffer)
{
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject signal;
    signal["target"] = targetPlayerId;
    signal["offer"] = sdpOffer;
    emitEvent("webrtc-signal", signal);
}

void SocketIOClient::sendAnswer(const QString& targetPlayerId, const QString& sdpAnswer)
{
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject signal;
    signal["target"] = targetPlayerId;
    signal["answer"] = sdpAnswer;
    emitEvent("webrtc-signal", signal);
}

void SocketIOClient::sendICECandidate(const QString& targetPlayerId, const QString& candidate, int sdpMLineIndex)
{
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject signal;
    signal["target"] = targetPlayerId;
    signal["candidate"] = candidate;
    signal["sdpMLineIndex"] = sdpMLineIndex;
    emitEvent("webrtc-signal", signal);
}

void SocketIOClient::setROMSharingEnabled(bool enabled)
{
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject payload;
    payload["enabled"] = enabled;
    emitEvent("rom-sharing-toggle", payload);
}

void SocketIOClient::declareROMReady(bool ready)
{
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject payload;
    payload["ready"] = ready;
    emitEvent("rom-ready", payload);
}

void SocketIOClient::declareROMInfo(const QString& fileName, const QString& hash, uint32_t fileSize)
{
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject payload;
    payload["fileName"] = fileName;
    payload["hash"] = hash;
    payload["fileSize"] = (int)fileSize;
    emitEvent("rom-declare", payload);
}

void SocketIOClient::sendSyncLog(const QString& matchId, const QJsonArray& entries, const QJsonObject& summary)
{
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject payload;
    payload["matchId"] = matchId;
    payload["entries"] = entries;
    payload["summary"] = summary;
    payload["context"] = "desktop-client";
    emitEvent("session-log", payload);
}

void SocketIOClient::sendDebugLog(const QString& matchId, const QString& logContent)
{
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject payload;
    payload["matchId"] = matchId;
    payload["content"] = logContent;
    emitEvent("debug-logs", payload);
}

void SocketIOClient::sendChatMessage(const QString& message)
{
    if (m_connectionState != Connected) {
        qWarning() << "Cannot send chat: not connected to server";
        return;
    }

    QJsonObject payload;
    payload["message"] = message;
    emitEvent("chat-message", payload);
}

void SocketIOClient::sendCheatsUpdate(const QJsonArray& cheats)
{
    if (m_connectionState != Connected) {
        qWarning() << "Cannot send cheats update: not connected to server";
        return;
    }

    const int chunkCount = (cheats.size() + kCheatsChunkSize - 1) / kCheatsChunkSize;
    if (chunkCount <= 1) {
        emitEvent("cheats-update", buildCheatsPayload(cheats));
        return;
    }

    const QString chunkId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    for (int chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
        const QJsonArray chunk = sliceJsonArray(cheats, chunkIndex * kCheatsChunkSize, kCheatsChunkSize);
        emitEvent("cheats-update", buildCheatsPayload(chunk, chunkId, chunkIndex, chunkCount));
    }
}

void SocketIOClient::sendSaveSync(const QJsonArray& saveFiles)
{
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject payload;
    payload["files"] = saveFiles;
    emitEvent("save-sync", payload);
}

void SocketIOClient::sendCoreSettingsSync(const QJsonObject& coreSettings)
{
    if (m_connectionState != Connected || coreSettings.isEmpty()) {
        return;
    }

    emitEvent("core-settings-sync", coreSettings);
}

void SocketIOClient::sendEmulationPauseUpdate(bool paused)
{
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject payload;
    payload["paused"] = paused;
    emitEvent("emulation-paused", payload);
}

void SocketIOClient::sendEmulationReady()
{
    if (m_connectionState != Connected) {
        return;
    }

    emitEvent("emulation-ready", QJsonObject());
}

void SocketIOClient::requestRoomList(bool waiting)
{
    if (m_connectionState != Connected) {
        qWarning() << "Cannot request room list: not connected to server";
        return;
    }

    qDebug() << "SocketIOClient: Requesting room list from server";
    QJsonObject payload;
    payload["waiting"] = waiting;
    emitEvent("list-rooms", payload);
}

QString SocketIOClient::getPlayerId() const
{
    return m_playerId;
}

QString SocketIOClient::getRoomId() const
{
    return m_roomId;
}

SocketIOClient::RoomInfo SocketIOClient::getCurrentRoom() const
{
    return m_currentRoom;
}

QList<SocketIOClient::PlayerInfo> SocketIOClient::getPlayerList() const
{
    return m_currentRoom.players;
}

SocketIOClient::GameConfig SocketIOClient::getGameConfig() const
{
    return m_gameConfig;
}

int SocketIOClient::getLastPingMs() const
{
    return m_lastPingMs;
}

//
// Private Slots
//

void SocketIOClient::on_serviceTimer()
{
    if (!m_enetHost) {
        return;
    }

    if (m_awaitingReconnectAck && m_reconnectAckSentAtMs > 0) {
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_reconnectAckSentAtMs;
        if (elapsed >= kReconnectAckTimeoutMs) {
            qWarning() << "SocketIOClient: Reconnect room ack timed out, retrying";
            m_awaitingReconnectAck = false;
            m_reconnectAckSentAtMs = 0;
            if (m_connectionState == Connected && !m_roomId.isEmpty() && !m_reconnectToken.isEmpty()) {
                sendReconnectRoom();
            } else if (m_connectionState == Reconnecting) {
                m_reconnectTimer->start(kReconnectIntervalMs);
            }
        }
    }

    ENetEvent event;
    while (enet_host_service(m_enetHost, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            if (m_connectTimer) {
                m_connectTimer->stop();
            }
            if (m_punchTimer) {
                m_punchTimer->stop();
            }
            {
                const bool resumeSession = (m_connectionState == Reconnecting);
                m_connectionState = Connected;
                qInfo() << "UDP signaling connected";
                if (m_pingTimer) {
                    m_pingTimer->start();
                }
                if (resumeSession && !m_roomId.isEmpty() && !m_reconnectToken.isEmpty()) {
                    sendReconnectRoom();
                    break;
                }
                emit connected();
            }
            break;

        case ENET_EVENT_TYPE_RECEIVE:
            handleSignalingPacket(QByteArray(reinterpret_cast<const char*>(event.packet->data),
                                             static_cast<int>(event.packet->dataLength)));
            enet_packet_destroy(event.packet);
            break;

        case ENET_EVENT_TYPE_DISCONNECT:
            if (m_connectionState == Connecting) {
                destroyEnetClient();
                m_connectionState = Error;
                emit connectionError(QStringLiteral("UDP signaling connection refused or lost during handshake"));
                break;
            }
            if (m_connectionState == Connected) {
                if (!m_intentionalDisconnect && !m_roomId.isEmpty() && !m_reconnectToken.isEmpty()) {
                    beginReconnect();
                    break;
                }
                m_connectionState = Disconnected;
                m_lastSentFrameSync = 0;
                if (m_pingTimer) {
                    m_pingTimer->stop();
                }
                emit disconnected();
            } else if (m_connectionState == Reconnecting) {
                m_serverPeer = nullptr;
                m_reconnectTimer->start(kReconnectIntervalMs);
            }
            m_serverPeer = nullptr;
            break;

        default:
            if (event.type == static_cast<ENetEventType>(kEnetSkippableEvent)) {
                break;
            }
            break;
        }
    }
}

void SocketIOClient::on_connectTimeout()
{
    if (m_connectionState == Connecting) {
        destroyEnetClient();
        m_connectionState = Error;
        emit connectionError(QStringLiteral("UDP signaling connection timed out"));
        return;
    }

    if (m_connectionState == Reconnecting) {
        m_serverPeer = nullptr;
        m_reconnectTimer->start(kReconnectIntervalMs);
    }
}

void SocketIOClient::handleSignalingPacket(const QByteArray& payload)
{
    QString eventName;
    QJsonArray args;
    if (!parseSignalingPacket(payload, &eventName, &args)) {
        qWarning() << "SocketIOClient: Ignoring malformed signaling packet";
        return;
    }

    handleEvent(eventName, args);
}

void SocketIOClient::handleEvent(const QString& eventName, const QJsonArray& args)
{
    if (eventName == "room-created" && args.size() > 0) {
        QJsonObject data = args[0].toObject();
        QString roomId = data["roomId"].toString();
        m_roomId = roomId;
        m_currentRoom.roomId = roomId;
        const QString token = data["reconnectToken"].toString();
        if (!token.isEmpty()) {
            m_reconnectToken = token;
        }
        qDebug() << "Room created:" << roomId;
        emit roomCreated(roomId);

    } else if (eventName == "room-joined" && args.size() > 0) {
        QJsonObject data = args[0].toObject();
        QString roomId = data["roomId"].toString();
        QString playerId = data["playerId"].toString();
        int slotIndex = data["slotIndex"].toInt(-1);
        m_roomId = roomId;
        if (!playerId.isEmpty()) {
            m_playerId = playerId;
        }
        m_currentRoom.roomId = roomId;
        m_currentRoom.localSlot = slotIndex;
        const QString token = data["reconnectToken"].toString();
        if (!token.isEmpty()) {
            m_reconnectToken = token;
        }
        qDebug() << "Room joined:" << roomId << "slot:" << slotIndex;
        if (m_awaitingReconnectAck) {
            m_awaitingReconnectAck = false;
            m_reconnectAckSentAtMs = 0;
            m_reconnectAttempts = 0;
            if (m_reconnectTimer) {
                m_reconnectTimer->stop();
            }
            qInfo() << "SocketIOClient: Session restored after reconnect";
            emit reconnected();
        }
        emit roomJoined(roomId, slotIndex);

    } else if (eventName == "users-updated" && args.size() > 0) {
        QJsonObject data = args[0].toObject();
        
        // Update player list
        QJsonArray playersArr = data["players"].toArray();
        QList<PlayerInfo> players;
        for (const auto& pVal : playersArr) {
            QJsonObject pObj = pVal.toObject();
            PlayerInfo p;
            p.id = pObj["playerId"].toString();
            if (p.id.isEmpty()) {
                p.id = pObj["id"].toString();
            }
            p.name = pObj["name"].toString();
            p.slot = pObj["slotIndex"].toInt(pObj["slot"].toInt(-1));
            p.isSpectator = pObj["isSpectator"].toBool();
            p.isReady = pObj["isReady"].toBool();
            players.append(p);
        }
        m_currentRoom.players = players;
        m_currentRoom.currentPlayers = data["playerCount"].toInt(players.size());
        
        int spectatorCount = data["spectators"].toArray().size();
        m_currentRoom.spectatorCount = spectatorCount;

        emit playersUpdated(players);
        emit spectatorCountUpdated(spectatorCount);

    } else if (eventName == "room-closed" && args.size() > 0) {
        QString reason = args[0].toObject()["reason"].toString();
        m_roomId.clear();
        emit roomClosed(reason);

    } else if (eventName == "game-started" && args.size() > 0) {
        QJsonObject data = args[0].toObject();
        m_gameConfig.mode = data["mode"].toString();
        m_gameConfig.resyncEnabled = data["resyncEnabled"].toBool();
        m_gameConfig.romHash = data["romHash"].toString();
        QString matchId = data["matchId"].toString();

        if (data.contains("cheats")) {
            emit cheatsUpdated(data["cheats"].toArray());
        }

        emit gameStarted(m_gameConfig.mode, m_gameConfig.resyncEnabled, matchId);

    } else if (eventName == "game-ended") {
        emit gameEnded();

    } else if (eventName == "controller-input" && args.size() > 0) {
        QJsonObject data = args[0].toObject();
        int slot = data["slot"].toInt(-1);
        uint32_t frameNumber = static_cast<uint32_t>(data["frame"].toInteger());
        uint32_t controllerState = static_cast<uint32_t>(data["input"].toInteger());
        emit controllerInputReceived(slot, frameNumber, controllerState);

    } else if (eventName == "frame-sync" && args.size() > 0) {
        QJsonObject data = args[0].toObject();
        int slot = data["slot"].toInt(-1);
        uint32_t frameNumber = static_cast<uint32_t>(data["frame"].toInteger());
        uint32_t stateHash = static_cast<uint32_t>(data["hash"].toInteger());
        emit frameSyncReceived(slot, frameNumber, stateHash);

    } else if (eventName == "webrtc-signal" && args.size() > 0) {
        QJsonObject signal = args[0].toObject();
        QString fromPlayerId = signal["from"].toString();
        
        if (signal.contains("offer")) {
            QString offer = signal["offer"].toString();
            emit offerReceived(fromPlayerId, offer);
        } else if (signal.contains("answer")) {
            QString answer = signal["answer"].toString();
            emit answerReceived(fromPlayerId, answer);
        } else if (signal.contains("candidate")) {
            QString candidate = signal["candidate"].toString();
            int mLineIndex = signal["sdpMLineIndex"].toInt(0);
            emit iceCandidateReceived(fromPlayerId, candidate, mLineIndex);
        }

    } else if (eventName == "rom-ready" && args.size() > 0) {
        QString playerId = args[0].toObject()["playerId"].toString();
        emit playerROMReady(playerId);

    } else if (eventName == "upload-token" && args.size() > 0) {
        QString token = args[0].toObject()["token"].toString();
        emit uploadTokenReceived(token);

    } else if (eventName == "reconnect-token" && args.size() > 0) {
        m_reconnectToken = args[0].toObject()["token"].toString();
        emit reconnectTokenReceived(m_reconnectToken);

    } else if (eventName == "reconnect-failed" && args.size() > 0) {
        qWarning() << "SocketIOClient: Reconnect rejected:" << args[0].toObject()["error"].toString();
        failReconnect();

    } else if (eventName == "chat-message" && args.size() > 0) {
        QJsonObject data = args[0].toObject();
        QString playerName = data["playerName"].toString();
        QString message = data["message"].toString();
        emit chatMessageReceived(playerName, message);

    } else if (eventName == "cheats-updated" && args.size() > 0) {
        QJsonArray cheats;
        QString chunkId;
        int chunkIndex = -1;
        int chunkCount = 0;
        if (args[0].isObject()) {
            const QJsonObject data = args[0].toObject();
            cheats = data["cheats"].toArray();
            chunkId = data["chunkId"].toString();
            chunkIndex = data["chunkIndex"].toInt(-1);
            chunkCount = data["chunkCount"].toInt(0);
        } else if (args[0].isArray()) {
            // Backward compatibility for older servers that emitted the raw array directly.
            cheats = args[0].toArray();
        }

        if (chunkCount > 1 && !chunkId.isEmpty() && chunkIndex >= 0) {
            ChunkedCheatUpdate& update = m_pendingCheatUpdates[chunkId];
            update.chunkCount = chunkCount;
            update.chunks[chunkIndex] = cheats;

            if (update.chunkCount > 0 && update.chunks.size() >= update.chunkCount) {
                QJsonArray combinedCheats;
                for (auto chunkIt = update.chunks.constBegin(); chunkIt != update.chunks.constEnd(); ++chunkIt) {
                    const QJsonArray& chunk = chunkIt.value();
                    for (const auto& cheatValue : chunk) {
                        combinedCheats.append(cheatValue);
                    }
                }

                m_pendingCheatUpdates.remove(chunkId);
                emit cheatsUpdated(combinedCheats);
            }
        } else {
            emit cheatsUpdated(cheats);
        }

    } else if (eventName == "save-sync" && args.size() > 0) {
        QJsonObject data = args[0].toObject();
        emit saveSyncReceived(data["files"].toArray());

    } else if (eventName == "core-settings-sync" && args.size() > 0) {
        emit coreSettingsSyncReceived(args[0].toObject());

    } else if (eventName == "update-input-delay" && args.size() > 0) {
        QJsonObject data = args[0].toObject();
        emit inputDelayReceived(data["frames"].toInt(4));

    } else if (eventName == "emulation-paused" && args.size() > 0) {
        QJsonObject data = args[0].toObject();
        emit emulationPauseReceived(data["paused"].toBool(false));

    } else if (eventName == "emulation-begin") {
        emit emulationBeginReceived();

    } else if (eventName == "player-pings" && args.size() > 0) {
        const QJsonArray pings = args[0].toObject().value(QStringLiteral("pings")).toArray();
        emit playerPingsReceived(pings);

    } else if (eventName == "rooms-list" && args.size() > 0) {
        QJsonObject data = args[0].toObject();
        QJsonArray rooms = data["rooms"].toArray();
        qDebug() << "SocketIOClient: Received room list with" << rooms.size() << "rooms";
        if (!rooms.isEmpty()) {
            QJsonObject room = rooms[0].toObject();
            m_currentRoom.roomId = room["roomId"].toString();
            m_currentRoom.roomName = room["roomName"].toString();
            m_currentRoom.ownerName = room["hostId"].toString();
            m_currentRoom.gameId = room["gameId"].toString("netplay");
            m_currentRoom.maxPlayers = room["maxPlayers"].toInt(4);
            m_currentRoom.currentPlayers = room["playerCount"].toInt(0);
        }
        emit roomsListed(rooms);
    }
}

void SocketIOClient::emitEvent(const QString& eventName, const QJsonObject& payload)
{
    if (eventName != QLatin1String("controller-input") &&
        eventName != QLatin1String("frame-sync")) {
        qDebug() << "SocketIOClient: Emitting event" << eventName;
    }

    const bool canEmit =
        m_serverPeer &&
        (m_connectionState == Connected ||
         (m_connectionState == Reconnecting && isGameplaySignalingEvent(eventName)));

    if (!canEmit) {
        if (!isGameplaySignalingEvent(eventName)) {
            qWarning() << "SocketIOClient: Cannot emit event, not connected";
        }
        return;
    }

    if (!sendSignalingEvent(m_serverPeer, eventName, payload)) {
        qWarning() << "SocketIOClient: Failed to send event" << eventName;
    }
}

void SocketIOClient::emitEvent(const QString& eventName, const QJsonArray& payload)
{
    if (m_connectionState != Connected || !m_serverPeer) {
        return;
    }

    sendSignalingEvent(m_serverPeer, eventName, payload);
}

QJsonObject SocketIOClient::buildOpenRoomPayload(const QString& roomName, const QString& gameId, int maxPlayers)
{
    QJsonObject extra;
    extra["sessionid"] = m_sessionId;
    extra["persistentId"] = m_persistentId;
    extra["reconnectToken"] = m_reconnectToken;
    extra["player_name"] = m_playerName;
    extra["room_name"] = roomName;
    extra["game_id"] = gameId;

    QJsonObject payload;
    payload["extra"] = extra;
    payload["maxPlayers"] = maxPlayers;
    return payload;
}

QJsonObject SocketIOClient::buildJoinRoomPayload(const QString& roomId, bool spectate)
{
    QJsonObject extra;
    extra["sessionid"] = m_sessionId;
    extra["persistentId"] = m_persistentId;
    extra["reconnectToken"] = m_reconnectToken;
    extra["player_name"] = m_playerName;
    extra["spectate"] = spectate;
    extra["roomId"] = roomId;

    QJsonObject payload;
    payload["roomId"] = roomId;
    payload["spectate"] = spectate;
    payload["extra"] = extra;
    return payload;
}

void SocketIOClient::updateRoomState(const QJsonObject& roomData)
{
    m_currentRoom.roomId = roomData["roomId"].toString();
    m_currentRoom.roomName = roomData["roomName"].toString();
    m_currentRoom.ownerName = roomData["ownerName"].toString();
    m_currentRoom.maxPlayers = roomData["maxPlayers"].toInt();
    m_currentRoom.currentPlayers = roomData["currentPlayers"].toInt();
    m_currentRoom.gameId = roomData["gameId"].toString();
}

void SocketIOClient::updatePlayerList(const QJsonArray& players)
{
    QList<PlayerInfo> playerList;
    for (const auto& playerVal : players) {
        QJsonObject playerObj = playerVal.toObject();
        PlayerInfo p;
        p.id = playerObj["id"].toString();
        p.name = playerObj["name"].toString();
        p.slot = playerObj["slot"].toInt();
        p.isSpectator = playerObj["isSpectator"].toBool();
        p.isReady = playerObj["isReady"].toBool();
        playerList.append(p);
    }
    m_currentRoom.players = playerList;
}

void SocketIOClient::sendInputDelayUpdate(int frames) {
    QJsonObject data;
    data["frames"] = frames;
    this->emitEvent("update-input-delay", data); // It's okay to call it from here because it's internal
}