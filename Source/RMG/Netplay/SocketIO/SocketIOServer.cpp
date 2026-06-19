#include "SocketIOServer.hpp"
#include <QWebSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>
#include <QDebug>
#include <QTimer>
#include <algorithm>

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

void SocketIOServer::rebuildLobbySlots(SignalingRoom& room)
{
    room.players.clear();

    int slotIndex = 0;
    for (auto* player : room.lobbyOrder)
    {
        if (!player)
        {
            continue;
        }

        player->slotIndex = slotIndex;
        player->playerId = QString("p%1").arg(slotIndex);
        room.players[slotIndex] = player;
        ++slotIndex;
    }
}

SocketIOServer::SocketIOServer(QObject* parent)
    : QObject(parent)
{
    m_pingTimer = new QTimer(this);
    m_pingTimer->setInterval(10000);
    connect(m_pingTimer, &QTimer::timeout, this, &SocketIOServer::onPingTimer);
}

SocketIOServer::~SocketIOServer()
{
    stopServer();
}

bool SocketIOServer::startServer(int port)
{
    if (m_server && m_server->isListening())
        return false;

    m_server = std::make_unique<QWebSocketServer>(
        "RMG-Signaling-Server",
        QWebSocketServer::SslMode::NonSecureMode,
        this);

    const qint64 maxPayloadLimit = 128 * 1024 * 1024; 

    if (!m_server->listen(QHostAddress::Any, port))
    {
        qWarning() << "Signaling server failed to listen on port" << port;
        m_server.reset();
        return false;
    }

    connect(m_server.get(), &QWebSocketServer::newConnection,
            this, &SocketIOServer::onNewConnection);

    m_pingTimer->start();

    qInfo() << "Signaling server started on port" << port;
    return true;
}

void SocketIOServer::stopServer()
{
    if (!m_server)
        return;

    // Disconnect all clients
    for (auto* client : m_clients.keys())
    {
        client->disconnect();
        client->close();
    }
    m_clients.clear();
    m_clientsById.clear();
    m_rooms.clear();

    if (m_pingTimer) {
        m_pingTimer->stop();
    }

    m_server->close();
    m_server.reset();

    qInfo() << "Signaling server stopped";
}

int SocketIOServer::getPort() const
{
    if (!m_server)
        return -1;
    return m_server->serverPort();
}

void SocketIOServer::createInitialRoom(const QString& roomId, const QString& hostName, const QString& gameName)
{
    SignalingRoom room;
    room.id = roomId;
    room.hostId = "host"; // Match placeholder ID
    room.roomName = hostName;
    room.gameName = gameName;
    room.gameId = gameName;
    room.maxPlayers = 4;
    room.started = false;
    
    // Explicitly initialize state storage
    room.activeCheats = QJsonArray();
    room.activeSaves = QJsonArray();
    room.inputDelayFrames = 4;
    
    auto* hostClient = new ClientConnection();
    hostClient->id = "host";
    hostClient->name = hostName;
    hostClient->roomId = roomId;
    hostClient->slotIndex = 0;
    hostClient->playerId = "p0";
    hostClient->socket = nullptr;
    room.lobbyOrder.append(hostClient);
    rebuildLobbySlots(room);
    
    m_rooms[roomId] = room;
    qInfo() << "SocketIOServer: Created initial room" << roomId << "for host" << hostName;
}

