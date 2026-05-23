/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "NetplayCoordinator.hpp"
#include "NatTraversal/NatTraversalProtocol.hpp"
#include "Netplay.hpp"
#include <RMG-Core/Netplay.hpp>
#include <algorithm>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QUuid>

using namespace UserInterface::Netplay;
using namespace RMGCore;

NetplayCoordinator::NetplayCoordinator(const QString& serverUrl, QObject* parent)
    : QObject(parent)
    , m_state(Idle)
    , m_playerId(QUuid::createUuid().toString(QUuid::WithoutBraces))
    , m_shouldAutoJoinRoom(false)
{
    // Create Socket.IO client
    m_socketIO = std::make_unique<SocketIOClient>(serverUrl, this);

    // Connect Socket.IO signals
    connect(m_socketIO.get(), &SocketIOClient::connected,
            this, &NetplayCoordinator::on_socketIO_connected);
    connect(m_socketIO.get(), &SocketIOClient::disconnected,
            this, &NetplayCoordinator::on_socketIO_disconnected);
    connect(m_socketIO.get(), &SocketIOClient::connectionError,
            this, &NetplayCoordinator::on_socketIO_connectionError);

    connect(m_socketIO.get(), &SocketIOClient::roomCreated,
            this, &NetplayCoordinator::on_socketIO_roomCreated);
    connect(m_socketIO.get(), &SocketIOClient::roomJoined,
            this, &NetplayCoordinator::on_socketIO_roomJoined);
    connect(m_socketIO.get(), &SocketIOClient::roomLeft,
            this, &NetplayCoordinator::on_socketIO_roomLeft);
    connect(m_socketIO.get(), &SocketIOClient::roomClosed,
            this, &NetplayCoordinator::on_socketIO_roomClosed);
    connect(m_socketIO.get(), &SocketIOClient::playersUpdated,
            this, &NetplayCoordinator::on_socketIO_playersUpdated);

    connect(m_socketIO.get(), &SocketIOClient::gameStarted,
            this, &NetplayCoordinator::on_socketIO_gameStarted);
    connect(m_socketIO.get(), &SocketIOClient::gameEnded,
            this, &NetplayCoordinator::on_socketIO_gameEnded);
        connect(m_socketIO.get(), &SocketIOClient::controllerInputReceived,
            this, &NetplayCoordinator::on_socketIO_controllerInputReceived);

    connect(m_socketIO.get(), &SocketIOClient::offerReceived,
            this, &NetplayCoordinator::on_socketIO_offerReceived);
    connect(m_socketIO.get(), &SocketIOClient::answerReceived,
            this, &NetplayCoordinator::on_socketIO_answerReceived);
    connect(m_socketIO.get(), &SocketIOClient::iceCandidateReceived,
            this, &NetplayCoordinator::on_socketIO_iceCandidateReceived);

    connect(m_socketIO.get(), &SocketIOClient::chatMessageReceived,
            this, &NetplayCoordinator::chatMessageReceived);
    connect(m_socketIO.get(), &SocketIOClient::cheatsUpdated,
            this, &NetplayCoordinator::on_socketIO_cheatsUpdated);
        connect(m_socketIO.get(), &SocketIOClient::saveSyncReceived,
            this, &NetplayCoordinator::on_socketIO_saveSyncReceived);
    connect(m_socketIO.get(), &SocketIOClient::roomsListed,
            this, &NetplayCoordinator::on_socketIO_roomsListed);

    // Initialize lockstep config
    m_lockstepConfig.numPlayers = 4;
    m_lockstepConfig.localPlayerSlot = 0;
    m_lockstepConfig.inputDelayFrames = 4;
    m_lockstepConfig.resyncEnabled = false;
    m_lockstepConfig.resyncCheckIntervalFrames = 30;
    m_lockstepConfig.stallTimeoutMilliseconds = 1000;

    qDebug() << "NetplayCoordinator created";
    UserInterface::Netplay::g_netplayCoordinator = this;
}

NetplayCoordinator::~NetplayCoordinator()
{
    if (UserInterface::Netplay::g_netplayCoordinator == this)
    {
        UserInterface::Netplay::g_netplayCoordinator = nullptr;
    }

    stopHosting();
    
    if (m_socketIO && m_socketIO->getConnectionState() == SocketIOClient::Connected) {
        m_socketIO->disconnect();
    }
}

