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
#include "Netplay.hpp"
#include "WebRTC/TurnCredentialClient.hpp"
#include <RMG-Core/Netplay/WebRTC/WebRTCDataChannel.hpp>
#include <RMG-Core/Netplay.hpp>
#include <RMG-Core/Emulation.hpp>
#include <algorithm>
#include <cstring>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QHash>
#include <QThread>
#include <QCoreApplication>
#include <QEventLoop>
#include <QUuid>
#include <QTimer>
#include <thread>

using namespace UserInterface::Netplay;
using namespace RMGCore;

namespace {

Qt::ConnectionType socketDispatchConnectionType(const QObject* target)
{
    if (QThread::currentThread() == target->thread()) {
        return Qt::DirectConnection;
    }

    // Emulation runs off the UI thread; queue socket I/O so we never block the
    // emulation thread (blocking here deadlocks with VidExt and lockstep stalls).
    return Qt::QueuedConnection;
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

constexpr int kWebRtcInputChannelWaitMs = 15000;
constexpr int kInputHandshakeRetryMs = 200;
constexpr int kInputHandshakeTimeoutMs = 8000;
constexpr uint32_t kInputHandshakeFrame = 0;
constexpr uint32_t kInputHandshakeState = 0;

} // namespace

std::shared_ptr<RMGCore::LockstepEngine> NetplayCoordinator::activeLockstepEngine()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_state != InGame || !m_lockstepEngine) {
        return nullptr;
    }

    return m_lockstepEngine;
}

NetplayCoordinator::NetplayCoordinator(const QString& serverUrl, QObject* parent)
    : QObject(parent)
    , m_state(Idle)
    , m_playerId(QUuid::createUuid().toString(QUuid::WithoutBraces))
    , m_shouldAutoJoinRoom(false)
{
    // Create Socket.IO client
    m_socketIO = std::make_unique<SocketIOClient>(serverUrl, this);
    connectSocketIOClientSignals(m_socketIO.get());

    // Initialize lockstep config
    m_lockstepConfig.numPlayers = 2;
    m_lockstepConfig.localPlayerSlot = 0;
    m_lockstepConfig.inputDelayFrames = 6;
    m_lockstepConfig.desyncDetectionEnabled = true;
    m_lockstepConfig.resyncEnabled = false;
    m_lockstepConfig.resyncCheckIntervalFrames = 180;
    m_lockstepConfig.stallTimeoutMilliseconds = 0;
    CoreSetEmbeddedNetplayInputDelayFrames(m_lockstepConfig.inputDelayFrames);

    m_emulationPrepTimer = new QTimer(this);
    m_emulationPrepTimer->setInterval(kInputHandshakeRetryMs);
    connect(m_emulationPrepTimer, &QTimer::timeout,
            this, &NetplayCoordinator::evaluateEmulationStartReadiness);

    qDebug() << "NetplayCoordinator created";
    UserInterface::Netplay::g_netplayCoordinator = this;
    TurnCredentialClient::instance().prefetch();
}