void SocketIOServer::handle_JoinRoom(QWebSocket* socket, const QJsonObject& msg)
{
    ClientConnection* client = getClientFromSocket(socket);
    if (!client)
        return;

    QString roomId = msg["roomId"].toString();
    if (roomId.isEmpty())
    {
        QJsonObject extra = msg["extra"].toObject();
        roomId = extra["roomId"].toString();
    }

    SignalingRoom* room = getRoomById(roomId);
    if (!room)
    {
        QJsonObject error;
        error["error"] = "Room not found";
        emitToClient(client->id, "join-failed", error);
        return;
    }

    if (room->started)
    {
        QJsonObject error;
        error["error"] = "Game already started";
        emitToClient(client->id, "join-failed", error);
        return;
    }

    if (room->lobbyOrder.size() >= room->maxPlayers)
    {
        QJsonObject error;
        error["error"] = "Room is full";
        emitToClient(client->id, "join-failed", error);
        return;
    }

    client->roomId = roomId;
    room->lobbyOrder.append(client);
    rebuildLobbySlots(*room);

    QJsonObject extra = msg["extra"].toObject();
    QString requestedName = extra["player_name"].toString().trimmed();
    if (client->name.isEmpty() && !requestedName.isEmpty())
    {
        client->name = requestedName;
    }
    if (client->name.isEmpty())
    {
        client->name = QString("P%1").arg(client->slotIndex + 1);
    }

    // 1. Acknowledge successful entry to client
    QJsonObject response;
    response["roomId"] = roomId;
    response["slotIndex"] = client->slotIndex;
    response["playerId"] = client->playerId;
    emitToClient(client->id, "room-joined", response);

    // 2. WORKAROUND: Catch up the joining user instantly on current room configurations
    if (!room->activeCheats.isEmpty())
    {
        const int chunkCount = (room->activeCheats.size() + kCheatsChunkSize - 1) / kCheatsChunkSize;
        if (chunkCount <= 1)
        {
            emitToClient(client->id, "cheats-updated", buildCheatsPayload(room->activeCheats));
        }
        else
        {
            const QString chunkId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            for (int chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
            {
                const QJsonArray chunk = sliceJsonArray(room->activeCheats, chunkIndex * kCheatsChunkSize, kCheatsChunkSize);
                emitToClient(client->id, "cheats-updated", buildCheatsPayload(chunk, chunkId, chunkIndex, chunkCount));
            }
        }
    }
    if (!room->activeSaves.isEmpty())
    {
        QJsonObject savePayload;
        savePayload["files"] = room->activeSaves;
        emitToClient(client->id, "save-sync", savePayload);
    }
    if (!room->activeCoreSettings.isEmpty())
    {
        emitToClient(client->id, "core-settings-sync", room->activeCoreSettings);
    }
    
    QJsonObject delayPayload;
    delayPayload["frames"] = room->inputDelayFrames;
    emitToClient(client->id, "update-input-delay", delayPayload);

    // 3. Inform everyone else about structural change
    broadcastRoomUpdate(roomId);

    qInfo() << "Player joined room:" << roomId << "slot:" << client->slotIndex << "and caught up on game states.";
    emit playerJoined(roomId, client->id, client->slotIndex);
}

void SocketIOServer::handle_CheatsUpdate(QWebSocket* socket, const QJsonObject& msg)
{
    ClientConnection* client = getClientFromSocket(socket);
    if (!client || client->roomId.isEmpty())
        return;

    SignalingRoom* room = getRoomById(client->roomId);
    if (!room)
        return;

    // 1. Extract the array cleanly from the incoming JSON object
    QJsonArray cheatsArray = msg.value("cheats").toArray();
    const QString chunkId = msg.value("chunkId").toString();
    const int chunkIndex = msg.value("chunkIndex").toInt(-1);
    const int chunkCount = msg.value("chunkCount").toInt(0);

    if (chunkCount > 1 && !chunkId.isEmpty() && chunkIndex >= 0)
    {
        const QString batchKey = client->roomId + ":" + chunkId;
        ChunkedCheatUpdate& update = m_pendingCheatUpdates[batchKey];
        update.chunkCount = chunkCount;
        update.chunks[chunkIndex] = cheatsArray;

        QJsonObject payload = buildCheatsPayload(cheatsArray, chunkId, chunkIndex, chunkCount);

        for (auto it = room->players.constBegin(); it != room->players.constEnd(); ++it)
        {
            ClientConnection* player = it.value();
            if (player && player->id != client->id && player->socket && player->socket->isValid())
            {
                emitToClient(player->id, "cheats-updated", payload);
            }
        }

        if (update.chunkCount > 0 && update.chunks.size() >= update.chunkCount)
        {
            QJsonArray combinedCheats;
            for (auto chunkIt = update.chunks.constBegin(); chunkIt != update.chunks.constEnd(); ++chunkIt)
            {
                const QJsonArray& chunk = chunkIt.value();
                for (const auto& cheatValue : chunk)
                {
                    combinedCheats.append(cheatValue);
                }
            }

            room->activeCheats = combinedCheats;
            m_pendingCheatUpdates.remove(batchKey);
            emit cheatsUpdated(client->roomId, room->activeCheats);
        }

        return;
    }

    // 2. Cache it on the server for future joiners
    room->activeCheats = cheatsArray;

    // 3. Broadcast a stable payload shape used by SocketIOClient.
    QJsonObject payload = buildCheatsPayload(cheatsArray);

    // 4. Broadcast to everyone else in the room
    for (auto it = room->players.constBegin(); it != room->players.constEnd(); ++it)
    {
        ClientConnection* player = it.value();
        // Check player validity, ensure it's not the sender, and make sure their socket is alive
        if (player && player->id != client->id && player->socket && player->socket->isValid())
        {
            emitToClient(player->id, "cheats-updated", payload);
        }
    }

    emit cheatsUpdated(client->roomId, room->activeCheats);
}


void SocketIOServer::handle_SaveSyncUpdate(QWebSocket* socket, const QJsonObject& msg)
{
    ClientConnection* client = getClientFromSocket(socket);
    if (!client || client->roomId.isEmpty())
        return;

    SignalingRoom* room = getRoomById(client->roomId);
    if (!room)
        return;

    // Cache structural save files
    room->activeSaves = msg.value("files").toArray();

    QJsonObject payload;
    payload["files"] = room->activeSaves;

    for (auto it = room->players.constBegin(); it != room->players.constEnd(); ++it)
    {
        ClientConnection* player = it.value();
        if (player && player->id != client->id && player->socket && player->socket->isValid())
        {
            emitToClient(player->id, "save-sync", payload);
        }
    }
    emit saveSyncReceived(client->roomId, room->activeSaves);
}

void SocketIOServer::handle_InputDelayUpdate(QWebSocket* socket, const QJsonObject& msg)
{
    ClientConnection* client = getClientFromSocket(socket);
    if (!client || client->roomId.isEmpty())
        return;

    SignalingRoom* room = getRoomById(client->roomId);
    if (!room || room->hostId != client->id)
        return; // Only host should configure latency properties

    int frames = msg.value("frames").toInt(4);
    room->inputDelayFrames = qBound(1, frames, 99);

    QJsonObject payload;
    payload["frames"] = room->inputDelayFrames;
    
    // Distribute frame delay configuration across active clients securely
    for (auto* player : room->players)
    {
        if (player && player->socket && player->socket->isValid())
        {
            emitToClient(player->id, "update-input-delay", payload);
        }
    }
}

void SocketIOServer::handle_EmulationPauseUpdate(QWebSocket* socket, const QJsonObject& msg)
{
    ClientConnection* client = getClientFromSocket(socket);
    if (!client || client->roomId.isEmpty())
        return;

    SignalingRoom* room = getRoomById(client->roomId);
    if (!room)
        return;

    QJsonObject payload;
    payload["paused"] = msg.value("paused").toBool(false);
    emitToRoom(client->roomId, "emulation-paused", payload);
}

bool SocketIOServer::startHostedGame(const QString& roomId, const QString& mode, bool resyncEnabled, const QString& romHash,
                                     const QJsonArray& cheats, const QJsonArray& saveFiles)
{
    SignalingRoom* room = getRoomById(roomId);
    if (!room)
    {
        qWarning() << "SocketIOServer: Cannot start hosted game, room not found" << roomId;
        return false;
    }

    if (room->players.size() < 2)
    {
        qWarning() << "SocketIOServer: Cannot start hosted game, need at least 2 players in room" << roomId;
        return false;
    }

    if (room->started)
    {
        return true;
    }

    room->started = true;
    room->emulationReadySlots.clear();
    room->emulationBeginSent = false;

    QJsonObject payload;
    payload["mode"] = mode;
    payload["resyncEnabled"] = resyncEnabled;
    payload["romHash"] = romHash;
    payload["matchId"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Q_UNUSED(saveFiles);
    emitToConnectedRoomClients(roomId, "game-started", payload);

    if (!cheats.isEmpty())
    {
        broadcastCheatsUpdate(roomId, cheats);
    }

    qInfo() << "SocketIOServer: Hosted game started in room" << roomId;
    emit gameStarted(roomId);
    return true;
}

void SocketIOServer::broadcastControllerInput(const QString& roomId, int slot, uint32_t frameNumber, uint32_t controllerState)
{
    SignalingRoom* room = getRoomById(roomId);
    if (!room)
    {
        return;
    }

    QJsonObject payload;
    payload["slot"] = slot;
    payload["frame"] = static_cast<qint64>(frameNumber);
    payload["input"] = static_cast<qint64>(controllerState);

    // Broadcast to all other players (not the originating player who already submitted locally)
    for (auto* player : room->players)
    {
        if (player && player->slotIndex != slot)
        {
            emitToClient(player->id, "controller-input", payload);
        }
    }
}

void SocketIOServer::broadcastFrameSync(const QString& roomId, int slot, uint32_t frameNumber, uint32_t stateHash)
{
    SignalingRoom* room = getRoomById(roomId);
    if (!room)
    {
        return;
    }

    QJsonObject payload;
    payload["slot"] = slot;
    payload["frame"] = static_cast<qint64>(frameNumber);
    payload["hash"] = static_cast<qint64>(stateHash);

    for (auto* player : room->players)
    {
        if (player && player->slotIndex != slot)
        {
            emitToClient(player->id, "frame-sync", payload);
        }
    }
}

void SocketIOServer::broadcastCheatsUpdate(const QString& roomId, const QJsonArray& cheats)
{
    SignalingRoom* room = getRoomById(roomId);
    if (!room)
    {
        qWarning() << "SocketIOServer: Cannot broadcast cheats update, room not found" << roomId;
        return;
    }

    room->activeCheats = cheats;

    const int chunkCount = (cheats.size() + kCheatsChunkSize - 1) / kCheatsChunkSize;
    if (chunkCount <= 1)
    {
        emitToConnectedRoomClients(roomId, "cheats-updated", buildCheatsPayload(cheats));
    }
    else
    {
        const QString chunkId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        for (int chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
        {
            const QJsonArray chunk = sliceJsonArray(cheats, chunkIndex * kCheatsChunkSize, kCheatsChunkSize);
            emitToConnectedRoomClients(roomId, "cheats-updated", buildCheatsPayload(chunk, chunkId, chunkIndex, chunkCount));
        }
    }

    emit cheatsUpdated(roomId, room->activeCheats);
}

void SocketIOServer::broadcastCoreSettingsSync(const QString& roomId, const QJsonObject& coreSettings)
{
    SignalingRoom* room = getRoomById(roomId);
    if (!room)
    {
        qWarning() << "SocketIOServer: Cannot broadcast core settings sync, room not found" << roomId;
        return;
    }

    room->activeCoreSettings = coreSettings;
    emitToConnectedRoomClients(roomId, "core-settings-sync", coreSettings);
    emit coreSettingsSyncReceived(roomId, coreSettings);
}

void SocketIOServer::broadcastSaveSync(const QString& roomId, const QJsonArray& saveFiles)
{
    SignalingRoom* room = getRoomById(roomId);
    if (!room)
    {
        qWarning() << "SocketIOServer: Cannot broadcast save sync, room not found" << roomId;
        return;
    }

    QJsonObject payload;
    payload["files"] = saveFiles;
    emitToConnectedRoomClients(roomId, "save-sync", payload);
}

void SocketIOServer::broadcastInputDelayUpdate(const QString& roomId, int frames)
{
    SignalingRoom* room = getRoomById(roomId);
    if (!room)
    {
        qWarning() << "SocketIOServer: Cannot broadcast input delay, room not found" << roomId;
        return;
    }

    if (frames < 1) {
        frames = 1;
    } else if (frames > 99) {
        frames = 99;
    }

    QJsonObject payload;
    payload["frames"] = frames;
    emitToRoom(roomId, "update-input-delay", payload);
}

void SocketIOServer::broadcastEmulationPauseUpdate(const QString& roomId, bool paused)
{
    SignalingRoom* room = getRoomById(roomId);
    if (!room)
    {
        qWarning() << "SocketIOServer: Cannot broadcast emulation pause, room not found" << roomId;
        return;
    }

    QJsonObject payload;
    payload["paused"] = paused;
    emitToRoom(roomId, "emulation-paused", payload);
}

void SocketIOServer::markEmulationReady(const QString& roomId, int slotIndex)
{
    SignalingRoom* room = getRoomById(roomId);
    if (!room || !room->started || slotIndex < 0) {
        return;
    }

    room->emulationReadySlots.insert(slotIndex);
    tryBroadcastEmulationBegin(room);
}

void SocketIOServer::tryBroadcastEmulationBegin(SignalingRoom* room)
{
    if (!room || room->emulationBeginSent || !room->started) {
        return;
    }

    const int requiredPlayers = room->lobbyOrder.size();
    if (requiredPlayers < 2) {
        return;
    }

    for (int slot = 0; slot < requiredPlayers; ++slot) {
        if (!room->emulationReadySlots.contains(slot)) {
            return;
        }
    }

    room->emulationBeginSent = true;

    qInfo() << "SocketIOServer: All players ready in room" << room->id
            << "- broadcasting emulation-begin";

    emitToConnectedRoomClients(room->id, "emulation-begin", QJsonObject());
    emit emulationBegin(room->id);
}

void SocketIOServer::handle_EmulationReady(QWebSocket* socket, const QJsonObject& msg)
{
    Q_UNUSED(msg);

    ClientConnection* client = getClientFromSocket(socket);
    if (!client || client->roomId.isEmpty() || client->slotIndex < 0) {
        return;
    }

    SignalingRoom* room = getRoomById(client->roomId);
    if (!room || !room->started) {
        return;
    }

    markEmulationReady(client->roomId, client->slotIndex);
}

void SocketIOServer::broadcastChatMessage(const QString& roomId, const QString& playerName, const QString& message)
{
    SignalingRoom* room = getRoomById(roomId);
    if (!room)
    {
        qWarning() << "SocketIOServer: Cannot broadcast chat message, room not found" << roomId;
        return;
    }

    if (message.isEmpty())
    {
        return;
    }

    QJsonObject payload;
    payload["playerName"] = playerName;
    payload["message"] = message;
    emitToRoom(roomId, "chat-message", payload);
}

void SocketIOServer::onNewConnection()
{
    if (!m_server)
        return;

    // Loop through all pending connections waiting in the queue
    while (true)
    {
        QWebSocket* socket = m_server->nextPendingConnection();
        if (!socket)
            break; // No more clients waiting, exit cleanly

        // 1. Set payload limits immediately on every connection as they come in
        socket->setMaxAllowedIncomingMessageSize(15728640);
        socket->setMaxAllowedIncomingFrameSize(15728640);

        // 2. Create and configure your client connection safely
        auto* client = new ClientConnection();
        client->id = generateClientId();
        client->socket = socket;

        m_clients[socket] = client;
        m_clientsById[client->id] = client;

        connect(socket, &QWebSocket::textMessageReceived,
                this, &SocketIOServer::onTextMessageReceived);
        connect(socket, &QWebSocket::disconnected,
                this, &SocketIOServer::onClientDisconnected);
        connect(socket, &QWebSocket::pong, this,
                [client](quint64 elapsedTime, const QByteArray&) {
            if (client) {
                client->lastPingMs = static_cast<int>(elapsedTime);
            }
        });

        // Socket.IO connect message (engine.io type 0)
        QJsonArray connectMsg;
        connectMsg.append(0);  // Socket.IO CONNECT type
        connectMsg.append(QString::number(0));
        QJsonObject data;
        data["sid"] = client->id;
        connectMsg.append(data);
        sendSocketIOMessage(socket, 0, connectMsg);

        qInfo() << "Client connected:" << client->id;
        emit clientConnected(client->id);
    }
}

void SocketIOServer::onTextMessageReceived(const QString& message)
{
    QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
    if (!socket)
        return;

    handleSocketIOMessage(socket, message);
}

void SocketIOServer::onClientDisconnected()
{
    QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
    if (!socket)
        return;

    ClientConnection* client = getClientFromSocket(socket);
    if (!client)
        return;

    QString clientId = client->id;
    QString roomId = client->roomId;

    // Remove from room
    if (!roomId.isEmpty())
    {
        SignalingRoom* room = getRoomById(roomId);
        if (room)
        {
            // Remove player from slot
            if (client->slotIndex >= 0 && client->slotIndex < 4)
            {
                room->players.remove(client->slotIndex);
            }

            // Broadcast room update
            broadcastRoomUpdate(roomId);

            // If host left, close room
            if (room->hostId == clientId)
            {
                m_rooms.remove(roomId);
            }

            emit playerLeft(roomId, clientId);
        }
    }

    m_clients.remove(socket);
    m_clientsById.remove(clientId);
    delete client;

    qInfo() << "Client disconnected:" << clientId;
    emit clientDisconnected(clientId);
}

void SocketIOServer::handleSocketIOMessage(QWebSocket* socket, const QString& message)
{
    // Socket.IO message format: engine.io type + Socket.IO packet
    // For example: "0" = Engine.IO OPEN, "2" = Engine.IO MESSAGE + Socket.IO packet

    if (message.isEmpty())
        return;

    char engineType = message[0].toLatin1();

    switch (engineType)
    {
        case '2':  // Engine.IO MESSAGE
        {
            // Socket.IO packet is after the engine.io type
            QString socketIOPacket = message.mid(1);

            // Parse Socket.IO JSON
            // Typical format: [Socket.IO type, event name, data object]
            QJsonDocument doc = QJsonDocument::fromJson(socketIOPacket.toUtf8());
            if (doc.isArray())
            {
                QJsonArray arr = doc.array();
                if (arr.size() >= 1)
                {
                    handleEvent(socket, arr);
                }
            }
            break;
        }

        case '4':  // Engine.IO UPGRADE
        case '3':  // Engine.IO UPGRADE
            // Acknowledge upgrade
            break;

        default:
            break;
    }
}

void SocketIOServer::handleEvent(QWebSocket* socket, const QJsonArray& args)
{
    if (args.isEmpty())
        return;

    // Socket.IO event: [type, namespace, data, ...]
    // type 2 = EVENT
    // For our purposes: [2, event_name, {...data}]

    int type = args[0].toInt(2);  // Default to EVENT type

    if (type == 2 && args.size() >= 2)  // Socket.IO EVENT
    {
        if (!args[1].isString())
            return;

        QString eventName = args[1].toString();
        QJsonObject data = (args.size() >= 3) ? args[2].toObject() : QJsonObject();

        if (eventName == "open-room")
            handle_OpenRoom(socket, data);
        else if (eventName == "join-room")
            handle_JoinRoom(socket, data);
        else if (eventName == "leave-room")
            handle_LeaveRoom(socket, data);
        else if (eventName == "claim-slot")
            handle_ClaimSlot(socket, data);
        else if (eventName == "set-name")
            handle_SetName(socket, data);
        else if (eventName == "webrtc-signal")
            handle_WebRTCSignal(socket, data);
        else if (eventName == "start-game")
            handle_StartGame(socket, data);
        else if (eventName == "list-rooms")
            handle_ListRooms(socket, data);
        else if (eventName == "chat-message")
            handle_ChatMessage(socket, data);
        else if (eventName == "cheats-update")
            handle_CheatsUpdate(socket, data);
        else if (eventName == "save-sync")
            handle_SaveSyncUpdate(socket, data);
        else if (eventName == "controller-input")
            handle_ControllerInput(socket, data);
        else if (eventName == "frame-sync")
            handle_FrameSync(socket, data);
        else if (eventName == "update-input-delay")
            handle_InputDelayUpdate(socket, data);
        else if (eventName == "emulation-paused")
            handle_EmulationPauseUpdate(socket, data);
        else if (eventName == "emulation-ready")
            handle_EmulationReady(socket, data);
    }
}

void SocketIOServer::handle_OpenRoom(QWebSocket* socket, const QJsonObject& msg)
{
    ClientConnection* client = getClientFromSocket(socket);
    if (!client)
        return;

    // Leave existing room if any
    if (!client->roomId.isEmpty())
    {
        SignalingRoom* room = getRoomById(client->roomId);
        if (room && client->slotIndex >= 0)
        {
            room->players.remove(client->slotIndex);
            broadcastRoomUpdate(client->roomId);
        }
    }

    // Create new room
    QString roomId = generateRoomId();
    SignalingRoom room;
    room.id = roomId;
    room.hostId = client->id;
    QJsonObject extra = msg["extra"].toObject();
    room.roomName = extra["room_name"].toString(client->name);
    room.gameId = extra["game_id"].toString("netplay");
    room.gameName = room.gameId;
    room.maxPlayers = msg["maxPlayers"].toInt(4);
    room.started = false;

    // Host takes slot 0
    client->roomId = roomId;
    client->slotIndex = 0;
    client->playerId = QString("p%1").arg(0);
    room.players[0] = client;

    m_rooms[roomId] = room;

    // Send room created response
    QJsonObject response;
    response["roomId"] = roomId;
    response["slotIndex"] = 0;
    response["playerId"] = "p0";
    emitToClient(client->id, "room-created", response);

    qInfo() << "Room created:" << roomId << "by" << client->id;
    emit roomCreated(roomId);
}

void SocketIOServer::handle_LeaveRoom(QWebSocket* socket, const QJsonObject& msg)
{
    ClientConnection* client = getClientFromSocket(socket);
    if (!client)
        return;

    if (client->roomId.isEmpty())
        return;

    QString roomId = client->roomId;
    SignalingRoom* room = getRoomById(roomId);

    if (room)
    {
        room->lobbyOrder.removeAll(client);
        rebuildLobbySlots(*room);
        broadcastRoomUpdate(roomId);

        // If host left, close room
        if (room->hostId == client->id)
        {
            m_rooms.remove(roomId);
        }
    }

    client->roomId.clear();
    client->slotIndex = -1;
    client->playerId.clear();

    qInfo() << "Player left room:" << roomId;
    emit playerLeft(roomId, client->id);
}

void SocketIOServer::handle_ClaimSlot(QWebSocket* socket, const QJsonObject& msg)
{
    ClientConnection* client = getClientFromSocket(socket);
    if (!client || client->roomId.isEmpty())
        return;

    // Lobby controller ports are fixed to join order.
    Q_UNUSED(msg);
}

void SocketIOServer::handle_SetName(QWebSocket* socket, const QJsonObject& msg)
{
    ClientConnection* client = getClientFromSocket(socket);
    if (!client)
        return;

    client->name = msg["name"].toString();
    qDebug() << "Player set name:" << client->name;


    if (!client->roomId.isEmpty())
    {
        broadcastRoomUpdate(client->roomId);
    }
}

bool SocketIOServer::relayHostedWebRTCSignal(const QString& roomId, const QString& fromPlayerId,
                                             const QJsonObject& msg)
{
    SignalingRoom* room = getRoomById(roomId);
    if (!room)
        return false;

    QString toPlayerId = msg.value(QStringLiteral("to")).toString();
    if (toPlayerId.isEmpty()) {
        toPlayerId = msg.value(QStringLiteral("target")).toString();
    }
    if (toPlayerId.isEmpty()) {
        return false;
    }

    ClientConnection* targetClient = nullptr;
    for (auto* player : room->players)
    {
        if (player && player->playerId == toPlayerId)
        {
            targetClient = player;
            break;
        }
    }

    if (!targetClient || !targetClient->socket || !targetClient->socket->isValid()) {
        return false;
    }

    QJsonObject signal = msg;
    signal[QStringLiteral("from")] = fromPlayerId;
    emitToClient(targetClient->id, QStringLiteral("webrtc-signal"), signal);
    return true;
}

void SocketIOServer::handle_WebRTCSignal(QWebSocket* socket, const QJsonObject& msg)
{
    ClientConnection* client = getClientFromSocket(socket);
    if (!client || client->roomId.isEmpty())
        return;

    QString toPlayerId = msg.value(QStringLiteral("to")).toString();
    if (toPlayerId.isEmpty()) {
        toPlayerId = msg.value(QStringLiteral("target")).toString();
    }
    if (toPlayerId.isEmpty()) {
        qWarning() << "SocketIOServer: WebRTC signal missing target player";
        return;
    }

    SignalingRoom* room = getRoomById(client->roomId);
    if (!room)
        return;

    ClientConnection* targetClient = nullptr;
    for (auto* player : room->players)
    {
        if (player && player->playerId == toPlayerId)
        {
            targetClient = player;
            break;
        }
    }

    if (!targetClient) {
        qWarning() << "SocketIOServer: WebRTC target player not found:" << toPlayerId;
        return;
    }

    QJsonObject signal = msg;
    signal[QStringLiteral("from")] = client->playerId;

    if (!targetClient->socket || !targetClient->socket->isValid()) {
        emit hostedWebRTCSignalReceived(client->playerId, signal);
        qDebug() << "SocketIOServer: WebRTC signal delivered to embedded host from" << client->playerId;
        return;
    }

    emitToClient(targetClient->id, QStringLiteral("webrtc-signal"), signal);

    qDebug() << "SocketIOServer: WebRTC signal relayed from" << client->playerId
             << "to" << toPlayerId;
}

void SocketIOServer::handle_StartGame(QWebSocket* socket, const QJsonObject& msg)
{
    ClientConnection* client = getClientFromSocket(socket);
    if (!client || client->roomId.isEmpty())
        return;

    SignalingRoom* room = getRoomById(client->roomId);
    if (!room || room->hostId != client->id)
        return;  // Only host can start game

    if (room->players.size() < 2)
        return;  // Need at least 2 players

    room->started = true;
    room->emulationReadySlots.clear();
    room->emulationBeginSent = false;

    // Broadcast game start to all players in room
    emitToRoom(client->roomId, "game-started", QJsonObject());

    qInfo() << "Game started in room:" << client->roomId;
    emit gameStarted(client->roomId);
}

void SocketIOServer::handle_ListRooms(QWebSocket* socket, const QJsonObject& msg)
{
    ClientConnection* client = getClientFromSocket(socket);
    if (!client)
        return;

    qDebug() << "SocketIOServer: Client requesting room list";

    const bool waitingOnly = msg.value("waiting").toBool(false);
    
    QJsonArray roomsArray;

    for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it)
    {
        const SignalingRoom& room = it.value();

        if (waitingOnly)
        {
            if (room.started)
                continue;
        }
        else
        {
            if (!room.started)
                continue;
        }

        QJsonObject roomObj;
        roomObj["roomId"] = room.id;
        roomObj["hostCode"] = room.id;
        roomObj["hostId"] = room.hostId;
        roomObj["hostName"] = room.hostId;
        roomObj["playerName"] = room.hostId;
        roomObj["roomName"] = room.roomName;
        roomObj["gameName"] = room.gameName;
        roomObj["gameId"] = room.gameId;
        roomObj["playerCount"] = room.players.size();
        roomObj["maxPlayers"] = room.maxPlayers;
        roomObj["lobbySize"] = QString("%1/%2").arg(room.players.size()).arg(room.maxPlayers);
        roomObj["started"] = room.started;

        // List players with more details
        QJsonArray playersArray;
        for (auto* player : room.players)
        {
            if (player)
            {
                QJsonObject playerObj;
                playerObj["playerId"] = player->playerId;
                playerObj["name"] = player->name;
                playerObj["slotIndex"] = player->slotIndex;
                playersArray.append(playerObj);
            }
        }
        roomObj["players"] = playersArray;

        roomsArray.append(roomObj);
    }

    qDebug() << "SocketIOServer: Sending" << roomsArray.size() << "rooms to client";
    
    QJsonObject response;
    response["rooms"] = roomsArray;
    emitToClient(client->id, "rooms-list", response);
}

void SocketIOServer::handle_ChatMessage(QWebSocket* socket, const QJsonObject& msg)
{
    ClientConnection* client = getClientFromSocket(socket);
    if (!client)
        return;

    // Get the message text
    QString message = msg.value("message").toString();
    if (message.isEmpty())
        return;

    // Broadcast to all players in the same room
    if (!client->roomId.isEmpty())
    {
        SignalingRoom* room = getRoomById(client->roomId);
        if (room)
        {
            // Build the chat message payload
            QJsonObject chatPayload;
            chatPayload["playerName"] = client->name;
            chatPayload["message"] = message;

            // Send to all players in room
            for (auto* player : room->players)
            {
                if (player && player->socket)
                {
                    emitToClient(player->id, "chat-message", chatPayload);
                }
            }

            qDebug() << "Chat relayed from" << client->name << ":" << message;
            emit chatMessageReceived(client->roomId, client->name, message);
        }
    }
}

void SocketIOServer::handle_ControllerInput(QWebSocket* socket, const QJsonObject& msg)
{
    ClientConnection* client = getClientFromSocket(socket);
    if (!client || client->roomId.isEmpty() || client->slotIndex < 0)
        return;

    uint32_t frameNumber = static_cast<uint32_t>(msg.value("frame").toInteger());
    uint32_t controllerState = static_cast<uint32_t>(msg.value("input").toInteger());
    emit controllerInputReceived(client->roomId, client->slotIndex, frameNumber, controllerState);
    broadcastControllerInput(client->roomId, client->slotIndex, frameNumber, controllerState);
}

void SocketIOServer::handle_FrameSync(QWebSocket* socket, const QJsonObject& msg)
{
    ClientConnection* client = getClientFromSocket(socket);
    if (!client || client->roomId.isEmpty() || client->slotIndex < 0)
        return;

    SignalingRoom* room = getRoomById(client->roomId);
    if (!room)
        return;

    const uint32_t frameNumber = static_cast<uint32_t>(msg.value("frame").toInteger());
    const uint32_t stateHash = static_cast<uint32_t>(msg.value("hash").toInteger());
    if (stateHash == 0) {
        return;
    }

    if (room->lastFrameSyncBySlot.value(client->slotIndex) == frameNumber) {
        return;
    }

    room->lastFrameSyncBySlot[client->slotIndex] = frameNumber;
    emit frameSyncReceived(client->roomId, client->slotIndex, frameNumber, stateHash);
    broadcastFrameSync(client->roomId, client->slotIndex, frameNumber, stateHash);
}

SocketIOServer::ClientConnection* SocketIOServer::getClientFromSocket(QWebSocket* socket)
{
    auto it = m_clients.find(socket);
    if (it != m_clients.end())
        return it.value();
    return nullptr;
}

SocketIOServer::ClientConnection* SocketIOServer::getClientById(const QString& clientId)
{
    auto it = m_clientsById.find(clientId);
    if (it != m_clientsById.end())
        return it.value();
    return nullptr;
}

SocketIOServer::SignalingRoom* SocketIOServer::getRoomById(const QString& roomId)
{
    auto it = m_rooms.find(roomId);
    if (it != m_rooms.end())
        return &it.value();
    return nullptr;
}

QString SocketIOServer::generateRoomId()
{
    // Simple 6-character room code (e.g., "ABC123")
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    QString id;
    for (int i = 0; i < 6; i++)
    {
        id += chars[rand() % 36];
    }
    return id;
}

QString SocketIOServer::generateClientId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
}

void SocketIOServer::sendSocketIOMessage(QWebSocket* socket, int type, const QJsonArray& args)
{
    if (!socket || !socket->isValid())
        return;

    QJsonArray message;
    message.append(type);
    for (const auto& arg : args) {
        message.append(arg);
    }

    QJsonDocument doc(message);
    QString payload = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));

    // Engine.IO MESSAGE type is '2'
    socket->sendTextMessage("2" + payload);
}