bool NetplayCoordinator::startHosting(int port, const QString& playerName, const QString& gameName)
{
    if (m_server != nullptr)
    {
        qWarning() << "Already hosting a server";
        return false;
    }

    m_playerName = playerName;

    // Create and start signaling server
    m_server = std::make_unique<SocketIOServer>(this);

    if (!m_server->startServer(port))
    {
        qWarning() << "Failed to start hosting server on port" << port;
        m_server.reset();
        return false;
    }

    // Connect to server signals
    connect(m_server.get(), &SocketIOServer::roomCreated,
            this, [this](const QString& roomId) {
                qInfo() << "Hosting: Room created" << roomId;
                setState(InLobby);
                emit roomCreated(roomId, 0);  // Host is always slot 0
            });

    connect(m_server.get(), &SocketIOServer::playerJoined,
            this, [this](const QString& roomId, const QString& playerId, int slotIndex) {
                qInfo() << "Hosting: Player joined room" << roomId << "slot" << slotIndex;
                // Trigger peer connection setup when players join
            });

    connect(m_server.get(), &SocketIOServer::gameStarted,
            this, [this](const QString& roomId) {
                qInfo() << "Hosting: Game started in room" << roomId;

                int totalPlayers = 1;
                if (this->isHostingServer() && m_server) {
                    totalPlayers = std::max(totalPlayers, m_server->getConnectedClientCount() + 1);
                }
                totalPlayers = std::max(totalPlayers, static_cast<int>(m_cachedPlayers.size()));
                if (totalPlayers > 4) {
                    totalPlayers = 4;
                }

                if (totalPlayers < 2) {
                    qWarning() << "NetplayCoordinator: Refusing to start host netplay with fewer than 2 players";
                    setState(InLobby);
                    return;
                }

                m_gameSession.numPlayers = totalPlayers;
                m_lockstepConfig.numPlayers = totalPlayers;
                m_lockstepConfig.localPlayerSlot = 0;

                setupPeerConnections(this->m_cachedPlayers);
                initializeLockstepEngine();
                setState(InGame);
                emit gameStarted(m_gameSession);
            });

    connect(m_server.get(), &SocketIOServer::roomPlayersUpdated,
            this, [this](const QString& roomId, const QJsonArray& playersArray) {
                if (roomId != m_gameSession.roomId)
                    return;

                QList<SocketIOClient::PlayerInfo> players;
                for (const auto& value : playersArray)
                {
                    QJsonObject obj = value.toObject();
                    SocketIOClient::PlayerInfo p;
                    p.id = obj["playerId"].toString();
                    p.name = obj["name"].toString();
                    p.slot = obj["slotIndex"].toInt(obj["slot"].toInt(-1));
                    p.isSpectator = false;
                    p.isReady = true;
                    players.append(p);
                }

                m_cachedPlayers = players;
                emit playersUpdated(players);
            });

    connect(m_server.get(), &SocketIOServer::controllerInputReceived,
        this, [this](const QString& roomId, int slot, uint32_t frameNumber, uint32_t controllerState) {
            if (roomId != m_gameSession.roomId || m_state != InGame || !m_lockstepEngine)
                return;

            m_lockstepEngine->submitRemoteInput(slot, frameNumber, controllerState);
        });

    connect(m_server.get(), &SocketIOServer::chatMessageReceived,
            this, [this](const QString& roomId, const QString& playerName, const QString& message) {
                if (roomId != m_gameSession.roomId)
                    return;
                emit chatMessageReceived(playerName, message);
            });

    // Create initial room for remote players to join
    m_gameSession.roomId = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8).toUpper();
    m_gameSession.localSlot = 0;
    m_gameSession.numPlayers = 1;
    m_lockstepConfig.localPlayerSlot = 0;
    m_lockstepConfig.numPlayers = 1;

    // Hosting is active as soon as server is up; room-created callback moves us to InLobby.
    setState(Connected);

    // Pre-create the room on the server so remote players can join it
    m_server->createInitialRoom(m_gameSession.roomId, playerName, gameName);

    qInfo() << "Hosting signaling server on port" << port;

    // For hosting, emit roomJoined directly (host doesn't need to connect as Socket.IO client)
    // This allows the session dialog to open immediately
    emit roomJoined(m_gameSession.roomId, 0);

    return true;
}

void NetplayCoordinator::stopHosting()
{
    if (m_server)
    {
        m_server->stopServer();
        m_server.reset();
        qInfo() << "Stopped hosting signaling server";
    }
}

bool NetplayCoordinator::isHostingServer() const
{
    return m_server != nullptr && m_server->isRunning();
}

void NetplayCoordinator::connectToServer(const QString& playerName)
{
    m_playerName = playerName;
    setState(Connecting);
    m_socketIO->connectToServer(playerName);
}

