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
#include <QTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>
#include <QUrl>
#include <QUuid>
#include <algorithm>

using namespace UserInterface::Netplay;

namespace {
constexpr int kCheatsChunkSize = 32;

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
} // namespace

SocketIOClient::SocketIOClient(const QString& serverUrl, QObject* parent)
    : QObject(parent)
    , m_serverUrl(serverUrl)
    , m_connectionState(Disconnected)
    , m_pingTimer(nullptr)
{
    m_webSocket = std::make_unique<QWebSocket>();
    m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_persistentId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QObject::connect(m_webSocket.get(), &QWebSocket::connected, this, &SocketIOClient::on_connected);
    QObject::connect(m_webSocket.get(), &QWebSocket::disconnected, this, &SocketIOClient::on_disconnected);
    QObject::connect(m_webSocket.get(), &QWebSocket::textMessageReceived, this, &SocketIOClient::on_textMessageReceived);
    QObject::connect(m_webSocket.get(), QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &SocketIOClient::on_error);
    QObject::connect(m_webSocket.get(), &QWebSocket::pong, this, &SocketIOClient::on_pong);
    // Note: QWebSocket may not directly expose sslErrors signal
    // If it does, uncomment the following line:
    // QObject::connect(m_webSocket.get(), QOverload<const QList<QSslError>&>::of(&QWebSocket::sslErrors),
    //         this, &SocketIOClient::on_sslErrors);
}

SocketIOClient::~SocketIOClient()
{
    if (m_webSocket && m_webSocket->isValid()) {
        m_webSocket->close();
    }
    if (m_pingTimer) {
        m_pingTimer->stop();
        delete m_pingTimer;
    }
}

void SocketIOClient::connectToServer(const QString& playerName)
{
    m_playerName = playerName;
    m_connectionState = Connecting;

    // Convert HTTP URL to WebSocket URL
    QString wsUrl = m_serverUrl;
    wsUrl.replace("http://", "ws://");
    wsUrl.replace("https://", "wss://");
    wsUrl = wsUrl + "/socket.io/?EIO=4&transport=websocket";

    qDebug() << "Connecting to Socket.IO server:" << wsUrl;
    m_webSocket->open(QUrl(wsUrl));
}

void SocketIOClient::disconnect()
{
    if (m_pingTimer) {
        m_pingTimer->stop();
    }
    if (m_webSocket && m_webSocket->isValid()) {
        m_webSocket->close();
    }
    m_connectionState = Disconnected;
    m_roomId.clear();
    m_playerId.clear();
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
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject payload;
    payload["frame"] = static_cast<qint64>(frameNumber);
    payload["input"] = static_cast<qint64>(controllerState);
    emitEvent("controller-input", payload);
}

