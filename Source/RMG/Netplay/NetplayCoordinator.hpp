/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
 #ifndef NETPLAYCOORDINATOR_HPP
 #define NETPLAYCOORDINATOR_HPP
 
 #include "NetplayProtocol.hpp"
 #include "SocketIO/SocketIOClient.hpp"
 #include "SocketIO/SocketIOServer.hpp"
 #include "WebRTC/WebRTCPeer.hpp"
 #include "NetplayTraversalClient.hpp"
 
 #include <RMG-Core/Netplay/LockstepEngine.hpp>
 #include <QString>
 #include <QObject>
 #include <QMap>
 #include <QList>
 #include <QJsonArray>
 #include <memory>
 #include <mutex>
#include <atomic>
#include <utility>
#include <vector>
 
 namespace UserInterface::Netplay {
 
 class NetplayCoordinator : public QObject {
     Q_OBJECT
 
 public:
     enum State {
         Idle,
         Connecting,
         Connected,
         CreatingRoom,
         JoiningRoom,
         InLobby,
         StartingGame,
         InGame,
         EndingGame,
         Error
     };
 
     struct GameSession {
         QString roomId;
         QString matchId;
         QString gameMode;
         bool resyncEnabled;
         QString romHash;
         int localSlot;
         int numPlayers;
     };
 
     explicit NetplayCoordinator(
         const QString& serverUrl = "udp://localhost:2626",
         QObject* parent = nullptr
     );
     ~NetplayCoordinator();
 
     // Hosting logic
     bool startHosting(int port = kDefaultNetplayHostingPort, const QString& playerName = "Player", const QString& gameName = "Unknown");
     void stopHosting();
     bool isHostingServer() const;
     QString lastHostingError() const { return m_lastHostingError; }
     SocketIOServer* getHostingServer() { return m_server.get(); }
 
     // Session Lifecycle
     void connectToServer(const QString& playerName);
     void connectToDirectIPServer(const QString& ipAddress, int port, const QString& playerName,
                                  const QString& roomId = QString());
     void connectViaNatTraversal(const QString& hostCode, const QString& playerName,
                                 const QString& roomId = QString());
     void cancelNatTraversal();
     void createRoom(const QString& roomName, const QString& gameId = "ssb64", int maxPlayers = 4);
     void joinRoom(const QString& roomId, bool asSpectator = false, const QString& password = "");
     void leaveRoom();
     void startGame(const QString& gameMode = "lockstep", bool resyncEnabled = false, const QString& romHash = "");
     void endGame();
 
     // Game Input & Emulation
     void beginEmulationSync();
     /**
      * Exchange frame-0 / input-delay window inputs with every peer before the
      * ROM boots so lockstep does not start from dropped early packets.
      */
     bool synchronizeLockstepFrameZero(int timeoutMilliseconds = 15000);
     void resetEmulationSync();
     void submitFrameInput(uint32_t controllerState);
     bool advanceFrame();
     /** Sample CPU hash and broadcast frame sync after the emulated frame completes. */
     void submitEndOfFrameSync();
     void verifyGameSync(uint32_t romChecksum);
 
     // Dialog & UI compatibility
     void requestRoomList();
     void setPlayerName(const QString& name);
     void sendChatMessage(const QString& message);
     void sendCheatsUpdate(const QJsonArray& cheats);
     void sendSaveSync(const QJsonArray& saveFiles);
     void sendCoreSettingsSync(const QJsonObject& coreSettings);
     
     // Input Delay (In-Game support)
     void setInputDelayFrames(int frames);
     int getInputDelayFrames() const;
     void sendInputDelayUpdate(int frames);

    void sendEmulationPauseUpdate(bool paused);
    void sendEmulationReady();
 
     // State Queries
     State getCurrentState() const;
     QString getCurrentStateString() const;
     GameSession getGameSession() const;
     QList<SocketIOClient::PlayerInfo> getPlayerList() const;
     int getPlayerPing(int slot) const;
     bool isHost() const;
     bool isInGame() const;
 
     // Network Info
     QString getPeerAddress() const;
     int getGamePort() const;
 
     // Components
     SocketIOClient* getSocketIOClient() { return m_socketIO.get(); }
     RMGCore::LockstepEngine* getLockstepEngine() { return m_lockstepEngine ? m_lockstepEngine.get() : nullptr; }
     QJsonObject getAutoJoinRoomData() const { return m_autoJoinRoomData; }
    
     // Missing method declarations
     void onDesyncDetected(const QString& reason);
     void createPeerOffer(int slot);
     void handlePeerAnswer(int slot, const QString& answer);
     void addICECandidate(int slot, const QString& candidate, int mLineIndex);
     uint32_t getSyncedInput(int slot);

 signals:
     void stateChanged(State newState);
     void connected();
     void disconnected();
     void connectionError(const QString& error);
     void inputDelayChanged(int frames);

     void motdReceived(const QString& motd);

     void roomCreated(const QString& roomId, int slot);
     void roomJoined(const QString& roomId, int slot);
     void playersUpdated(const QList<SocketIOClient::PlayerInfo>& players);
     void playerPingsUpdated();
     void roomClosed(const QString& reason);
 
     void gameStarted(const GameSession& session);
     void gameFrameReady(uint32_t frameNumber, const QMap<int, uint32_t>& inputs);
     void gameEnded();
 
     void desyncDetected(const QString& reason);
     void peerInputStalled(int playerSlot, uint32_t frameNumber);
     void peerConnected(int slot);
     void peerDisconnected(int slot);
 
     void roomsUpdated();
     void chatMessageReceived(const QString& playerName, const QString& message);
     void cheatsUpdated(const QJsonArray& cheats);
     void saveSyncReceived(const QJsonArray& saveFiles);
     void coreSettingsSyncReceived(const QJsonObject& coreSettings);
     void emulationBeginReceived();
 
     void resyncAttempted();
     void resyncSucceeded();
     void resyncFailed(const QString& reason);
     
 private slots:
     void on_socketIO_connected();
     void on_socketIO_disconnected();
     void on_socketIO_reconnecting();
     void on_socketIO_reconnected();
     void on_socketIO_connectionError(const QString& error);
     void on_socketIO_roomCreated(const QString& roomId);
     void on_socketIO_roomJoined(const QString& roomId, int slotIndex);
     void on_socketIO_roomLeft();
     void on_socketIO_roomClosed(const QString& reason);
     void on_socketIO_playersUpdated(const QList<SocketIOClient::PlayerInfo>& players);
     void on_socketIO_roomsListed(const QJsonArray& rooms);
     void on_socketIO_gameStarted(const QString& mode, bool resync, const QString& matchId);
     void on_socketIO_gameEnded();
     void on_socketIO_cheatsUpdated(const QJsonArray& cheats);
     void on_socketIO_saveSyncReceived(const QJsonArray& saveFiles);
     void on_socketIO_coreSettingsSyncReceived(const QJsonObject& coreSettings);
     void on_socketIO_controllerInputReceived(int slot, uint32_t frameNumber, uint32_t controllerState);
     void on_socketIO_inputDelayReceived(int frames);
     void on_socketIO_emulationBeginReceived();
     void relayLocalControllerInput(quint32 sendFrameNumber, quint32 state);
     void flushPendingControllerRelay();
     void relayLocalControllerInputBurst(
         quint32 startFrameNumber,
         quint32 endFrameNumber,
         quint32 state);
     void bufferEarlyRemoteInput(int slot, uint32_t frameNumber, uint32_t controllerState);
     void flushEarlyRemoteInputs(
         const std::shared_ptr<RMGCore::LockstepEngine>& engine);
     void rebroadcastLocalInputBuffer(
         const std::shared_ptr<RMGCore::LockstepEngine>& engine);
     void on_socketIO_emulationPauseReceived(bool paused);
     void on_peerFrameSyncReceived(int slot, uint32_t frameNumber, uint32_t stateHash);

     void on_lockstep_frameReady(uint32_t frameNumber, const QMap<int, uint32_t>& inputs);
     void on_lockstep_peerStalled(int playerSlot, uint32_t frameNumber);
     void on_lockstep_desyncDetected(uint32_t frameNumber, const QString& reason);

     void on_socketIO_offerReceived(const QString& fromPlayerId, const QString& sdpOffer);
     void on_socketIO_answerReceived(const QString& fromPlayerId, const QString& sdpAnswer);
     void on_socketIO_iceCandidateReceived(const QString& fromPlayerId, const QString& candidate, int mLineIndex);
 
     void on_webRTC_connectionEstablished(const QString& peerId);
     void on_webRTC_connectionFailed(const QString& peerId, const QString& reason);
     void on_webRTC_dataChannelOpened(const QString& peerId, const QString& label);
 
 private:
    void clearRoomSessionState();
     void setState(State newState);
     void setupPeerConnections(const QList<SocketIOClient::PlayerInfo>& players);
     void synchronizeLockstepPlayerCount();
     void syncLockstepPeerSessionActive();
     void initializeLockstepEngine();
     void applyPlayerPings(const QJsonArray& pings);
     void queueFrameSyncCheck(uint32_t frameNumber);
     void broadcastFrameSync(
         const std::shared_ptr<RMGCore::LockstepEngine>& engine,
         uint32_t frameNumber,
         uint32_t stateHash);
     void bindWebRTCPeerSignals(const std::shared_ptr<WebRTCPeer>& peer, const QString& peerId);
     void attachPeerDataChannelToLockstep(int peerSlot, const QString& label);
     void attachExistingPeerDataChannels();
     void recoverWebRTCPeerConnections();
     void recreatePeerConnection(int slot);
     int findPeerSlotById(const QString& peerId) const;
     void connectSocketIOClientSignals(SocketIOClient* client);
     void sendWebRTCOffer(const QString& targetPlayerId, const QString& sdpOffer);
     void sendWebRTCAnswer(const QString& targetPlayerId, const QString& sdpAnswer);
     void sendWebRTCIceCandidate(const QString& targetPlayerId, const QString& candidate, int mLineIndex);
     void on_hostedWebRTCSignalReceived(const QString& fromPlayerId, const QJsonObject& signal);
     std::shared_ptr<RMGCore::LockstepEngine> activeLockstepEngine();
 
     // Internal components
     std::unique_ptr<SocketIOClient> m_socketIO;
     std::unique_ptr<SocketIOServer> m_server;
     std::unique_ptr<NetplayTraversalClient> m_traversalClient;
     QString m_lastHostingError;
     std::shared_ptr<RMGCore::LockstepEngine> m_lockstepEngine;
     QMap<int, std::shared_ptr<WebRTCPeer>> m_peers;
     QString m_playerId;

     // State data
     State m_state;
     GameSession m_gameSession;
     QString m_playerName;
     QList<SocketIOClient::PlayerInfo> m_cachedPlayers;
     QMap<int, int> m_playerPingMs;
     bool m_shouldAutoJoinRoom = false;
     QString m_autoJoinRoomId;
     QJsonObject m_autoJoinRoomData;
 
     // CRITICAL: Scoped configuration type
     RMGCore::LockstepEngine::Config m_lockstepConfig;

     std::map<int, uint32_t> m_currentFrameInputs;

     QJsonArray m_sessionSyncCheats;
     QJsonArray m_sessionSyncSaves;
     QJsonObject m_sessionSyncCoreSettings;
     uint32_t m_lastBroadcastFrameSync = 0;
    std::atomic<uint32_t> m_pendingFrameSyncFrame{0};
    std::atomic<bool> m_pumpNetworkQueued{false};
    std::atomic<bool> m_relayInputQueued{false};
    std::mutex m_relayQueueMutex;
    std::vector<std::pair<quint32, quint32>> m_pendingRelayQueue;
    struct EarlyRemoteInput {
        int slot = -1;
        uint32_t frameNumber = 0;
        uint32_t controllerState = 0;
    };
    std::mutex m_earlyRemoteInputMutex;
    std::vector<EarlyRemoteInput> m_earlyRemoteInputs;
     
     mutable std::recursive_mutex m_mutex;
 };
 
 } // namespace UserInterface::Netplay
 
 #endif // NETPLAYCOORDINATOR_HPP