void NetplayCoordinator::connectToDirectIPServer(const QString& ipAddress, int port, const QString& playerName,
                                               const QString& roomId)
{
    m_playerName = playerName;
    m_shouldAutoJoinRoom = true;
    m_autoJoinRoomId = roomId.trimmed();
    
    // Recreate the Socket.IO client with the new server URL
    QString serverUrl = QString("http://%1:%2").arg(ipAddress).arg(port);
    
    // Disconnect existing client if connected
    if (m_socketIO && m_socketIO->getConnectionState() != SocketIOClient::Disconnected)
    {
        m_socketIO->disconnect();
    }
    
    // Create new client with the direct IP server URL
    m_socketIO = std::make_unique<SocketIOClient>(serverUrl, this);
    
    // Reconnect all signals
    connect(m_socketIO.get(), &SocketIOClient::connected,
            this, &NetplayCoordinator::on_socketIO_connected);
    connect(m_socketIO.get(), &SocketIOClient::disconnected,
            this, &NetplayCoordinator::on_socketIO_disconnected);
    connect(m_socketIO.get(), &SocketIOClient::connectionError,
            this, &NetplayCoordinator::on_socketIO_connectionError);

    connect(m_socketIO.get(), &SocketIOClient::roomCreated,
            this, &NetplayCoordinator::on_socketIO_roomCreated);
    connect(m_socketIO.get(), &SocketIOClient::roomJoined,
            this, &NetplayCoordinator::on_socketIO_roomJoined);
    connect(m_socketIO.get(), &SocketIOClient::roomLeft,
            this, &NetplayCoordinator::on_socketIO_roomLeft);
    connect(m_socketIO.get(), &SocketIOClient::roomClosed,
            this, &NetplayCoordinator::on_socketIO_roomClosed);
    connect(m_socketIO.get(), &SocketIOClient::playersUpdated,
            this, &NetplayCoordinator::on_socketIO_playersUpdated);
    connect(m_socketIO.get(), &SocketIOClient::roomsListed,
            this, &NetplayCoordinator::on_socketIO_roomsListed);

    connect(m_socketIO.get(), &SocketIOClient::gameStarted,
            this, &NetplayCoordinator::on_socketIO_gameStarted);
    connect(m_socketIO.get(), &SocketIOClient::gameEnded,
            this, &NetplayCoordinator::on_socketIO_gameEnded);
        connect(m_socketIO.get(), &SocketIOClient::controllerInputReceived,
            this, &NetplayCoordinator::on_socketIO_controllerInputReceived);

    connect(m_socketIO.get(), &SocketIOClient::offerReceived,
            this, &NetplayCoordinator::on_socketIO_offerReceived);
    connect(m_socketIO.get(), &SocketIOClient::answerReceived,
            this, &NetplayCoordinator::on_socketIO_answerReceived);
    connect(m_socketIO.get(), &SocketIOClient::iceCandidateReceived,
            this, &NetplayCoordinator::on_socketIO_iceCandidateReceived);

    connect(m_socketIO.get(), &SocketIOClient::chatMessageReceived,
            this, &NetplayCoordinator::chatMessageReceived);
    connect(m_socketIO.get(), &SocketIOClient::cheatsUpdated,
            this, &NetplayCoordinator::on_socketIO_cheatsUpdated);
        connect(m_socketIO.get(), &SocketIOClient::saveSyncReceived,
            this, &NetplayCoordinator::on_socketIO_saveSyncReceived);
    
    // Now connect to the server
    setState(Connecting);
    m_socketIO->connectToServer(playerName);
}

void NetplayCoordinator::createRoom(const QString& roomName, const QString& gameId, int maxPlayers)
{
    if (m_state != Connected) {
        qWarning() << "NetplayCoordinator: Cannot create room - not connected";
        return;
    }

    setState(CreatingRoom);
    m_socketIO->openRoom(roomName, gameId, maxPlayers);
}

void NetplayCoordinator::joinRoom(const QString& roomId, bool asSpectator, const QString& password)
{
    if (m_state != Connected) {
        qWarning() << "NetplayCoordinator: Cannot join room - not connected";
        return;
    }

    setState(JoiningRoom);
    m_socketIO->joinRoom(roomId, asSpectator);
}

void NetplayCoordinator::leaveRoom()
{
    if (m_state != InLobby && m_state != InGame) {
        return;
    }

    m_socketIO->leaveRoom();
    m_peers.clear();
    setState(Connected);
}

void NetplayCoordinator::startGame(const QString& gameMode, bool resyncEnabled, const QString& romHash)
{
    if (isHostingServer()) {
        if (m_state != InLobby && m_state != Connected && m_state != StartingGame) {
            qWarning() << "NetplayCoordinator: Cannot start hosted game in state" << getCurrentStateString();
            return;
        }

        if (m_gameSession.roomId.isEmpty()) {
            qWarning() << "NetplayCoordinator: Cannot start hosted game - missing room id";
            return;
        }

        m_gameSession.gameMode = gameMode;
        m_gameSession.resyncEnabled = resyncEnabled;
        m_gameSession.romHash = romHash;

        const int activePlayers = std::max(1, m_server ? (m_server->getConnectedClientCount() + 1) : 1);
        if (activePlayers < 2) {
            qWarning() << "NetplayCoordinator: Need at least 2 players to start hosted netplay, have" << activePlayers;
            setState(InLobby);
            return;
        }

        setState(StartingGame);
        if (!m_server->startHostedGame(m_gameSession.roomId, gameMode, resyncEnabled, romHash)) {
            qWarning() << "NetplayCoordinator: Failed to start hosted game";
            setState(InLobby);
        }
        return;
    }

    if (m_state != InLobby) {
        qWarning() << "NetplayCoordinator: Cannot start game - not in lobby";
        return;
    }

    setState(StartingGame);
    m_socketIO->startGame(gameMode, resyncEnabled, romHash);
}

void NetplayCoordinator::endGame()
{
    if (m_state != InGame) {
        return;
    }

    setState(EndingGame);
    m_socketIO->endGame();
}