void SocketIOClient::sendFrameSync(uint32_t frameNumber)
{
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject payload;
    payload["frame"] = static_cast<qint64>(frameNumber);
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

void SocketIOClient::sendEmulationPauseUpdate(bool paused)
{
    if (m_connectionState != Connected) {
        return;
    }

    QJsonObject payload;
    payload["paused"] = paused;
    emitEvent("emulation-paused", payload);
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

//
// Private Slots
//

void SocketIOClient::on_connected()
{
    qDebug() << "Socket.IO connected";
    m_connectionState = Connected;

    // Send Socket.IO connection packet (CONNECT - message type 0)
    m_webSocket->sendTextMessage("0");

    emit connected();

    // Start keep-alive ping
    if (!m_pingTimer) {
        m_pingTimer = new QTimer(this);
        QObject::connect(m_pingTimer, &QTimer::timeout, [this]() {
            if (m_webSocket && m_connectionState == Connected) {
                m_webSocket->ping();
            }
        });
    }
    m_pingTimer->start(5000); // Ping every 5 seconds

}

void SocketIOClient::on_disconnected()
{
    qDebug() << "Socket.IO disconnected";
    m_connectionState = Disconnected;
    if (m_pingTimer) {
        m_pingTimer->stop();
    }
    emit disconnected();
}

void SocketIOClient::on_textMessageReceived(const QString& message)
{
    handleSocketIOMessage(message);
}

void SocketIOClient::on_error(QAbstractSocket::SocketError error)
{
    qWarning() << "Socket.IO error:" << error << m_webSocket->errorString();
    m_connectionState = Error;
    const QString message = m_webSocket->errorString().isEmpty()
                                ? QStringLiteral("WebSocket error %1").arg(static_cast<int>(error))
                                : m_webSocket->errorString();
    emit connectionError(message);
}

void SocketIOClient::on_pong(quint64 elapsedTime, const QByteArray& payload)
{
    Q_UNUSED(elapsedTime);
    Q_UNUSED(payload);
}

void SocketIOClient::on_sslErrors(const QList<QSslError>& errors)
{
    for (const auto& error : errors) {
        qWarning() << "Socket.IO SSL Error:" << error.errorString();
    }
    // For now, we ignore SSL errors to allow self-signed certificates
    // In production, this should be more carefully handled
    if (m_webSocket) {
        m_webSocket->ignoreSslErrors();
    }
}

//
// Private Methods
//

void SocketIOClient::handleSocketIOMessage(const QString& message)
{
    // Socket.IO message format: [messageType, ...args]
    // messageType 0 = CONNECT
    // messageType 2 = EVENT
    // messageType 3 = ACK
    // messageType 4 = ERROR
    // messageType 5 = BINARY_EVENT
    // messageType 6 = BINARY_ACK

    if (message.isEmpty()) {
        return;
    }

    // First character is the message type
    QChar msgType = message[0];
    QString payload = message.length() > 1 ? message.mid(1) : "";

    if (msgType == '0') {
        // CONNECT
        qDebug() << "Socket.IO CONNECT received";
    } else if (msgType == '2') {
        // EVENT - payload can be [eventName, ...args] or [2, eventName, ...args]
        QJsonDocument doc = QJsonDocument::fromJson(payload.toUtf8());
        if (doc.isArray()) {
            QJsonArray arr = doc.array();
            if (arr.size() > 0) {
                int eventIndex = 0;

                // Server may include Socket.IO event type (2) as first element.
                if (arr[0].isDouble() && arr[0].toInt() == 2) {
                    eventIndex = 1;
                }

                if (arr.size() > eventIndex && arr[eventIndex].isString()) {
                    QString eventName = arr[eventIndex].toString();
                    QJsonArray args;
                    for (int i = eventIndex + 1; i < arr.size(); ++i) {
                        args.append(arr[i]);
                    }
                    handleEvent(eventName, args);
                }
            }
        }
    } else if (msgType == '3') {
        // ACK
        qDebug() << "Socket.IO ACK received";
    } else if (msgType == '4') {
        // ERROR
        qWarning() << "Socket.IO ERROR received:" << payload;
    }
}

void SocketIOClient::handleEvent(const QString& eventName, const QJsonArray& args)
{
    if (eventName == "room-created" && args.size() > 0) {
        QJsonObject data = args[0].toObject();
        QString roomId = data["roomId"].toString();
        m_roomId = roomId;
        m_currentRoom.roomId = roomId;
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
        qDebug() << "Room joined:" << roomId << "slot:" << slotIndex;
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
        emit frameSyncReceived(slot, frameNumber);

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
        QString token = args[0].toObject()["token"].toString();
        emit reconnectTokenReceived(token);

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

    } else if (eventName == "update-input-delay" && args.size() > 0) {
        QJsonObject data = args[0].toObject();
        emit inputDelayReceived(data["frames"].toInt(4));

    } else if (eventName == "emulation-paused" && args.size() > 0) {
        QJsonObject data = args[0].toObject();
        emit emulationPauseReceived(data["paused"].toBool(false));

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
    QJsonArray arr;
    arr.append(2);  // Socket.IO EVENT type
    arr.append(eventName);
    arr.append(payload);
    
    QString jsonStr = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    QString message = QString("2") + jsonStr; // Engine.IO MESSAGE type is '2'
    
    if (eventName != "controller-input") {
        qDebug() << "SocketIOClient: Emitting event" << eventName;
    }
    
    if (m_webSocket && m_connectionState == Connected) {
        m_webSocket->sendTextMessage(message);
    } else {
        qWarning() << "SocketIOClient: Cannot emit event, not connected or no socket";
    }
}

void SocketIOClient::emitEvent(const QString& eventName, const QJsonArray& payload)
{
    QJsonArray arr;
    arr.append(2);  // Socket.IO EVENT type
    arr.append(eventName);
    for (const auto& item : payload) {
        arr.append(item);
    }
    
    QString jsonStr = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    QString message = QString("2") + jsonStr;  // Engine.IO MESSAGE type is '2'
    
    if (m_webSocket && m_connectionState == Connected) {
        m_webSocket->sendTextMessage(message);
    }
}

QJsonObject SocketIOClient::buildOpenRoomPayload(const QString& roomName, const QString& gameId, int maxPlayers)
{
    QJsonObject extra;
    extra["sessionid"] = m_sessionId;
    extra["persistentId"] = m_persistentId;
    extra["reconnectToken"] = "";
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
    extra["reconnectToken"] = "";
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