void SocketIOServer::emitToRoom(const QString& roomId, const QString& eventName, const QJsonObject& data)
{
    SignalingRoom* room = getRoomById(roomId);
    if (!room)
        return;

    for (auto* player : room->players)
    {
        if (player && player->socket && player->socket->isValid())
        {
            emitToClient(player->id, eventName, data);
        }
    }
}

void SocketIOServer::emitToConnectedRoomClients(const QString& roomId, const QString& eventName, const QJsonObject& data)
{
    SignalingRoom* room = getRoomById(roomId);
    if (!room)
        return;

    for (auto it = room->players.constBegin(); it != room->players.constEnd(); ++it)
    {
        ClientConnection* player = it.value();
        if (player && player->socket && player->socket->isValid())
        {
            emitToClient(player->id, eventName, data);
        }
    }
}

void SocketIOServer::broadcastRoomUpdate(const QString& roomId)
{
    SignalingRoom* room = getRoomById(roomId);
    if (!room)
        return;

    QJsonArray playersArray;
    for (auto* player : room->lobbyOrder)
    {
        if (player)
        {
            QJsonObject playerObj;
            playerObj["playerId"] = player->playerId;
            playerObj["id"] = player->playerId;
            playerObj["name"] = player->name;
            playerObj["slotIndex"] = player->slotIndex;
            playerObj["slot"] = player->slotIndex;
            playerObj["clientId"] = player->id;
            playersArray.append(playerObj);
        }
    }

    QJsonObject update;
    update["players"] = playersArray;
    update["playerCount"] = room->lobbyOrder.size();
    update["started"] = room->started;

    emit roomPlayersUpdated(roomId, playersArray);
    emitToRoom(roomId, "users-updated", update);
}