void NetplayCoordinator::submitFrameInput(uint32_t controllerState)
{
    if (m_state != InGame || !m_lockstepEngine) return;

    uint32_t frameNumber = m_lockstepEngine->getCurrentFrameNumber();

    if (isHostingServer()) {
        // P1 (Host) sends their input to the server to be broadcast to P2
        QMetaObject::invokeMethod(m_server.get(), [this, frameNumber, controllerState](){
            m_server->broadcastControllerInput(m_gameSession.roomId, m_lockstepConfig.localPlayerSlot, frameNumber, controllerState);
        }, Qt::QueuedConnection);
    } else if (m_socketIO) {
        // Guests send input to host via Socket.IO transport.
        QMetaObject::invokeMethod(m_socketIO.get(), [this, frameNumber, controllerState](){
            m_socketIO->sendControllerInput(frameNumber, controllerState);
        }, Qt::QueuedConnection);
    }

    m_lockstepEngine->submitLocalInput(controllerState);
}

bool NetplayCoordinator::advanceFrame()
{
    if (m_state != InGame || !m_lockstepEngine) {
        return false;
    }

    return m_lockstepEngine->advanceFrame();
}

void NetplayCoordinator::onDesyncDetected(const QString& reason)
{
    emit desyncDetected(reason);

    if (m_lockstepEngine && m_lockstepEngine->isDesynchronized()) {
        emit resyncAttempted();
        // Actual resync would be handled at a higher level (Emulation.cpp)
    }
}

void NetplayCoordinator::verifyGameSync(uint32_t romChecksum)
{
    if (m_state == InGame && m_lockstepEngine) {
        m_lockstepEngine->checkDesync(romChecksum);
    }
}

NetplayCoordinator::State NetplayCoordinator::getCurrentState() const
{
    return m_state;
}

QString NetplayCoordinator::getCurrentStateString() const
{
    switch (m_state) {
    case Idle: return "Idle";
    case Connecting: return "Connecting";
    case Connected: return "Connected";
    case CreatingRoom: return "CreatingRoom";
    case JoiningRoom: return "JoiningRoom";
    case InLobby: return "InLobby";
    case StartingGame: return "StartingGame";
    case InGame: return "InGame";
    case EndingGame: return "EndingGame";
    case Error: return "Error";
    default: return "Unknown";
    }
}

NetplayCoordinator::GameSession NetplayCoordinator::getGameSession() const
{
    return m_gameSession;
}

QList<SocketIOClient::PlayerInfo> NetplayCoordinator::getPlayerList() const
{
    if (!m_cachedPlayers.isEmpty()) {
        return m_cachedPlayers;
    }
    if (m_socketIO) {
        return m_socketIO->getPlayerList();
    }
    return QList<SocketIOClient::PlayerInfo>();
}

bool NetplayCoordinator::isHost() const
{
    // Owner is determined by who initiated room creation
    // This could be tracked in m_gameSession if needed
    return m_gameSession.localSlot == 0;
}

bool NetplayCoordinator::isInGame() const
{
    return m_state == InGame;
}

//
// Private Slots - Socket.IO
//

void NetplayCoordinator::on_socketIO_connected()
{
    qDebug() << "NetplayCoordinator: Socket.IO connected";
    setState(Connected);

    // Set player name now that we're connected
    if (!m_playerName.isEmpty()) {
        m_socketIO->setPlayerName(m_playerName);
    }

    // If we should auto-join (direct IP connection), request room list
    if (m_shouldAutoJoinRoom)
    {
        qDebug() << "NetplayCoordinator: Auto-joining room for direct connection";
        m_socketIO->requestRoomList(true);
    }

    emit connected();
}

void NetplayCoordinator::on_socketIO_disconnected()
{
    qDebug() << "NetplayCoordinator: Socket.IO disconnected";
    
    // Kill all engine slots so it doesn't stay stuck in advanceFrame()
    if (m_lockstepEngine) {
        for (int i = 0; i < 4; ++i) {
            m_lockstepEngine->setDataChannel(i, nullptr);
        }
    }
    
    m_peers.clear();
    m_cachedPlayers.clear();
    setState(Idle);
    emit disconnected();
}

void NetplayCoordinator::on_socketIO_connectionError(const QString& error)
{
    qWarning() << "NetplayCoordinator: Socket.IO error:" << error;
    setState(Error);
    emit connectionError(error);
}

void NetplayCoordinator::on_socketIO_roomCreated(const QString& roomId)
{
    qDebug() << "NetplayCoordinator: Room created" << roomId;
    m_gameSession.roomId = roomId;
    m_gameSession.localSlot = 0;
    m_lockstepConfig.localPlayerSlot = 0;
    setState(InLobby);
    emit roomCreated(roomId, 0);
}

void NetplayCoordinator::on_socketIO_roomJoined(const QString& roomId)
{
    qDebug() << "NetplayCoordinator: Room joined" << roomId;
    m_gameSession.roomId = roomId;
    // Slot will be set by on_socketIO_playersUpdated
    setState(InLobby);
    emit roomJoined(roomId, m_gameSession.localSlot);
}

