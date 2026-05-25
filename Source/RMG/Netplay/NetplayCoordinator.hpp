/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
 #ifndef NETPLAYCOORDINATOR_HPP
 #define NETPLAYCOORDINATOR_HPP
 
 #include "NatTraversal/NatTraversalProtocol.hpp"
 #include "SocketIO/SocketIOClient.hpp"
 #include "SocketIO/SocketIOServer.hpp"
 #include "WebRTC/WebRTCPeer.hpp"
 
 #include <RMG-Core/Netplay/LockstepEngine.hpp>
 #include <QString>
 #include <QObject>
 #include <QMap>
 #include <QList>
 #include <QJsonArray>
 #include <memory>
 #include <mutex>
 
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
         const QString& serverUrl = "http://localhost:2626",
         QObject* parent = nullptr
     );
     ~NetplayCoordinator();
 
     // Hosting logic
     bool startHosting(int port = kDefaultNetplayHostingPort, const QString& playerName = "Player", const QString& gameName = "Unknown");
     void stopHosting();
     bool isHostingServer() const;
     SocketIOServer* getHostingServer() { return m_server.get(); }
 
     // Session Lifecycle
     void connectToServer(const QString& playerName);
     void connectToDirectIPServer(const QString& ipAddress, int port, const QString& playerName,
                                  const QString& roomId = QString());
     void createRoom(const QString& roomName, const QString& gameId = "ssb64", int maxPlayers = 4);
     void joinRoom(const QString& roomId, bool asSpectator = false, const QString& password = "");
     void leaveRoom();
     void startGame(const QString& gameMode = "lockstep", bool resyncEnabled = false, const QString& romHash = "");
     void endGame();
 
     // Game Input & Emulation
     void submitFrameInput(uint32_t controllerState);
     bool advanceFrame();
     void verifyGameSync(uint32_t romChecksum);
 
     // Dialog & UI compatibility
     void requestRoomList();
     void setPlayerName(const QString& name);
     void sendChatMessage(const QString& message);
     void sendCheatsUpdate(const QJsonArray& cheats);
     void sendSaveSync(const QJsonArray& saveFiles);
     
     // Input Delay (In-Game support)
     void setInputDelayFrames(int frames);
     int getInputDelayFrames() const;
     void sendInputDelayUpdate(int frames);

    void sendEmulationPauseUpdate(bool paused);
 
     // State Queries
     State getCurrentState() const;
     QString getCurrentStateString() const;
     GameSession getGameSession() const;
     QList<SocketIOClient::PlayerInfo> getPlayerList() const;
     bool isHost() const;
     bool isInGame() const;
 
     // Network Info
     QString getPeerAddress() const;
     int getGamePort() const;
 
     // Components
     SocketIOClient* getSocketIOClient() { return m_socketIO.get(); }
     RMGCore::LockstepEngine* getLockstepEngine() { return m_lockstepEngine.get(); }
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
 
     void resyncAttempted();
     void resyncSucceeded();
     void resyncFailed(const QString& reason);
     
 private slots:
     void on_socketIO_connected();
     void on_socketIO_disconnected();
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
     void on_socketIO_controllerInputReceived(int slot, uint32_t frameNumber, uint32_t controllerState);
     void on_socketIO_inputDelayReceived(int frames);
    void on_socketIO_emulationPauseReceived(bool paused);
     void on_peerFrameSyncReceived(int slot, uint32_t frameNumber);

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
     void setState(State newState);
     void setupPeerConnections(const QList<SocketIOClient::PlayerInfo>& players);
     void initializeLockstepEngine();
     void broadcastFrameSyncIfNeeded(uint32_t frameNumber);
 
     // Internal components
     std::unique_ptr<SocketIOClient> m_socketIO;
     std::unique_ptr<SocketIOServer> m_server;
     std::unique_ptr<RMGCore::LockstepEngine> m_lockstepEngine;
     QMap<int, std::shared_ptr<WebRTCPeer>> m_peers;
     QString m_playerId;

     // State data
     State m_state;
     GameSession m_gameSession;
     QString m_playerName;
     QList<SocketIOClient::PlayerInfo> m_cachedPlayers;
     bool m_shouldAutoJoinRoom = false;
     QString m_autoJoinRoomId;
     QJsonObject m_autoJoinRoomData;
 
     // CRITICAL: Scoped configuration type
     RMGCore::LockstepEngine::Config m_lockstepConfig;

     std::map<int, uint32_t> m_currentFrameInputs;

     QJsonArray m_sessionSyncCheats;
     QJsonArray m_sessionSyncSaves;
     
     mutable std::recursive_mutex m_mutex;
 };
 
 } // namespace UserInterface::Netplay
 
 #endif // NETPLAYCOORDINATOR_HPP