void SocketIOServer::onPingTimer()
{
    for (auto it = m_clients.constBegin(); it != m_clients.constEnd(); ++it) {
        QWebSocket* socket = it.key();
        if (socket && socket->state() == QAbstractSocket::ConnectedState) {
            socket->ping();
        }
    }

    QTimer::singleShot(750, this, [this]() {
        for (const QString& roomId : m_rooms.keys()) {
            broadcastRoomPlayerPings(roomId);
        }
    });
}

void SocketIOServer::broadcastRoomPlayerPings(const QString& roomId)
{
    SignalingRoom* room = getRoomById(roomId);
    if (!room) {
        return;
    }

    QJsonArray pings;
    {
        QJsonObject hostPing;
        hostPing.insert(QStringLiteral("slot"), 0);
        hostPing.insert(QStringLiteral("ms"), 0);
        pings.append(hostPing);
    }

    for (auto* player : room->lobbyOrder) {
        if (!player || player->slotIndex <= 0) {
            continue;
        }

        QJsonObject entry;
        entry.insert(QStringLiteral("slot"), player->slotIndex);
        entry.insert(QStringLiteral("ms"), player->lastPingMs);
        pings.append(entry);
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("pings"), pings);
    emitToRoom(roomId, QStringLiteral("player-pings"), payload);
    emit playerPingsUpdated(roomId, pings);
}

void SocketIOServer::emitToClient(const QString& clientId, const QString& eventName, const QJsonObject& data)
{
    ClientConnection* client = getClientById(clientId);
    if (!client || !client->socket) return;

    QJsonArray args;
    args.append(eventName);
    args.append(data);
    sendSocketIOMessage(client->socket, 2, args);
}

void SocketIOServer::emitToClient(const QString& clientId, const QString& eventName, const QJsonArray& data)
{
    ClientConnection* client = getClientById(clientId);
    if (!client || !client->socket) return;

    QJsonArray args;
    args.append(eventName);
    args.append(data);
    sendSocketIOMessage(client->socket, 2, args);
}