void NetplayCoordinator::on_socketIO_roomLeft()
{
    qDebug() << "NetplayCoordinator: Room left";
    m_peers.clear();
    m_cachedPlayers.clear();
    m_gameSession = GameSession();
    setState(Connected);
    emit roomClosed("left");
}

void NetplayCoordinator::on_socketIO_roomClosed(const QString& reason)
{
    qDebug() << "NetplayCoordinator: Room closed:" << reason;
    m_peers.clear();
    m_cachedPlayers.clear();
    m_gameSession = GameSession();
    setState(Connected);
    emit roomClosed(reason);
}

void NetplayCoordinator::on_socketIO_playersUpdated(const QList<SocketIOClient::PlayerInfo>& players)
{
    m_cachedPlayers = players;
    int count = players.size();

    // Update the config snapshot
    m_lockstepConfig.numPlayers = count;

    // CRITICAL: Tell the LIVE engine to update its requirements
    if (m_lockstepEngine) {
        m_lockstepEngine->setNumPlayers(count);
    }

    for (const auto& player : players) {
        if (player.id == m_socketIO->getPlayerId()) {
            m_gameSession.localSlot = player.slot;
            m_lockstepConfig.localPlayerSlot = player.slot;
            
            // Tell the engine who WE are
            if (m_lockstepEngine) {
                m_lockstepEngine->setLocalPlayerSlot(player.slot);
            }
            break;
        }
    }

    emit playersUpdated(players);
}

void NetplayCoordinator::on_socketIO_roomsListed(const QJsonArray& rooms)
{
    qDebug() << "NetplayCoordinator: Received rooms list, count:" << rooms.size();

    if (!m_shouldAutoJoinRoom) {
        return;
    }

    QString roomId;
    if (rooms.size() > 0) {
        const QJsonObject room = rooms[0].toObject();
        roomId = room.value(QStringLiteral("roomId")).toString();
        m_autoJoinRoomData = room;
    } else if (!m_autoJoinRoomId.isEmpty()) {
        roomId = m_autoJoinRoomId;
        qDebug() << "NetplayCoordinator: Room list empty, joining room from session index:" << roomId;
    }

    m_shouldAutoJoinRoom = false;
    m_autoJoinRoomId.clear();

    if (roomId.isEmpty()) {
        qWarning() << "NetplayCoordinator: No joinable room found on host";
        setState(Error);
        emit connectionError(QStringLiteral("No joinable room found on host"));
        return;
    }

    qDebug() << "NetplayCoordinator: Auto-joining room:" << roomId;
    m_socketIO->joinRoom(roomId, false);
}

void NetplayCoordinator::on_socketIO_gameStarted(const QString& mode, bool resync, const QString& matchId)
{
    // Update the config with the actual assigned slot from the server
    m_lockstepConfig.localPlayerSlot = m_gameSession.localSlot;
    m_lockstepConfig.numPlayers = m_cachedPlayers.size();

    initializeLockstepEngine();
    
    // Tell the Core which slot WE are
    CoreSetEmbeddedNetplayState(true, m_gameSession.localSlot);

    setState(InGame);
    emit gameStarted(m_gameSession);
}

void NetplayCoordinator::on_socketIO_gameEnded()
{
    qDebug() << "NetplayCoordinator: Game ended";
    m_lockstepEngine.reset();
    setState(InLobby);
    emit gameEnded();
}

void NetplayCoordinator::on_socketIO_offerReceived(const QString& fromPlayerId, const QString& sdpOffer)
{
    qDebug() << "NetplayCoordinator: Offer received from" << fromPlayerId;

    // Find which slot this player is in
    int slotIndex = -1;
    for (const auto& player : m_socketIO->getPlayerList()) {
        if (player.id == fromPlayerId) {
            slotIndex = player.slot;
            break;
        }
    }

    if (slotIndex < 0) {
        qWarning() << "NetplayCoordinator: Could not find slot for player" << fromPlayerId;
        return;
    }

    // Create peer if not exists
    if (!m_peers.contains(slotIndex)) {
        auto peer = std::make_shared<WebRTCPeer>(fromPlayerId, false, this);
        m_peers[slotIndex] = peer;

        // Connect WebRTC peer signals
        connect(peer.get(), &WebRTCPeer::offerCreated, this, [this, id = fromPlayerId](const QString& offer) {
            m_socketIO->sendOffer(id, offer);
        });
        connect(peer.get(), &WebRTCPeer::iceCandidateGenerated, this, [this, id = fromPlayerId](const QString& candidate, int mLineIndex) {
            m_socketIO->sendICECandidate(id, candidate, mLineIndex);
        });
        connect(peer.get(), &WebRTCPeer::connectionEstablished, this, [this, id = fromPlayerId]() {
            on_webRTC_connectionEstablished(id);
        });
        connect(peer.get(), &WebRTCPeer::connectionFailed, this, [this, id = fromPlayerId](const QString& reason) {
            on_webRTC_connectionFailed(id, reason);
        });
        connect(peer.get(), &WebRTCPeer::dataChannelOpened, this, [this, id = fromPlayerId](const QString& label) {
            on_webRTC_dataChannelOpened(id, label);
        });
    }

    // Set remote description
    auto peer = m_peers[slotIndex];
    if (peer) {
        peer->setRemoteDescription(sdpOffer);
    }
}