NetplayCoordinator::~NetplayCoordinator()
{
    resetEmulationSync();

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
    m_lastHostingError.clear();

    if (m_server != nullptr && m_server->isRunning() && m_server->getPort() == port)
    {
        qInfo() << "Already hosting signaling server on port" << port;
        return true;
    }

    if (m_server != nullptr)
    {
        stopHosting();
    }

    m_playerName = playerName;

    // Create and start signaling server
    m_server = std::make_unique<SocketIOServer>(this);

    QString listenError;
    if (!m_server->startServer(port, &listenError))
    {
        m_lastHostingError = listenError;
        qWarning() << "Failed to start hosting server on port" << port << ":" << listenError;
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

                resetEmulationStartPrep();
                m_gameStartPrepTimer.start();

                int totalPlayers = 1;
                if (this->isHostingServer() && m_server) {
                    totalPlayers = std::max(totalPlayers, m_server->getConnectedClientCount() + 1);
                }
                totalPlayers = std::max(totalPlayers, static_cast<int>(m_cachedPlayers.size()));
                for (const auto& player : m_cachedPlayers) {
                    if (player.slot >= 0) {
                        totalPlayers = std::max(totalPlayers, player.slot + 1);
                    }
                }
                if (totalPlayers > 4) {
                    totalPlayers = 4;
                }

                m_gameSession.numPlayers = totalPlayers;
                m_lockstepConfig.numPlayers = totalPlayers;
                // Preserve remapped host controller port (not always P1).
                if (m_gameSession.localSlot >= 0) {
                    m_lockstepConfig.localPlayerSlot = m_gameSession.localSlot;
                }

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
                    if (p.id.isEmpty()) {
                        p.id = obj["id"].toString();
                    }
                    p.clientId = obj["clientId"].toString();
                    if (p.clientId.isEmpty()) {
                        p.clientId = p.id;
                    }
                    p.name = obj["name"].toString();
                    p.slot = obj["slotIndex"].toInt(obj["slot"].toInt(-1));
                    p.isSpectator = false;
                    p.isReady = true;
                    players.append(p);

                    // Host identity is clientId "host", not necessarily controller port 0.
                    if (p.clientId == QStringLiteral("host") || p.id == QStringLiteral("host")) {
                        m_gameSession.localSlot = p.slot;
                        m_lockstepConfig.localPlayerSlot = p.slot;
                        if (m_lockstepEngine) {
                            m_lockstepEngine->setLocalPlayerSlot(p.slot);
                        }
                    }
                }

                m_cachedPlayers = players;
                synchronizeLockstepPlayerCount();

                if (m_state == Connected || m_state == InLobby || m_state == StartingGame) {
                    setupPeerConnections(players);
                }

                emit playersUpdated(players);
            });

    connect(m_server.get(), &SocketIOServer::controllerInputReceived,
        this, [this](const QString& roomId, int slot, uint32_t frameNumber, uint32_t controllerState) {
            if (roomId != m_gameSession.roomId) {
                return;
            }

            // Hosted path shares the same dual-path rule as Socket.IO clients.
            on_socketIO_controllerInputReceived(slot, frameNumber, controllerState);
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
    connect(m_server.get(), &SocketIOServer::playerPingsUpdated,
            this, [this](const QString& roomId, const QJsonArray& pings) {
                if (roomId != m_gameSession.roomId) {
                    return;
                }
                applyPlayerPings(pings);
            });

    // Create initial room for remote players to join
    m_gameSession.roomId = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8).toUpper();
    m_gameSession.localSlot = 0;
    m_gameSession.numPlayers = 1;
    m_lockstepConfig.localPlayerSlot = 0;
    m_lockstepConfig.numPlayers = 1;
    m_isRoomHost = true;
    m_playerId = QStringLiteral("host");

    // Hosting is active as soon as server is up; room-created callback moves us to InLobby.
    setState(Connected);

    // Pre-create the room on the server so remote players can join it
    m_server->createInitialRoom(m_gameSession.roomId, playerName, gameName);

    m_playerPingMs[0] = 0;

    qInfo() << "Hosting signaling server on port" << port;

    setState(InLobby);
    TurnCredentialClient::instance().ensureCredentials(15000);

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
    applyNetplayConnectionSettings(NetplayConnectionMode::Direct, false);

    m_playerName = playerName;
    m_shouldAutoJoinRoom = true;
    m_autoJoinRoomId = roomId.trimmed();

    const QString connectAddress = ipAddress.trimmed();
    const int connectPort = port;

    // Recreate the signaling client with the new server endpoint
    QString serverUrl = QString("udp://%1:%2").arg(connectAddress).arg(connectPort);
    
    // Disconnect existing client if connected
    if (m_socketIO && m_socketIO->getConnectionState() != SocketIOClient::Disconnected)
    {
        m_socketIO->disconnect();
    }
    
    // Create new client with the direct IP server URL
    m_socketIO = std::make_unique<SocketIOClient>(serverUrl, this);
    connectSocketIOClientSignals(m_socketIO.get());
    
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
    if (m_state != InLobby &&
        m_state != InGame &&
        m_state != StartingGame &&
        m_state != JoiningRoom &&
        m_state != CreatingRoom) {
        return;
    }

    const bool wasInGame = (m_state == InGame || m_state == StartingGame);
    const bool hosting = isHostingServer();

    if (wasInGame) {
        resetEmulationSync();
        emit gameEnded();
    }

    if (hosting) {
        stopHosting();
        clearRoomSessionState();
        setState(Idle);
        emit roomClosed(QStringLiteral("left"));
        return;
    }

    if (m_socketIO && m_socketIO->getConnectionState() == SocketIOClient::Connected) {
        m_socketIO->leaveRoom();
    }

    clearRoomSessionState();
    if (m_socketIO && m_socketIO->getConnectionState() == SocketIOClient::Connected) {
        setState(Connected);
    } else {
        setState(Idle);
    }
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

        setState(StartingGame);
        if (!m_server->startHostedGame(m_gameSession.roomId, gameMode, resyncEnabled, romHash,
                                        m_sessionSyncCheats, m_sessionSyncSaves,
                                        m_sessionSyncCoreSettings)) {
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
    const auto engine = activeLockstepEngine();
    if (!engine) {
        return;
    }

    const auto outbound = engine->submitLocalInput(controllerState);
    if (outbound.empty()) {
        return;
    }

    const Qt::ConnectionType dispatchType =
        isHostingServer()
            ? socketDispatchConnectionType(m_server.get())
            : socketDispatchConnectionType(m_socketIO.get());

    // Always relay the full outbound burst on signaling — never tip-only.
    // Tip-only + gap-fill raced with the WebRTC full stream and invented wrong
    // mid-window inputs (reciprocal state-hash desyncs). WebRTC still carries
    // every frame via LockstepEngine::broadcastInput; signaling must match.
    for (const auto& [frame, state] : outbound) {
        const quint32 frameNumber = frame;
        const quint32 frameState = state;
        QMetaObject::invokeMethod(
            this,
            [this, frameNumber, frameState]() {
                relayLocalControllerInput(frameNumber, frameState);
            },
            dispatchType);
    }
}

void NetplayCoordinator::flushPendingControllerRelay()
{
    m_relayInputQueued.store(false, std::memory_order_release);

    const quint32 frameNumber = m_pendingRelayFrame.load(std::memory_order_relaxed);
    const quint32 state = m_pendingRelayState.load(std::memory_order_relaxed);
    relayLocalControllerInput(frameNumber, state);
}

void NetplayCoordinator::relayLocalControllerInputBurst(
    quint32 startFrameNumber,
    quint32 endFrameNumber,
    quint32 state)
{
    if (endFrameNumber < startFrameNumber) {
        return;
    }

    relayLocalControllerInput(endFrameNumber, state);
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
    const auto engine = activeLockstepEngine();
    if (!engine) {
        return false;
    }

    // Hash at the lockstep boundary for the previously completed frame. At this
    // point every peer is in GetKeys after that frame's inputs were consumed by
    // the core and before waiting on the next frame — so labels cannot drift a
    // frame relative to GL swap / extra PIF polls.
    const uint32_t frameToRun = engine->getCurrentFrameNumber();
    if (frameToRun > 0) {
        maybeSubmitCompletedFrameSync(frameToRun - 1);
    }

    return engine->advanceFrame();
}

void NetplayCoordinator::submitEndOfFrameSync()
{
    // Frame sync is taken in advanceFrame() at the lockstep boundary. GL swap
    // timing is not deterministic across peers (extra polls, skipped buffers).
}

void NetplayCoordinator::onDesyncDetected(const QString& reason)
{
    emit desyncDetected(reason);

    const auto engine = activeLockstepEngine();
    if (engine && engine->isDesynchronized()) {
        emit resyncAttempted();
        // Actual resync would be handled at a higher level (Emulation.cpp)
    }
}

void NetplayCoordinator::verifyGameSync(uint32_t romChecksum)
{
    const auto engine = activeLockstepEngine();
    if (engine) {
        engine->checkDesync(romChecksum);
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

int NetplayCoordinator::getPlayerPing(int slot) const
{
    return m_playerPingMs.value(slot, -1);
}

void NetplayCoordinator::applyPlayerPings(const QJsonArray& pings)
{
    for (const QJsonValue& value : pings) {
        const QJsonObject entry = value.toObject();
        const int slot = entry.value(QStringLiteral("slot")).toInt(-1);
        const int pingMs = entry.value(QStringLiteral("ms")).toInt(-1);
        if (slot >= 0) {
            m_playerPingMs[slot] = pingMs;
        }
    }

    if (isHostingServer()) {
        m_playerPingMs[0] = 0;
    } else if (m_socketIO && m_gameSession.localSlot >= 0) {
        const int localPing = m_socketIO->getLastPingMs();
        if (localPing >= 0) {
            m_playerPingMs[m_gameSession.localSlot] = localPing;
        }
    }

    emit playerPingsUpdated();
}

bool NetplayCoordinator::isHost() const
{
    // Room host is independent of controller port (P1/P2 can be remapped).
    return isHostingServer() || m_isRoomHost;
}

bool NetplayCoordinator::reorderLobbyPlayers(const QStringList& clientIds)
{
    if (!isHost() || clientIds.isEmpty()) {
        return false;
    }

    if (m_state != InLobby && m_state != Connected) {
        return false;
    }

    if (isHostingServer() && m_server && !m_gameSession.roomId.isEmpty()) {
        return m_server->reorderLobbyPlayers(m_gameSession.roomId, clientIds);
    }

    if (m_socketIO) {
        m_socketIO->reorderPlayers(clientIds);
        return true;
    }

    return false;
}

bool NetplayCoordinator::isInGame() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_state == InGame;
}

//
// Private Slots - Socket.IO
//

void NetplayCoordinator::connectSocketIOClientSignals(SocketIOClient* client)
{
    if (!client) {
        return;
    }

    connect(client, &SocketIOClient::connected,
            this, &NetplayCoordinator::on_socketIO_connected);
    connect(client, &SocketIOClient::disconnected,
            this, &NetplayCoordinator::on_socketIO_disconnected);
    connect(client, &SocketIOClient::reconnecting,
            this, &NetplayCoordinator::on_socketIO_reconnecting);
    connect(client, &SocketIOClient::reconnected,
            this, &NetplayCoordinator::on_socketIO_reconnected);
    connect(client, &SocketIOClient::connectionError,
            this, &NetplayCoordinator::on_socketIO_connectionError);

    connect(client, &SocketIOClient::roomCreated,
            this, &NetplayCoordinator::on_socketIO_roomCreated);
    connect(client, &SocketIOClient::roomJoined,
            this, &NetplayCoordinator::on_socketIO_roomJoined);
    connect(client, &SocketIOClient::roomLeft,
            this, &NetplayCoordinator::on_socketIO_roomLeft);
    connect(client, &SocketIOClient::roomClosed,
            this, &NetplayCoordinator::on_socketIO_roomClosed);
    connect(client, &SocketIOClient::playersUpdated,
            this, &NetplayCoordinator::on_socketIO_playersUpdated);

    connect(client, &SocketIOClient::gameStarted,
            this, &NetplayCoordinator::on_socketIO_gameStarted);
    connect(client, &SocketIOClient::gameEnded,
            this, &NetplayCoordinator::on_socketIO_gameEnded);
    connect(client, &SocketIOClient::controllerInputReceived,
            this, &NetplayCoordinator::on_socketIO_controllerInputReceived);
    connect(client, &SocketIOClient::frameSyncReceived,
            this, &NetplayCoordinator::on_peerFrameSyncReceived);

    connect(client, &SocketIOClient::offerReceived,
            this, &NetplayCoordinator::on_socketIO_offerReceived);
    connect(client, &SocketIOClient::answerReceived,
            this, &NetplayCoordinator::on_socketIO_answerReceived);
    connect(client, &SocketIOClient::iceCandidateReceived,
            this, &NetplayCoordinator::on_socketIO_iceCandidateReceived);

    connect(client, &SocketIOClient::chatMessageReceived,
            this, &NetplayCoordinator::chatMessageReceived);
    connect(client, &SocketIOClient::cheatsUpdated,
            this, &NetplayCoordinator::on_socketIO_cheatsUpdated);
    connect(client, &SocketIOClient::saveSyncReceived,
            this, &NetplayCoordinator::on_socketIO_saveSyncReceived);
    connect(client, &SocketIOClient::coreSettingsSyncReceived,
            this, &NetplayCoordinator::on_socketIO_coreSettingsSyncReceived);
    connect(client, &SocketIOClient::roomsListed,
            this, &NetplayCoordinator::on_socketIO_roomsListed);
    connect(client, &SocketIOClient::inputDelayReceived,
            this, &NetplayCoordinator::on_socketIO_inputDelayReceived);
    connect(client, &SocketIOClient::emulationPauseReceived,
            this, &NetplayCoordinator::on_socketIO_emulationPauseReceived);
    connect(client, &SocketIOClient::emulationBeginReceived,
            this, &NetplayCoordinator::on_socketIO_emulationBeginReceived);
    connect(client, &SocketIOClient::pingUpdated,
            this, [this](int pingMs) {
        if (m_gameSession.localSlot >= 0) {
            m_playerPingMs[m_gameSession.localSlot] = pingMs;
        }
        emit playerPingsUpdated();
    });
    connect(client, &SocketIOClient::playerPingsReceived,
            this, [this](const QJsonArray& pings) {
        applyPlayerPings(pings);
    });
}

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

void NetplayCoordinator::on_socketIO_reconnecting()
{
    qWarning() << "NetplayCoordinator: Signaling connection lost, attempting reconnect";
}

void NetplayCoordinator::on_socketIO_reconnected()
{
    qInfo() << "NetplayCoordinator: Signaling connection restored";

    if (m_state == Idle || m_state == Error) {
        setState(Connected);
    }

    attachExistingPeerDataChannels();
    recoverWebRTCPeerConnections();
}

void NetplayCoordinator::on_socketIO_disconnected()
{
    qDebug() << "NetplayCoordinator: Socket.IO disconnected";

    const bool wasInGame = (m_state == InGame || m_state == StartingGame);

    if (auto engine = activeLockstepEngine()) {
        engine->releaseCurrentFrameWait();
    }

    clearRoomSessionState();

    if (wasInGame) {
        resetEmulationSync();
        emit gameEnded();
    } else {
        CoreClearNetplaySyncSettings();
    }

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
    m_isRoomHost = true;
    setState(InLobby);
    emit roomCreated(roomId, 0);
}

void NetplayCoordinator::on_socketIO_roomJoined(const QString& roomId, int slotIndex)
{
    qDebug() << "NetplayCoordinator: Room joined" << roomId << "slot:" << slotIndex;
    m_gameSession.roomId = roomId;
    m_isRoomHost = false;
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
    clearRoomSessionState();
    setState(Connected);
    emit roomClosed("left");
}

void NetplayCoordinator::on_socketIO_roomClosed(const QString& reason)
{
    qDebug() << "NetplayCoordinator: Room closed:" << reason;
    clearRoomSessionState();
    setState(Connected);
    emit roomClosed(reason);
}

void NetplayCoordinator::syncLockstepPeerSessionActive()
{
    if (!m_lockstepEngine) {
        return;
    }

    const int numPlayers = m_lockstepConfig.numPlayers;
    const QList<SocketIOClient::PlayerInfo> players = getPlayerList();
    // An empty list is often a transient signaling blip — do not mark everyone
    // inactive or frame 0 will advance with zeros and desync when peers return.
    const bool trustAbsences = !players.isEmpty();

    for (int slot = 0; slot < numPlayers; ++slot) {
        if (slot == m_lockstepConfig.localPlayerSlot) {
            m_lockstepEngine->setPeerSessionActive(slot, true);
            continue;
        }

        bool present = false;
        for (const auto& player : players) {
            if (player.slot == slot) {
                present = true;
                break;
            }
        }

        if (present) {
            m_lockstepEngine->setPeerSessionActive(slot, true);
        } else if (trustAbsences) {
            m_lockstepEngine->setPeerSessionActive(slot, false);
        }
    }
}

void NetplayCoordinator::on_socketIO_playersUpdated(const QList<SocketIOClient::PlayerInfo>& players)
{
    m_cachedPlayers = players;
    synchronizeLockstepPlayerCount();

    const QString localPlayerId = m_socketIO ? m_socketIO->getPlayerId() : m_playerId;
    for (const auto& player : players) {
        const bool isSelf =
            (!localPlayerId.isEmpty() &&
             (player.id == localPlayerId || player.clientId == localPlayerId)) ||
            (m_isRoomHost &&
             (player.clientId == QStringLiteral("host") || player.id == QStringLiteral("host")));
        if (!isSelf) {
            continue;
        }

        m_gameSession.localSlot = player.slot;
        m_lockstepConfig.localPlayerSlot = player.slot;
        if (m_lockstepEngine) {
            m_lockstepEngine->setLocalPlayerSlot(player.slot);
        }
        break;
    }

    if (m_state == Connected || m_state == InLobby || m_state == StartingGame) {
        setupPeerConnections(players);
    }

    if (m_state == InGame) {
        syncLockstepPeerSessionActive();
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

    resetEmulationStartPrep();
    m_gameStartPrepTimer.start();

    m_lockstepConfig.localPlayerSlot = m_gameSession.localSlot;
    synchronizeLockstepPlayerCount();
    setupPeerConnections(getPlayerList());

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
    int numPlayers = 1;

    if (isHostingServer()) {
        numPlayers = std::max(1, m_server ? (m_server->getConnectedClientCount() + 1) : 1);
    }

    numPlayers = std::max(numPlayers, static_cast<int>(m_cachedPlayers.size()));
    for (const auto& player : m_cachedPlayers) {
        if (player.slot >= 0) {
            numPlayers = std::max(numPlayers, player.slot + 1);
        }
    }
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

    // Belt-and-suspenders: ensure synced timing is in core before M64CMD_EXECUTE.
    if (!m_sessionSyncCoreSettings.isEmpty()) {
        CoreNetplaySyncSettings settings;
        if (coreSettingsFromJson(m_sessionSyncCoreSettings, settings)) {
            CoreSetNetplaySyncSettings(settings);
        }
    }

    initializeLockstepEngine();

    CoreSetEmbeddedNetplayState(true, m_gameSession.localSlot);
    setState(InGame);
}

void NetplayCoordinator::resetEmulationSync()
{
    CoreSetEmbeddedNetplayState(false, 0);
    CoreClearNetplaySyncSettings();
    UserInterface::Netplay::installEmbeddedNetplayCallbacks();

    std::shared_ptr<RMGCore::LockstepEngine> engine;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (m_state == InGame || m_state == EndingGame) {
            m_state = InLobby;
        }
        engine = std::move(m_lockstepEngine);
        m_currentFrameInputs.clear();
        m_lastBroadcastFrameSync = 0;
        m_pendingFrameSyncFrame.store(0, std::memory_order_relaxed);
    }

    if (engine) {
        engine->shutdown();
    }

    m_sessionSyncCoreSettings = QJsonObject();
    m_pumpNetworkQueued.store(false, std::memory_order_relaxed);
    m_relayInputQueued.store(false, std::memory_order_relaxed);
    m_pendingRelayFrame.store(0, std::memory_order_relaxed);
    m_pendingRelayState.store(0, std::memory_order_relaxed);
    resetEmulationStartPrep();
}

void NetplayCoordinator::initializeLockstepEngine()
{
    // The emulator thread reads m_lockstepEngine/m_currentFrameInputs under
    // m_mutex (activeLockstepEngine()/getSyncedInput()). Reassigning the
    // shared_ptr and clearing the map here without the lock is a data race that
    // corrupts the shared_ptr control block / map nodes (heap double-free).
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (m_lockstepEngine) {
        m_lockstepEngine->shutdown();
        m_lockstepEngine.reset();
    }
    m_currentFrameInputs.clear();
    m_pumpNetworkQueued.store(false, std::memory_order_relaxed);
    m_relayInputQueued.store(false, std::memory_order_relaxed);
    m_pendingRelayFrame.store(0, std::memory_order_relaxed);
    m_pendingRelayState.store(0, std::memory_order_relaxed);

    m_lockstepEngine = std::make_shared<RMGCore::LockstepEngine>(m_lockstepConfig);

    RMGCore::LockstepEngine::Callbacks callbacks;
    callbacks.frameReady = [this](uint32_t frameNumber, const std::map<int, uint32_t>& inputs) {
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            m_currentFrameInputs = inputs;
        }

        QMap<int, uint32_t> qtInputs;
        for (const auto& [slot, input] : inputs) {
            qtInputs[slot] = input;
        }

        QMetaObject::invokeMethod(
            this,
            [this, frameNumber, qtInputs]() {
                emit gameFrameReady(frameNumber, qtInputs);
            },
            Qt::QueuedConnection);
    };

    callbacks.peerInputStalled = [this](int playerSlot, uint32_t frameNumber) {
        QMetaObject::invokeMethod(
            this,
            [this, playerSlot, frameNumber]() {
                emit peerInputStalled(playerSlot, frameNumber);
            },
            Qt::QueuedConnection);
    };

    callbacks.desyncDetected = [this](uint32_t frameNumber, const std::string& reason) {
        Q_UNUSED(frameNumber);
        const QString qtReason = QString::fromStdString(reason);
        QMetaObject::invokeMethod(
            this,
            [this, qtReason]() {
                emit desyncDetected(qtReason);
            },
            Qt::QueuedConnection);
    };

    callbacks.pumpNetwork = [this]() {
        if (QThread::currentThread() == this->thread()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
            return;
        }

        // Never block the emulation thread on the UI thread here. Also keep only
        // one queued pump in flight; otherwise lockstep stalls can flood the event
        // queue and make the host appear frozen.
        if (!m_pumpNetworkQueued.exchange(true, std::memory_order_acq_rel)) {
            QMetaObject::invokeMethod(
                this,
                [this]() {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
                    m_pumpNetworkQueued.store(false, std::memory_order_release);
                },
                Qt::QueuedConnection);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };

    m_lockstepEngine->setCallbacks(callbacks);
    syncLockstepPeerSessionActive();
    attachExistingPeerDataChannels();

    if (!m_inputHandshakeInputs.empty()) {
        m_lockstepEngine->importPreGameHandshakeInputs(
            m_inputHandshakeInputs,
            kInputHandshakeState);
    } else {
        m_lockstepEngine->importPreGameHandshakeInputs(
            {},
            kInputHandshakeState);
    }
    m_lockstepEngine->markInputBootstrapComplete();

    UserInterface::Netplay::installEmbeddedNetplayCallbacks();
}

void NetplayCoordinator::attachPeerDataChannelToLockstep(int peerSlot, const QString& label)
{
    if (!m_lockstepEngine || peerSlot < 0) {
        return;
    }

    const auto peerIt = m_peers.find(peerSlot);
    if (peerIt == m_peers.end() || !peerIt.value()) {
        return;
    }

    auto channel = peerIt.value()->getDataChannel(label);
    if (!channel) {
        qWarning() << "NetplayCoordinator: Failed to get data channel" << label
                   << "for slot" << peerSlot;
        return;
    }

    m_lockstepEngine->setDataChannel(peerSlot, channel);
    if (peerSlot != m_lockstepConfig.localPlayerSlot) {
        m_lockstepEngine->setPeerSessionActive(peerSlot, true);
    }
    qDebug() << "NetplayCoordinator: Data channel" << label
             << "registered with LockstepEngine for slot" << peerSlot;
}

void NetplayCoordinator::attachExistingPeerDataChannels()
{
    if (!m_lockstepEngine) {
        return;
    }

    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        attachPeerDataChannelToLockstep(it.key(), QStringLiteral("RMG-Input"));
    }
}

int NetplayCoordinator::findPeerSlotById(const QString& peerId) const
{
    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        const auto& peer = it.value();
        if (peer && peer->getPeerId() == peerId) {
            return it.key();
        }
    }

    return -1;
}

