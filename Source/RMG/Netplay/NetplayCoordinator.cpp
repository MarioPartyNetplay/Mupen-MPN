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
#include "NetplayProtocol.hpp"
#include "NetplayTraversalLookup.hpp"
#include "Netplay.hpp"
#include <RMG-Core/Netplay.hpp>
#include <RMG-Core/Emulation.hpp>
#include <algorithm>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QThread>
#include <QCoreApplication>
#include <QEventLoop>
#include <QUuid>

using namespace UserInterface::Netplay;
using namespace RMGCore;

namespace {

Qt::ConnectionType socketDispatchConnectionType(const QObject* target)
{
    if (QThread::currentThread() == target->thread()) {
        return Qt::DirectConnection;
    }

    // Emulation runs off the UI thread; block until socket I/O is dispatched so
    // peers receive inputs before this frame's stall timeout expires.
    return Qt::BlockingQueuedConnection;
}

QJsonObject coreSettingsToJson(const CoreNetplaySyncSettings& settings)
{
    QJsonObject payload;
    payload[QStringLiteral("countPerOp")] = settings.countPerOp;
    payload[QStringLiteral("countPerOpDenomPot")] = settings.countPerOpDenomPot;
    payload[QStringLiteral("disableExtraMem")] = settings.disableExtraMem;
    payload[QStringLiteral("siDmaDuration")] = settings.siDmaDuration;
    payload[QStringLiteral("cpuEmulator")] = settings.cpuEmulator;
    return payload;
}

bool coreSettingsFromJson(const QJsonObject& payload, CoreNetplaySyncSettings& settings)
{
    if (!payload.contains(QStringLiteral("countPerOp")) ||
        !payload.contains(QStringLiteral("countPerOpDenomPot")) ||
        !payload.contains(QStringLiteral("disableExtraMem")) ||
        !payload.contains(QStringLiteral("siDmaDuration")) ||
        !payload.contains(QStringLiteral("cpuEmulator")))
    {
        return false;
    }

    settings.countPerOp = payload.value(QStringLiteral("countPerOp")).toInt(0);
    settings.countPerOpDenomPot = payload.value(QStringLiteral("countPerOpDenomPot")).toInt(0);
    settings.disableExtraMem = payload.value(QStringLiteral("disableExtraMem")).toBool(false);
    settings.siDmaDuration = payload.value(QStringLiteral("siDmaDuration")).toInt(-1);
    settings.cpuEmulator = payload.value(QStringLiteral("cpuEmulator")).toInt(2);
    settings.valid = true;
    return true;
}

} // namespace

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
    connect(m_socketIO.get(), &SocketIOClient::frameSyncReceived,
            this, &NetplayCoordinator::on_peerFrameSyncReceived);

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
    connect(m_socketIO.get(), &SocketIOClient::coreSettingsSyncReceived,
            this, &NetplayCoordinator::on_socketIO_coreSettingsSyncReceived);
    connect(m_socketIO.get(), &SocketIOClient::roomsListed,
            this, &NetplayCoordinator::on_socketIO_roomsListed);
    connect(m_socketIO.get(), &SocketIOClient::inputDelayReceived,
            this, &NetplayCoordinator::on_socketIO_inputDelayReceived);
        connect(m_socketIO.get(), &SocketIOClient::emulationPauseReceived,
            this, &NetplayCoordinator::on_socketIO_emulationPauseReceived);
    connect(m_socketIO.get(), &SocketIOClient::emulationBeginReceived,
            this, &NetplayCoordinator::on_socketIO_emulationBeginReceived);

    // Initialize lockstep config
    m_lockstepConfig.numPlayers = 4;
    m_lockstepConfig.localPlayerSlot = 0;
    m_lockstepConfig.inputDelayFrames = 6;
    m_lockstepConfig.desyncDetectionEnabled = true;
    m_lockstepConfig.resyncEnabled = false;
    m_lockstepConfig.resyncCheckIntervalFrames = 30;
    m_lockstepConfig.stallTimeoutMilliseconds = RMGCore::LockstepEngine::stallTimeoutForDelayFrames(6);

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
                Q_UNUSED(playerId);
                synchronizeLockstepPlayerCount();
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
                synchronizeLockstepPlayerCount();
                emit playersUpdated(players);
            });

    connect(m_server.get(), &SocketIOServer::controllerInputReceived,
        this, [this](const QString& roomId, int slot, uint32_t frameNumber, uint32_t controllerState) {
            if (roomId != m_gameSession.roomId || m_state != InGame || !m_lockstepEngine)
                return;

            if (slot == m_gameSession.localSlot) {
                return;
            }

            m_lockstepEngine->submitRemoteInput(slot, frameNumber, controllerState);
        });

    connect(m_server.get(), &SocketIOServer::frameSyncReceived,
        this, [this](const QString& roomId, int slot, uint32_t frameNumber, uint32_t stateHash) {
            if (roomId != m_gameSession.roomId)
                return;
            on_peerFrameSyncReceived(slot, frameNumber, stateHash);
        });

    connect(m_server.get(), &SocketIOServer::chatMessageReceived,
            this, [this](const QString& roomId, const QString& playerName, const QString& message) {
                if (roomId != m_gameSession.roomId)
                    return;
                emit chatMessageReceived(playerName, message);
            });

    connect(m_server.get(), &SocketIOServer::cheatsUpdated,
            this, [this](const QString& roomId, const QJsonArray& cheats) {
                if (roomId != m_gameSession.roomId)
                    return;
                emit cheatsUpdated(cheats);
            });

    connect(m_server.get(), &SocketIOServer::saveSyncReceived,
            this, [this](const QString& roomId, const QJsonArray& saveFiles) {
                if (roomId != m_gameSession.roomId)
                    return;
                emit saveSyncReceived(saveFiles);
            });

    connect(m_server.get(), &SocketIOServer::coreSettingsSyncReceived,
            this, [this](const QString& roomId, const QJsonObject& coreSettings) {
                if (roomId != m_gameSession.roomId)
                    return;
                on_socketIO_coreSettingsSyncReceived(coreSettings);
            });

    connect(m_server.get(), &SocketIOServer::hostedWebRTCSignalReceived,
            this, &NetplayCoordinator::on_hostedWebRTCSignalReceived);
    connect(m_server.get(), &SocketIOServer::emulationBegin,
            this, [this](const QString& roomId) {
                if (roomId != m_gameSession.roomId) {
                    return;
                }
                emit emulationBeginReceived();
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

    QString connectAddress = ipAddress.trimmed();
    int connectPort = port;

    if (looksLikeTraversalCode(connectAddress)) {
        const TraversalLookupResult lookup = lookupTraversalHost(connectAddress);
        if (!lookup.success) {
            setState(Error);
            emit connectionError(lookup.error);
            return;
        }
        connectAddress = lookup.address;
        connectPort = lookup.port;
    }

    // Recreate the Socket.IO client with the new server URL
    QString serverUrl = QString("http://%1:%2").arg(connectAddress).arg(connectPort);
    
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
    connect(m_socketIO.get(), &SocketIOClient::frameSyncReceived,
            this, &NetplayCoordinator::on_peerFrameSyncReceived);

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
    connect(m_socketIO.get(), &SocketIOClient::coreSettingsSyncReceived,
            this, &NetplayCoordinator::on_socketIO_coreSettingsSyncReceived);
        connect(m_socketIO.get(), &SocketIOClient::emulationPauseReceived,
            this, &NetplayCoordinator::on_socketIO_emulationPauseReceived);
    connect(m_socketIO.get(), &SocketIOClient::emulationBeginReceived,
            this, &NetplayCoordinator::on_socketIO_emulationBeginReceived);
    
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
        if (!m_server->startHostedGame(m_gameSession.roomId, gameMode, resyncEnabled, romHash,
                                        m_sessionSyncCheats, m_sessionSyncSaves)) {
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
    if (m_state != InGame && m_state != StartingGame) {
        return;
    }

    setState(EndingGame);
    if (!isHostingServer() && m_socketIO) {
        m_socketIO->endGame();
    }

    resetEmulationSync();
}

void NetplayCoordinator::submitFrameInput(uint32_t controllerState)
{
    if (m_state != InGame || !m_lockstepEngine) return;

    const auto outbound =
        m_lockstepEngine->submitLocalInput(controllerState);

    const Qt::ConnectionType dispatchType =
        isHostingServer()
            ? socketDispatchConnectionType(m_server.get())
            : socketDispatchConnectionType(m_socketIO.get());

    for (const auto& [sendFrameNumber, state] : outbound) {
        QMetaObject::invokeMethod(
            this,
            "relayLocalControllerInput",
            dispatchType,
            Q_ARG(quint32, sendFrameNumber),
            Q_ARG(quint32, state));
    }
}

void NetplayCoordinator::relayLocalControllerInput(
    quint32 sendFrameNumber,
    quint32 state)
{
    if (isHostingServer()) {
        if (m_server && !m_gameSession.roomId.isEmpty()) {
            m_server->broadcastControllerInput(
                m_gameSession.roomId,
                m_lockstepConfig.localPlayerSlot,
                sendFrameNumber,
                state);
        }
    } else if (m_socketIO) {
        m_socketIO->sendControllerInput(sendFrameNumber, state);
    }
}

bool NetplayCoordinator::advanceFrame()
{
    if (m_state != InGame || !m_lockstepEngine) {
        return false;
    }

    const bool advanced = m_lockstepEngine->advanceFrame();
    if (advanced) {
        broadcastFrameSyncIfNeeded(m_lockstepEngine->getCurrentFrameNumber());
    }
    return advanced;
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
    CoreClearNetplaySyncSettings();
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

void NetplayCoordinator::on_socketIO_roomJoined(const QString& roomId, int slotIndex)
{
    qDebug() << "NetplayCoordinator: Room joined" << roomId << "slot:" << slotIndex;
    m_gameSession.roomId = roomId;
    if (slotIndex >= 0) {
        m_gameSession.localSlot = slotIndex;
        m_lockstepConfig.localPlayerSlot = slotIndex;
    }
    setState(InLobby);
    emit roomJoined(roomId, slotIndex >= 0 ? slotIndex : m_gameSession.localSlot);
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
    synchronizeLockstepPlayerCount();

    for (const auto& player : players) {
        if (player.id == m_socketIO->getPlayerId()) {
            m_gameSession.localSlot = player.slot;
            m_lockstepConfig.localPlayerSlot = player.slot;

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
    Q_UNUSED(mode);
    Q_UNUSED(resync);
    Q_UNUSED(matchId);

    m_lockstepConfig.localPlayerSlot = m_gameSession.localSlot;
    synchronizeLockstepPlayerCount();

    if (!m_sessionSyncCheats.isEmpty()) {
        emit cheatsUpdated(m_sessionSyncCheats);
    }

    emit gameStarted(m_gameSession);
}

void NetplayCoordinator::on_socketIO_gameEnded()
{
    qDebug() << "NetplayCoordinator: Game ended";
    resetEmulationSync();
    emit gameEnded();
}

void NetplayCoordinator::on_socketIO_offerReceived(const QString& fromPlayerId, const QString& sdpOffer)
{
    qDebug() << "NetplayCoordinator: Offer received from" << fromPlayerId;

    // Find which slot this player is in
    int slotIndex = -1;
    for (const auto& player : getPlayerList()) {
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
        bindWebRTCPeerSignals(peer, fromPlayerId);
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
void NetplayCoordinator::synchronizeLockstepPlayerCount()
{
    int numPlayers = 2;

    if (isHostingServer()) {
        numPlayers = std::max(2, m_server ? (m_server->getConnectedClientCount() + 1) : 2);
    }

    numPlayers = std::max(numPlayers, static_cast<int>(m_cachedPlayers.size()));
    if (numPlayers > 4) {
        numPlayers = 4;
    }

    m_lockstepConfig.numPlayers = numPlayers;
    m_gameSession.numPlayers = numPlayers;

    if (m_lockstepEngine) {
        m_lockstepEngine->setNumPlayers(numPlayers);
    }
}

void NetplayCoordinator::beginEmulationSync()
{
    m_lockstepConfig.localPlayerSlot = m_gameSession.localSlot;
    synchronizeLockstepPlayerCount();
    initializeLockstepEngine();

    CoreSetEmbeddedNetplayState(true, m_gameSession.localSlot);
    setState(InGame);
}

void NetplayCoordinator::resetEmulationSync()
{
    CoreSetEmbeddedNetplayState(false, 0);
    CoreClearNetplaySyncSettings();
    m_lockstepEngine.reset();
    m_currentFrameInputs.clear();
    m_sessionSyncCoreSettings = QJsonObject();
    m_lastBroadcastFrameSync = 0;

    if (m_state == InGame || m_state == EndingGame) {
        setState(InLobby);
    }
}

void NetplayCoordinator::initializeLockstepEngine()
{
    m_lockstepEngine.reset();
    m_currentFrameInputs.clear();

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

    callbacks.pumpNetwork = [this]() {
        if (QThread::currentThread() == this->thread()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
            return;
        }

        QMetaObject::invokeMethod(
            this,
            []() {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
            },
            Qt::BlockingQueuedConnection);
    };

    m_lockstepEngine->setCallbacks(callbacks);
    attachExistingPeerDataChannels();

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

void NetplayCoordinator::attachExistingPeerDataChannels()
{
    if (!m_lockstepEngine) {
        return;
    }

    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        const int slot = it.key();
        const auto& peer = it.value();
        if (!peer) {
            continue;
        }

        auto channel = peer->getDataChannel(QStringLiteral("RMG-Input"));
        if (!channel) {
            continue;
        }

        m_lockstepEngine->setDataChannel(slot, channel);
        qDebug() << "NetplayCoordinator: Attached existing data channel to LockstepEngine for slot" << slot;
    }
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
    m_sessionSyncCheats = cheats;

    if (isHostingServer()) {
        if (m_server && !m_gameSession.roomId.isEmpty()) {
            m_server->broadcastCheatsUpdate(m_gameSession.roomId, cheats);
            emit cheatsUpdated(cheats);
        }
        return;
    }

    if (m_socketIO && m_socketIO->getConnectionState() == SocketIOClient::Connected) {
        m_socketIO->sendCheatsUpdate(cheats);
    }
}

void NetplayCoordinator::sendSaveSync(const QJsonArray& saveFiles)
{
    m_sessionSyncSaves = saveFiles;

    if (isHostingServer()) {
        if (m_server && !m_gameSession.roomId.isEmpty()) {
            if (!saveFiles.isEmpty()) {
                m_server->broadcastSaveSync(m_gameSession.roomId, saveFiles);
            } else {
                qDebug() << "NetplayCoordinator: No save files found to sync for this ROM";
            }
        }
        return;
    }

    if (m_socketIO && m_socketIO->getConnectionState() == SocketIOClient::Connected) {
        m_socketIO->sendSaveSync(saveFiles);
    }
}

void NetplayCoordinator::sendCoreSettingsSync(const QJsonObject& coreSettings)
{
    if (coreSettings.isEmpty()) {
        return;
    }

    m_sessionSyncCoreSettings = coreSettings;

    if (isHostingServer()) {
        if (m_server && !m_gameSession.roomId.isEmpty()) {
            m_server->broadcastCoreSettingsSync(m_gameSession.roomId, coreSettings);
        }
        return;
    }

    if (m_socketIO && m_socketIO->getConnectionState() == SocketIOClient::Connected) {
        m_socketIO->sendCoreSettingsSync(coreSettings);
    }
}

void NetplayCoordinator::sendEmulationPauseUpdate(bool paused)
{
    Q_UNUSED(paused);

    // Lockstep netplay cannot pause one peer without desyncing everyone.
    if (CoreIsEmbeddedNetplayActive()) {
        return;
    }

    if (!isInGame()) {
        return;
    }

    if (isHostingServer()) {
        if (m_server && !m_gameSession.roomId.isEmpty()) {
            m_server->broadcastEmulationPauseUpdate(m_gameSession.roomId, paused);
        }
        return;
    }

    if (m_socketIO && m_socketIO->getConnectionState() == SocketIOClient::Connected) {
        m_socketIO->sendEmulationPauseUpdate(paused);
    }
}

void NetplayCoordinator::setInputDelayFrames(int frames)
{
    if (frames < 1) {
        frames = 1;
    } else if (frames > 99) {
        frames = 99;
    }

    m_lockstepConfig.inputDelayFrames = frames;
    if (m_lockstepEngine) {
        m_lockstepEngine->setInputDelayFrames(frames);
    }
}

int NetplayCoordinator::getInputDelayFrames() const
{
    return m_lockstepConfig.inputDelayFrames;
}

void NetplayCoordinator::on_socketIO_cheatsUpdated(const QJsonArray& cheats)
{
    m_sessionSyncCheats = cheats;
    emit cheatsUpdated(cheats);
}

void NetplayCoordinator::on_socketIO_saveSyncReceived(const QJsonArray& saveFiles)
{
    emit saveSyncReceived(saveFiles);
}

void NetplayCoordinator::on_socketIO_coreSettingsSyncReceived(const QJsonObject& coreSettings)
{
    if (isHostingServer()) {
        return;
    }

    m_sessionSyncCoreSettings = coreSettings;

    CoreNetplaySyncSettings settings;
    if (!coreSettingsFromJson(coreSettings, settings)) {
        qWarning() << "NetplayCoordinator: Ignoring invalid core settings sync payload";
        return;
    }

    CoreSetNetplaySyncSettings(settings);
    emit coreSettingsSyncReceived(coreSettings);
}

void NetplayCoordinator::on_socketIO_emulationPauseReceived(bool paused)
{
    Q_UNUSED(paused);

    // Lockstep netplay must keep all peers advancing frames together.
    if (CoreIsEmbeddedNetplayActive()) {
        return;
    }

    if (paused)
    {
        if (CoreIsEmulationRunning() && !CoreIsEmulationPaused())
        {
            CorePauseEmulation();
        }
        return;
    }

    if (CoreIsEmulationPaused())
    {
        CoreResumeEmulation();
    }
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

void NetplayCoordinator::on_peerFrameSyncReceived(int slot, uint32_t frameNumber, uint32_t stateHash)
{
    if (m_state != InGame || !m_lockstepEngine) {
        return;
    }

    if (slot == m_gameSession.localSlot) {
        return;
    }

    if (slot >= 0 && slot < m_lockstepConfig.numPlayers) {
        m_lockstepEngine->submitPeerFrameSync(slot, frameNumber, stateHash);
    } else if (slot == -1 && m_lockstepConfig.numPlayers == 2) {
        const int inferredSlot = (m_gameSession.localSlot == 0) ? 1 : 0;
        m_lockstepEngine->submitPeerFrameSync(inferredSlot, frameNumber, stateHash);
    }
}

void NetplayCoordinator::broadcastFrameSyncIfNeeded(uint32_t frameNumber)
{
    if (m_state != InGame || frameNumber == 0 || !m_lockstepEngine) {
        return;
    }

    // ~1 Hz at 60 FPS; compare state hashes at the same lockstep frame.
    constexpr uint32_t kFrameSyncIntervalFrames = 60;
    if (frameNumber % kFrameSyncIntervalFrames != 0 ||
        frameNumber == m_lastBroadcastFrameSync) {
        return;
    }

    const uint32_t stateHash = CoreGetNetplayFrameSyncHash();
    if (stateHash == 0) {
        return;
    }

    m_lastBroadcastFrameSync = frameNumber;
    m_lockstepEngine->recordLocalFrameSync(frameNumber, stateHash);

    if (isHostingServer()) {
        QMetaObject::invokeMethod(m_server.get(), [this, frameNumber, stateHash]() {
            m_server->broadcastFrameSync(
                m_gameSession.roomId,
                m_lockstepConfig.localPlayerSlot,
                frameNumber,
                stateHash);
        }, Qt::QueuedConnection);
    } else if (m_socketIO) {
        QMetaObject::invokeMethod(m_socketIO.get(), [this, frameNumber, stateHash]() {
            m_socketIO->sendFrameSync(frameNumber, stateHash);
        }, Qt::QueuedConnection);
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

void NetplayCoordinator::bindWebRTCPeerSignals(const std::shared_ptr<WebRTCPeer>& peer, const QString& peerId)
{
    if (!peer) {
        return;
    }

    connect(peer.get(), &WebRTCPeer::offerCreated, this, [this, peerId](const QString& offer) {
        sendWebRTCOffer(peerId, offer);
    });
    connect(peer.get(), &WebRTCPeer::answerReceived, this, [this, peerId](const QString& answer) {
        sendWebRTCAnswer(peerId, answer);
    });
    connect(peer.get(), &WebRTCPeer::iceCandidateGenerated, this,
            [this, peerId](const QString& candidate, int mLineIndex) {
        sendWebRTCIceCandidate(peerId, candidate, mLineIndex);
    });
    connect(peer.get(), &WebRTCPeer::connectionEstablished, this, [this, peerId]() {
        on_webRTC_connectionEstablished(peerId);
    });
    connect(peer.get(), &WebRTCPeer::connectionFailed, this, [this, peerId](const QString& reason) {
        on_webRTC_connectionFailed(peerId, reason);
    });
    connect(peer.get(), &WebRTCPeer::dataChannelOpened, this, [this, peerId](const QString& label) {
        on_webRTC_dataChannelOpened(peerId, label);
    });
}

void NetplayCoordinator::sendWebRTCOffer(const QString& targetPlayerId, const QString& sdpOffer)
{
    if (isHostingServer() && m_server && !m_gameSession.roomId.isEmpty()) {
        QJsonObject signal;
        signal[QStringLiteral("target")] = targetPlayerId;
        signal[QStringLiteral("offer")] = sdpOffer;
        m_server->relayHostedWebRTCSignal(m_gameSession.roomId, QStringLiteral("p0"), signal);
        return;
    }

    if (m_socketIO) {
        m_socketIO->sendOffer(targetPlayerId, sdpOffer);
    }
}

void NetplayCoordinator::sendWebRTCAnswer(const QString& targetPlayerId, const QString& sdpAnswer)
{
    if (isHostingServer() && m_server && !m_gameSession.roomId.isEmpty()) {
        QJsonObject signal;
        signal[QStringLiteral("target")] = targetPlayerId;
        signal[QStringLiteral("answer")] = sdpAnswer;
        m_server->relayHostedWebRTCSignal(m_gameSession.roomId, QStringLiteral("p0"), signal);
        return;
    }

    if (m_socketIO) {
        m_socketIO->sendAnswer(targetPlayerId, sdpAnswer);
    }
}

void NetplayCoordinator::sendWebRTCIceCandidate(const QString& targetPlayerId, const QString& candidate, int mLineIndex)
{
    if (isHostingServer() && m_server && !m_gameSession.roomId.isEmpty()) {
        QJsonObject signal;
        signal[QStringLiteral("target")] = targetPlayerId;
        signal[QStringLiteral("candidate")] = candidate;
        signal[QStringLiteral("sdpMLineIndex")] = mLineIndex;
        m_server->relayHostedWebRTCSignal(m_gameSession.roomId, QStringLiteral("p0"), signal);
        return;
    }

    if (m_socketIO) {
        m_socketIO->sendICECandidate(targetPlayerId, candidate, mLineIndex);
    }
}

void NetplayCoordinator::on_hostedWebRTCSignalReceived(const QString& fromPlayerId, const QJsonObject& signal)
{
    if (signal.contains(QStringLiteral("offer"))) {
        on_socketIO_offerReceived(fromPlayerId, signal.value(QStringLiteral("offer")).toString());
    } else if (signal.contains(QStringLiteral("answer"))) {
        on_socketIO_answerReceived(fromPlayerId, signal.value(QStringLiteral("answer")).toString());
    } else if (signal.contains(QStringLiteral("candidate"))) {
        on_socketIO_iceCandidateReceived(
            fromPlayerId,
            signal.value(QStringLiteral("candidate")).toString(),
            signal.value(QStringLiteral("sdpMLineIndex")).toInt(0));
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
        bindWebRTCPeerSignals(peer, player.id);

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

void NetplayCoordinator::sendInputDelayUpdate(int frames)
{
    if (!isHost()) {
        return;
    }

    setInputDelayFrames(frames);

    if (isHostingServer() && m_server && !m_gameSession.roomId.isEmpty()) {
        m_server->broadcastInputDelayUpdate(m_gameSession.roomId, frames);
    } else if (m_socketIO) {
        m_socketIO->sendInputDelayUpdate(frames);
    }

    emit inputDelayChanged(frames);
}

void NetplayCoordinator::sendEmulationReady()
{
    if (m_gameSession.roomId.isEmpty()) {
        return;
    }

    if (isHostingServer()) {
        if (m_server) {
            m_server->markEmulationReady(
                m_gameSession.roomId,
                m_gameSession.localSlot);
        }
        return;
    }

    if (m_socketIO &&
        m_socketIO->getConnectionState() == SocketIOClient::Connected) {
        m_socketIO->sendEmulationReady();
    }
}

void NetplayCoordinator::on_socketIO_emulationBeginReceived()
{
    emit emulationBeginReceived();
}

void NetplayCoordinator::on_socketIO_inputDelayReceived(int frames)
{
    if (frames < 1) {
        frames = 1;
    } else if (frames > 99) {
        frames = 99;
    }

    setInputDelayFrames(frames);
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