void NetplayCoordinator::on_socketIO_answerReceived(const QString& fromPlayerId, const QString& sdpAnswer)
{
    qDebug() << "NetplayCoordinator: Answer received from" << fromPlayerId;

    // Find peer and set remote description
    for (int slot = 0; slot < 4; ++slot) {
        if (m_peers.contains(slot)) {
            auto peer = m_peers[slot];
            if (peer && peer->getPeerId() == fromPlayerId) {
                peer->setRemoteDescription(sdpAnswer);
                break;
            }
        }
    }
}
void NetplayCoordinator::initializeLockstepEngine()
{
    if (m_lockstepEngine) {
        return;
    }

    m_lockstepEngine = std::make_unique<RMGCore::LockstepEngine>(m_lockstepConfig);

    RMGCore::LockstepEngine::Callbacks callbacks;
    callbacks.frameReady = [this](uint32_t frameNumber, const std::map<int, uint32_t>& inputs) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        
        // 1. Store the inputs in our local map so the Core can "Pull" them later
        m_currentFrameInputs = inputs;

        // 2. Notify the UI
        QMap<int, uint32_t> qtInputs;
        for (const auto& [slot, input] : inputs) {
            qtInputs[slot] = input;
        }
        emit gameFrameReady(frameNumber, qtInputs);
    };

    callbacks.peerInputStalled = [this](int playerSlot, uint32_t frameNumber) {
        emit peerInputStalled(playerSlot, frameNumber);
    };

    callbacks.desyncDetected = [this](uint32_t frameNumber, const std::string& reason) {
        emit desyncDetected(QString::fromStdString(reason));
    };

    m_lockstepEngine->setCallbacks(callbacks);

    // --- THE BRIDGE: Connect Coordinator to Emulator Core ---
    // This tells the Emulator: "When you need input or need to advance, call these!"
    CoreSetEmbeddedNetplayCallbacks(
        [](uint32_t state) { 
            if (UserInterface::Netplay::g_netplayCoordinator) 
                UserInterface::Netplay::g_netplayCoordinator->submitFrameInput(state); 
        },
        [](int slot) { 
            if (UserInterface::Netplay::g_netplayCoordinator) 
                return UserInterface::Netplay::g_netplayCoordinator->getSyncedInput(slot);
            return (uint32_t)0;
        },
        []() { 
            if (UserInterface::Netplay::g_netplayCoordinator) 
                return UserInterface::Netplay::g_netplayCoordinator->advanceFrame();
            return false;
        }
    );
}

void NetplayCoordinator::on_socketIO_iceCandidateReceived(const QString& fromPlayerId, const QString& candidate, int mLineIndex)
{
    // Use a more efficient iterator-based lookup
    auto it = m_peers.begin();
    while (it != m_peers.end()) {
        auto peer = it.value();
        
        // Check if this is the peer we are looking for and it still exists
        if (peer && peer->getPeerId() == fromPlayerId) {
            // Only add the candidate if the peer connection is actually active
            // This prevents adding candidates to a peer that just failed/closed
            peer->addICECandidate(candidate, mLineIndex);
            return; // Exit once found
        }
        ++it;
    }

    qDebug() << "NetplayCoordinator: Ignored ICE candidate for unknown or dropped peer:" << fromPlayerId;
}

void NetplayCoordinator::on_webRTC_connectionEstablished(const QString& peerId)
{
    qDebug() << "NetplayCoordinator: WebRTC connection established with peer" << peerId;

    // Find which slot this peer is in
    int peerSlot = -1;
    for (int slot = 0; slot < 4; ++slot) {
        if (m_peers.contains(slot)) {
            auto peer = m_peers[slot];
            if (peer && peer->getPeerId() == peerId) {
                peerSlot = slot;
                break;
            }
        }
    }

    if (peerSlot >= 0) {
        emit peerConnected(peerSlot);
    }
}

void NetplayCoordinator::on_webRTC_connectionFailed(const QString& peerId, const QString& reason)
{
    qWarning() << "NetplayCoordinator: WebRTC connection failed for peer" << peerId << ":" << reason;

    int peerSlot = -1;
    for (int slot = 0; slot < 4; ++slot) {
        if (m_peers.contains(slot)) {
            auto peer = m_peers[slot];
            if (peer && peer->getPeerId() == peerId) {
                peerSlot = slot;
                
                // CRITICAL: Tell the engine this slot is DEAD
                if (m_lockstepEngine) {
                    m_lockstepEngine->setDataChannel(slot, nullptr);
                }
                
                m_peers.remove(slot); 
                break;
            }
        }
    }

    if (peerSlot >= 0) {
        emit peerDisconnected(peerSlot);
    }
}