void NetplayCoordinator::recreatePeerConnection(int slot)
{
    QString peerId;
    for (const auto& player : getPlayerList()) {
        if (player.slot == slot) {
            peerId = player.id;
            break;
        }
    }

    if (peerId.isEmpty()) {
        return;
    }

    qInfo() << "NetplayCoordinator: Recreating WebRTC peer for slot" << slot;

    if (m_lockstepEngine) {
        m_lockstepEngine->setDataChannel(slot, nullptr);
        m_lockstepEngine->wakeInputWaiters();
    }

    if (m_peers.contains(slot)) {
        auto oldPeer = m_peers[slot];
        m_peers.remove(slot);
        if (oldPeer) {
            oldPeer->close();
        }
    }

    const bool initiator = m_gameSession.localSlot < slot;
    auto peer = std::make_shared<WebRTCPeer>(peerId, initiator, this);
    m_peers[slot] = peer;
    bindWebRTCPeerSignals(peer, peerId);

    if (initiator) {
        peer->createDataChannel(QStringLiteral("RMG-Input"));
        createPeerOffer(slot);
    }
}

void NetplayCoordinator::recoverWebRTCPeerConnections()
{
    if (m_state != InGame) {
        return;
    }

    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        const auto& peer = it.value();
        if (!peer) {
            continue;
        }

        const auto state = peer->getConnectionState();
        if (state == WebRTCPeer::Connected) {
            continue;
        }

        if (peer->isInitiator()) {
            peer->attemptRecovery();
        }
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
    qWarning() << "NetplayCoordinator: WebRTC connection degraded for peer" << peerId << ":" << reason
               << "- continuing via signaling relay";

    const int slot = findPeerSlotById(peerId);
    if (slot < 0) {
        return;
    }

    if (m_lockstepEngine) {
        m_lockstepEngine->setDataChannel(slot, nullptr);
        // Inputs still arrive over signaling; do not treat WebRTC loss as disconnect.
        m_lockstepEngine->wakeInputWaiters();
    }

    if (m_state == InGame) {
        const auto peerIt = m_peers.find(slot);
        if (peerIt != m_peers.end() && peerIt.value() && peerIt.value()->isInitiator()) {
            peerIt.value()->attemptRecovery();
        }
    }
}