void NetplayCoordinator::on_webRTC_dataChannelOpened(const QString& peerId, const QString& label)
{
    qDebug() << "NetplayCoordinator: WebRTC data channel opened for peer" << peerId << "label:" << label;

    if (!m_lockstepEngine) {
        qWarning() << "NetplayCoordinator: LockstepEngine not initialized";
        return;
    }

    // Find which slot this peer is in
    int peerSlot = -1;
    for (int slot = 0; slot < 4; ++slot) {
        if (m_peers.contains(slot)) {
            auto peer = m_peers[slot];
            if (peer && peer->getPeerId() == peerId) {
                peerSlot = slot;
                break;
            }
        }
    }

    if (peerSlot >= 0 && m_peers[peerSlot]) {
        // Get the data channel from the peer and register it with lockstep engine
        // This allows the lockstep engine to send input data through this channel
        auto channel = m_peers[peerSlot]->getDataChannel(label);
        if (channel) {
            m_lockstepEngine->setDataChannel(peerSlot, channel);
            qDebug() << "NetplayCoordinator: Data channel" << label << "registered with LockstepEngine for slot" << peerSlot;
        } else {
            qWarning() << "NetplayCoordinator: Failed to get data channel" << label << "for slot" << peerSlot;
        }
    }
}

//
// Compatibility Methods for Dialogs
//

void NetplayCoordinator::requestRoomList()
{
    // Request room list from Socket.IO server
    if (m_socketIO && m_socketIO->getConnectionState() == SocketIOClient::Connected) {
        // Emit roomsUpdated signal to indicate rooms are being queried
        // The actual room list is managed by the browser widget
        emit roomsUpdated();
    }
    qDebug() << "NetplayCoordinator: Requesting room list";
}

void NetplayCoordinator::setPlayerName(const QString& name)
{
    m_playerName = name;
    if (m_socketIO) {
        m_socketIO->setPlayerName(name);
    }
}

void NetplayCoordinator::sendChatMessage(const QString& message)
{
    if (message.trimmed().isEmpty()) {
        return;
    }

    if (isHostingServer()) {
        if (m_server && !m_gameSession.roomId.isEmpty()) {
            m_server->broadcastChatMessage(m_gameSession.roomId, m_playerName, message);
            emit chatMessageReceived(m_playerName, message);
            return;
        }
    }

    // Send chat message via Socket.IO if connected
    if (m_socketIO && m_socketIO->getConnectionState() == SocketIOClient::Connected) {
        m_socketIO->sendChatMessage(message);
    } else {
        qWarning() << "Cannot send chat message: not connected to server";
    }
}

void NetplayCoordinator::sendCheatsUpdate(const QJsonArray& cheats)
{
    if (isHostingServer()) {
        if (m_server && !m_gameSession.roomId.isEmpty()) {
            m_server->broadcastCheatsUpdate(m_gameSession.roomId, cheats);
        }
        return;
    }

    if (m_socketIO && m_socketIO->getConnectionState() == SocketIOClient::Connected) {
        m_socketIO->sendCheatsUpdate(cheats);
    }
}

void NetplayCoordinator::sendSaveSync(const QJsonArray& saveFiles)
{
    if (isHostingServer()) {
        if (m_server && !m_gameSession.roomId.isEmpty()) {
            m_server->broadcastSaveSync(m_gameSession.roomId, saveFiles);
        }
        return;
    }

    if (m_socketIO && m_socketIO->getConnectionState() == SocketIOClient::Connected) {
        m_socketIO->sendSaveSync(saveFiles);
    }
}

void NetplayCoordinator::setInputDelayFrames(int frames)
{
    if (frames < 0) {
        frames = 0;
    } else if (frames > 8) {
        frames = 8;
    }

    m_lockstepConfig.inputDelayFrames = frames;
}

int NetplayCoordinator::getInputDelayFrames() const
{
    return m_lockstepConfig.inputDelayFrames;
}

void NetplayCoordinator::on_socketIO_cheatsUpdated(const QJsonArray& cheats)
{
    emit cheatsUpdated(cheats);
}

void NetplayCoordinator::on_socketIO_saveSyncReceived(const QJsonArray& saveFiles)
{
    emit saveSyncReceived(saveFiles);
}

void NetplayCoordinator::on_socketIO_controllerInputReceived(int slot, uint32_t frameNumber, uint32_t controllerState)
{
    if (m_state != InGame || !m_lockstepEngine) {
        return;
    }

    if (slot == m_gameSession.localSlot) {
        return;
    }

    if (slot >= 0 && slot < m_lockstepConfig.numPlayers) {
        m_lockstepEngine->submitRemoteInput(slot, frameNumber, controllerState);
    } else if (slot == -1 && m_lockstepConfig.numPlayers == 2) {
        // Fallback for 2-player simple sync
        int inferredSlot = (m_gameSession.localSlot == 0) ? 1 : 0;
        m_lockstepEngine->submitRemoteInput(inferredSlot, frameNumber, controllerState);
    }
}


QString NetplayCoordinator::getPeerAddress() const
{
    // Return the address of the peer/server connection
    // For local hosting: return 127.0.0.1 (or the configured host address)
    // For remote connection: return the actual server address
    if (m_server) {
        // If we're hosting, return localhost
        return "127.0.0.1";
    }
    if (m_socketIO) {
        // If we're connecting to a server, could return that address
        // For now, return localhost as default
        return "127.0.0.1";
    }
    return "127.0.0.1";
}

int NetplayCoordinator::getGamePort() const
{
    // Return the port where the game will communicate on
    return kDefaultNetplayHostingPort;
}
// Private Slots - Lockstep
//

void NetplayCoordinator::on_lockstep_frameReady(uint32_t frameNumber, const QMap<int, uint32_t>& inputs)
{
    emit gameFrameReady(frameNumber, inputs);
}

void NetplayCoordinator::on_lockstep_peerStalled(int playerSlot, uint32_t frameNumber)
{
    emit peerInputStalled(playerSlot, frameNumber);
}

void NetplayCoordinator::on_lockstep_desyncDetected(uint32_t frameNumber, const QString& reason)
{
    emit desyncDetected(reason);
}

//
// Private Methods
//

void NetplayCoordinator::setState(State newState)
{
    if (m_state != newState) {
        qDebug() << "NetplayCoordinator: State change" << getCurrentStateString() << "->" << 
                   (newState == Idle ? "Idle" : 
                    newState == Connecting ? "Connecting" :
                    newState == Connected ? "Connected" :
                    newState == CreatingRoom ? "CreatingRoom" :
                    newState == JoiningRoom ? "JoiningRoom" :
                    newState == InLobby ? "InLobby" :
                    newState == StartingGame ? "StartingGame" :
                    newState == InGame ? "InGame" :
                    newState == EndingGame ? "EndingGame" : "Error");
        m_state = newState;
        emit stateChanged(m_state);
    }
}

void NetplayCoordinator::setupPeerConnections(const QList<SocketIOClient::PlayerInfo>& players)
{
    qDebug() << "NetplayCoordinator: Setting up peer connections for" << players.size() << "players";

    for (const auto& player : players) {
        if (player.slot == m_gameSession.localSlot) {
            continue; // Skip self
        }

        // Create WebRTC peer for this player
        bool initiator = m_gameSession.localSlot < player.slot;
        auto peer = std::make_shared<WebRTCPeer>(player.id, initiator, this);
        m_peers[player.slot] = peer;

        // Connect WebRTC peer signals
        connect(peer.get(), &WebRTCPeer::offerCreated, this, [this, id = player.id](const QString& offer) {
            m_socketIO->sendOffer(id, offer);
        });
        connect(peer.get(), &WebRTCPeer::iceCandidateGenerated, this, [this, id = player.id](const QString& candidate, int mLineIndex) {
            m_socketIO->sendICECandidate(id, candidate, mLineIndex);
        });
        connect(peer.get(), &WebRTCPeer::connectionEstablished, this, [this, id = player.id]() {
            on_webRTC_connectionEstablished(id);
        });
        connect(peer.get(), &WebRTCPeer::connectionFailed, this, [this, id = player.id](const QString& reason) {
            on_webRTC_connectionFailed(id, reason);
        });
        connect(peer.get(), &WebRTCPeer::dataChannelOpened, this, [this, id = player.id](const QString& label) {
            on_webRTC_dataChannelOpened(id, label);
        });

        if (initiator) {
            // We create the data channel and the offer
            peer->createDataChannel("RMG-Input");
            createPeerOffer(player.slot);
        }
    }
}

void NetplayCoordinator::createPeerOffer(int slot)
{
    if (!m_peers.contains(slot)) {
        return;
    }

    auto peer = m_peers[slot];
    if (peer) {
        peer->createOffer();
        // TODO: When offer is ready, send via Socket.IO
    }
}

void NetplayCoordinator::handlePeerAnswer(int slot, const QString& answer)
{
    if (!m_peers.contains(slot)) {
        return;
    }

    auto peer = m_peers[slot];
    if (peer) {
        peer->setRemoteDescription(answer);
    }
}

void NetplayCoordinator::addICECandidate(int slot, const QString& candidate, int mLineIndex)
{
    if (!m_peers.contains(slot)) {
        return;
    }

    auto peer = m_peers[slot];
    if (peer) {
        peer->addICECandidate(candidate, mLineIndex);
    }
}

void NetplayCoordinator::sendInputDelayUpdate(int frames) {
    if (!isHost()) return;

    if (m_lockstepEngine) {
        m_lockstepEngine->setInputDelayFrames(frames);
    }

    // Use the new public function we just created instead of emitEvent
    if (m_socketIO) {
        m_socketIO->sendInputDelayUpdate(frames);
    }
}

// In your Socket.IO message handler (on_socketIO_eventReceived):
// When a guest receives "update-input-delay":
void NetplayCoordinator::on_socketIO_inputDelayReceived(int frames) {
    m_lockstepConfig.inputDelayFrames = frames;
    if (m_lockstepEngine) {
        m_lockstepEngine->setInputDelayFrames(frames);
    }
    emit inputDelayChanged(frames);
}

uint32_t NetplayCoordinator::getSyncedInput(int slot) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    // NO OFFSET! The core uses 0-3 just like we do.
    if (slot >= 0 && slot < m_lockstepConfig.numPlayers) {
        if (m_currentFrameInputs.count(slot)) {
            return m_currentFrameInputs[slot];
        }
    }
    
    return 0;
}