void NetplayCoordinator::on_webRTC_dataChannelOpened(const QString& peerId, const QString& label)
{
    qDebug() << "NetplayCoordinator: WebRTC data channel opened for peer" << peerId << "label:" << label;

    const int peerSlot = findPeerSlotById(peerId);
    if (peerSlot < 0) {
        return;
    }

    if (label == QStringLiteral("RMG-Input")) {
        const auto peerIt = m_peers.constFind(peerSlot);
        if (peerIt != m_peers.constEnd() && peerIt.value()) {
            const auto channel = peerIt.value()->getDataChannel(label);
            if (channel && channel->isOpen() && !m_lockstepEngine) {
                attachInputChannelForHandshake(peerSlot, channel);
            }
        }
        evaluateEmulationStartReadiness();
    }

    if (!m_lockstepEngine) {
        qDebug() << "NetplayCoordinator: Deferring data channel attach for slot"
                 << peerSlot << "until LockstepEngine is initialized";
        return;
    }

    attachPeerDataChannelToLockstep(peerSlot, label);
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
            // Always broadcast, including an empty list, so clients can wipe leftovers.
            m_server->broadcastSaveSync(m_gameSession.roomId, saveFiles);
            if (saveFiles.isEmpty()) {
                qDebug() << "NetplayCoordinator: Broadcasting empty save sync for this ROM";
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

    // Host applies locally here too so M64CMD_EXECUTE always sees the same
    // CountPerOp / SiDmaDuration / CPU as clients, even if dialog sync raced.
    CoreNetplaySyncSettings settings;
    if (coreSettingsFromJson(coreSettings, settings)) {
        CoreSetNetplaySyncSettings(settings);
    }

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
    m_lockstepConfig.stallTimeoutMilliseconds = 0;
    CoreSetEmbeddedNetplayInputDelayFrames(frames);
    if (auto engine = activeLockstepEngine()) {
        engine->setInputDelayFrames(frames);
        engine->wakeInputWaiters();
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
    if (coreSettingsFromJson(coreSettings, settings)) {
        CoreSetNetplaySyncSettings(settings);
    } else {
        qWarning() << "NetplayCoordinator: Received core settings sync with invalid payload";
    }

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
    int resolvedSlot = slot;
    if (resolvedSlot == -1 && m_lockstepConfig.numPlayers == 2) {
        resolvedSlot = (m_gameSession.localSlot == 0) ? 1 : 0;
    }

    if (resolvedSlot >= 0 &&
        resolvedSlot != m_gameSession.localSlot &&
        m_inputHandshakeActive &&
        !m_emulationReadySent &&
        frameNumber <= kInputHandshakeFrame + static_cast<uint32_t>(m_lockstepConfig.inputDelayFrames)) {
        recordInputHandshake(resolvedSlot, frameNumber, controllerState);
    }

    const auto engine = activeLockstepEngine();
    if (!engine) {
        return;
    }

    if (resolvedSlot == m_gameSession.localSlot) {
        return;
    }

    if (resolvedSlot < 0 || resolvedSlot >= m_lockstepConfig.numPlayers) {
        return;
    }

    // While WebRTC is up for this peer, signaling inputs are backup-only. Applying
    // them gap-fills ahead of the P2P stream and desyncs both sides.
    if (engine->hasOpenDataChannelForPeer(resolvedSlot)) {
        return;
    }

    engine->submitRemoteInput(resolvedSlot, frameNumber, controllerState);
}

void NetplayCoordinator::on_peerFrameSyncReceived(int slot, uint32_t frameNumber, uint32_t stateHash)
{
    const auto engine = activeLockstepEngine();
    if (!engine) {
        return;
    }

    if (slot == m_gameSession.localSlot) {
        return;
    }

    if (slot >= 0 && slot < m_lockstepConfig.numPlayers) {
        engine->submitPeerFrameSync(slot, frameNumber, stateHash);
    } else if (slot == -1 && m_lockstepConfig.numPlayers == 2) {
        const int inferredSlot = (m_gameSession.localSlot == 0) ? 1 : 0;
        engine->submitPeerFrameSync(inferredSlot, frameNumber, stateHash);
    }
}

void NetplayCoordinator::maybeSubmitCompletedFrameSync(uint32_t completedFrame)
{
    if (completedFrame == 0) {
        return;
    }

    // ~1 Hz at 60 FPS; compare state hashes at the same lockstep frame.
    const uint32_t syncInterval = static_cast<uint32_t>(
        std::max(60, m_lockstepConfig.resyncCheckIntervalFrames));

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (completedFrame % syncInterval != 0 ||
            completedFrame == m_lastBroadcastFrameSync) {
            return;
        }
    }

    const auto engine = activeLockstepEngine();
    if (!engine) {
        return;
    }

    const uint32_t stateHash = CoreGetNetplayFrameSyncHash();
    if (stateHash == 0) {
        return;
    }

    // Sampled on the emulation thread at a lockstep barrier; hop to the
    // coordinator thread only for the Qt/network broadcast.
    if (QThread::currentThread() == thread()) {
        broadcastFrameSync(engine, completedFrame, stateHash);
        return;
    }

    QMetaObject::invokeMethod(
        this,
        [this, engine, completedFrame, stateHash]() {
            broadcastFrameSync(engine, completedFrame, stateHash);
        },
        Qt::QueuedConnection);
}

void NetplayCoordinator::queueFrameSyncCheck(uint32_t frameNumber)
{
    // Kept for ABI with older call sites; hashing now happens in
    // maybeSubmitCompletedFrameSync() at the lockstep boundary.
    Q_UNUSED(frameNumber);
}

void NetplayCoordinator::broadcastFrameSync(
    const std::shared_ptr<RMGCore::LockstepEngine>& engine,
    uint32_t frameNumber,
    uint32_t stateHash)
{
    if (!engine || frameNumber == 0 || stateHash == 0) {
        return;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (frameNumber == m_lastBroadcastFrameSync) {
            return;
        }
        m_lastBroadcastFrameSync = frameNumber;
    }

    engine->recordLocalFrameSync(frameNumber, stateHash);

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

void NetplayCoordinator::clearRoomSessionState()
{
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        if (it.value()) {
            it.value()->close();
        }
    }
    m_peers.clear();
    m_cachedPlayers.clear();
    m_playerPingMs.clear();
    m_gameSession = GameSession();
    m_isRoomHost = false;
    m_autoJoinRoomData = QJsonObject();

    // Immediately clear lobby UI state instead of waiting for server callbacks.
    emit playersUpdated(QList<SocketIOClient::PlayerInfo>());
    emit playerPingsUpdated();
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

    if (!TurnCredentialClient::instance().ensureCredentials(15000)) {
        qWarning() << "NetplayCoordinator: Cloudflare TURN credentials unavailable; WebRTC may fail behind NAT";
    }

    // Preserve existing WebRTC sessions across controller-port remaps. Peers are
    // identified by stable playerId; m_peers is only keyed by the current slot.
    QHash<QString, std::shared_ptr<WebRTCPeer>> peersById;
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        if (it.value()) {
            peersById.insert(it.value()->getPeerId(), it.value());
        }
    }
    m_peers.clear();

    for (const auto& player : players) {
        if (player.slot < 0 || player.slot == m_gameSession.localSlot || player.id.isEmpty()) {
            continue;
        }

        if (auto existing = peersById.take(player.id)) {
            m_peers.insert(player.slot, existing);
            continue;
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

    // Close peers for players who left the lobby.
    for (auto it = peersById.begin(); it != peersById.end(); ++it) {
        if (it.value()) {
            it.value()->close();
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

void NetplayCoordinator::resetEmulationStartPrep()
{
    m_emulationReadyRequested = false;
    m_emulationReadySent = false;
    m_inputHandshakeActive = false;
    m_inputChannelWaitExpired = false;
    m_inputHandshakeForced = false;
    m_inputHandshakeReceivedSlots.clear();
    m_inputHandshakeInputs.clear();
    if (m_emulationPrepTimer) {
        m_emulationPrepTimer->stop();
    }
}

int NetplayCoordinator::countRequiredRemotePeers() const
{
    int required = 0;
    const QList<SocketIOClient::PlayerInfo> players = getPlayerList();
    for (const auto& player : players) {
        if (player.slot < 0 || player.slot == m_gameSession.localSlot) {
            continue;
        }
        ++required;
    }
    return required;
}

bool NetplayCoordinator::areRemoteInputChannelsReady() const
{
    const QList<SocketIOClient::PlayerInfo> players = getPlayerList();
    bool sawRemote = false;

    for (const auto& player : players) {
        if (player.slot < 0 || player.slot == m_gameSession.localSlot) {
            continue;
        }

        sawRemote = true;
        const auto peerIt = m_peers.constFind(player.slot);
        if (peerIt == m_peers.constEnd() || !peerIt.value()) {
            return false;
        }

        const auto channel = peerIt.value()->getDataChannel(QStringLiteral("RMG-Input"));
        if (!channel || !channel->isOpen()) {
            return false;
        }
    }

    return sawRemote;
}

bool NetplayCoordinator::isInputHandshakeComplete() const
{
    if (m_inputHandshakeForced) {
        return true;
    }

    const QList<SocketIOClient::PlayerInfo> players = getPlayerList();
    for (const auto& player : players) {
        if (player.slot < 0 || player.slot == m_gameSession.localSlot) {
            continue;
        }

        if (!m_inputHandshakeReceivedSlots.contains(player.slot)) {
            return false;
        }
    }

    return countRequiredRemotePeers() > 0;
}

void NetplayCoordinator::beginInputHandshake()
{
    if (m_inputHandshakeActive) {
        sendInputHandshakeBurst();
        return;
    }

    qInfo() << "NetplayCoordinator: Starting pre-start input handshake";
    m_inputHandshakeActive = true;
    sendInputHandshakeBurst();

    if (m_emulationPrepTimer && !m_emulationPrepTimer->isActive()) {
        m_emulationPrepTimer->start();
    }
}

void NetplayCoordinator::attachInputChannelForHandshake(
    int peerSlot,
    const std::shared_ptr<WebRTCDataChannel>& channel)
{
    if (!channel || peerSlot < 0 || peerSlot == m_gameSession.localSlot) {
        return;
    }

    channel->onBinaryMessageReceived =
        [this, peerSlot](const std::vector<uint8_t>& data) {
            if (data.size() != 8) {
                return;
            }

            uint32_t frameNumber = 0;
            uint32_t controllerState = 0;
            std::memcpy(&frameNumber, data.data(), 4);
            std::memcpy(&controllerState, data.data() + 4, 4);

            if (m_inputHandshakeActive && !m_emulationReadySent) {
                recordInputHandshake(peerSlot, frameNumber, controllerState);
            }
        };
}

void NetplayCoordinator::sendInputHandshakeBurst()
{
    relayLocalControllerInput(kInputHandshakeFrame, kInputHandshakeState);

    std::vector<uint8_t> packet(8);
    std::memcpy(packet.data(), &kInputHandshakeFrame, 4);
    std::memcpy(packet.data() + 4, &kInputHandshakeState, 4);

    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        if (it.key() == m_gameSession.localSlot || !it.value()) {
            continue;
        }

        const auto channel = it.value()->getDataChannel(QStringLiteral("RMG-Input"));
        if (channel && channel->isOpen()) {
            (void)channel->sendBinary(packet);
        }
    }
}

void NetplayCoordinator::recordInputHandshake(
    int slot,
    uint32_t frameNumber,
    uint32_t controllerState)
{
    if (slot < 0 ||
        slot >= m_lockstepConfig.numPlayers ||
        slot == m_gameSession.localSlot) {
        return;
    }

    if (frameNumber > kInputHandshakeFrame + static_cast<uint32_t>(m_lockstepConfig.inputDelayFrames)) {
        return;
    }

    if (!m_inputHandshakeReceivedSlots.contains(slot)) {
        qDebug() << "NetplayCoordinator: Input handshake received from slot" << slot
                 << "frame" << frameNumber;
    }

    m_inputHandshakeReceivedSlots.insert(slot);
    m_inputHandshakeInputs[slot] = controllerState;
    evaluateEmulationStartReadiness();
}

void NetplayCoordinator::requestEmulationReadyWhenPrepared()
{
    m_emulationReadyRequested = true;

    if (countRequiredRemotePeers() == 0) {
        if (!m_emulationReadySent) {
            sendEmulationReady();
            m_emulationReadySent = true;
        }
        return;
    }

    if (m_emulationPrepTimer && !m_emulationPrepTimer->isActive()) {
        m_emulationPrepTimer->start();
    }

    evaluateEmulationStartReadiness();
}

void NetplayCoordinator::evaluateEmulationStartReadiness()
{
    if (!m_emulationReadyRequested || m_emulationReadySent) {
        return;
    }

    if (countRequiredRemotePeers() == 0) {
        sendEmulationReady();
        m_emulationReadySent = true;
        if (m_emulationPrepTimer) {
            m_emulationPrepTimer->stop();
        }
        return;
    }

    if (!areRemoteInputChannelsReady()) {
        if (!m_inputChannelWaitExpired &&
            m_gameStartPrepTimer.isValid() &&
            m_gameStartPrepTimer.elapsed() >= kWebRtcInputChannelWaitMs) {
            qWarning() << "NetplayCoordinator: WebRTC input channels not ready after"
                       << kWebRtcInputChannelWaitMs
                       << "ms; continuing via signaling relay";
            m_inputChannelWaitExpired = true;
        }

        if (!m_inputChannelWaitExpired) {
            return;
        }
    }

    if (!m_inputHandshakeActive) {
        beginInputHandshake();
        return;
    }

    if (!isInputHandshakeComplete()) {
        if (!m_inputHandshakeForced &&
            m_gameStartPrepTimer.isValid() &&
            m_gameStartPrepTimer.elapsed() >= kInputHandshakeTimeoutMs) {
            qWarning() << "NetplayCoordinator: Input handshake timed out after"
                       << kInputHandshakeTimeoutMs
                       << "ms; proceeding with partial handshake";
            m_inputHandshakeForced = true;
        } else if (!m_inputHandshakeForced) {
            sendInputHandshakeBurst();
            return;
        }
    }

    qInfo() << "NetplayCoordinator: Pre-start input paths ready; sending emulation-ready";
    sendEmulationReady();
    m_emulationReadySent = true;
    if (m_emulationPrepTimer) {
        m_emulationPrepTimer->stop();
    }
}

void NetplayCoordinator::rebroadcastSessionSync()
{
    if (!isHostingServer() || m_gameSession.roomId.isEmpty() || !m_server) {
        return;
    }

    qInfo() << "NetplayCoordinator: Rebroadcasting session cheats/saves/core settings";
    sendCheatsUpdate(m_sessionSyncCheats);
    sendSaveSync(m_sessionSyncSaves);
    if (!m_sessionSyncCoreSettings.isEmpty()) {
        sendCoreSettingsSync(m_sessionSyncCoreSettings);
    }
}

bool NetplayCoordinator::hasAppliedCoreSettingsSync() const
{
    return CoreHasNetplaySyncSettings();
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
    const auto engine = activeLockstepEngine();

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (slot >= 0 && slot < m_lockstepConfig.numPlayers) {
        const auto cached = m_currentFrameInputs.find(slot);
        if (cached != m_currentFrameInputs.end()) {
            return cached->second;
        }
    }

    if (!engine || slot < 0 || slot >= m_lockstepConfig.numPlayers) {
        return 0;
    }

    const uint32_t completedFrame =
        engine->getCurrentFrameNumber() > 0
            ? engine->getCurrentFrameNumber() - 1
            : 0;
    const auto inputs = engine->getFrameInputs(completedFrame);
    const auto it = inputs.find(slot);
    return it != inputs.end() ? it->second : 0;
}