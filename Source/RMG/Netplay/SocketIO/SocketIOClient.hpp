/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef SOCKETIOCLIENT_HPP
#define SOCKETIOCLIENT_HPP

#include <QString>
#include <QMap>
#include <QWebSocket>
#include <QObject>
#include <QUrl>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
#include <functional>
#include <memory>

namespace UserInterface::Netplay {

/**
 * @class SocketIOClient
 * @brief Socket.IO client for WebRTC signaling server communication
 * 
 * Handles:
 * - Connection to Socket.IO signaling server
 * - Room creation/joining/leaving
 * - WebRTC offer/answer/ICE candidate relay
 * - Player state management
 * - Game state events
 */
class SocketIOClient : public QObject {
    Q_OBJECT

public:
    enum ConnectionState {
        Disconnected,
        Connecting,
        Connected,
        Error
    };

    enum RoomState {
        None,
        Lobbying,
        GameStarted,
        GameEnded
    };

    struct PlayerInfo {
        QString id;
        QString name;
        int slot;
        bool isSpectator;
        bool isReady;
    };

    struct RoomInfo {
        QString roomId;
        QString roomName;
        QString ownerName;
        int maxPlayers = 4;
        int currentPlayers = 0;
        int spectatorCount = 0;
        QString gameId;
        int localSlot = -1;
        RoomState state = None;
        QList<PlayerInfo> players;
    };

    struct GameConfig {
        QString mode;          // "lockstep" or "streaming"
        bool resyncEnabled;
        QString romHash;
    };

    // Constructor/Destructor
    explicit SocketIOClient(const QString& serverUrl = "http://localhost:2626", QObject* parent = nullptr);
    ~SocketIOClient();

    // Connection Management
    void connectToServer(const QString& playerName);
    void disconnect();
    ConnectionState getConnectionState() const;

    // Room Management
    void openRoom(const QString& roomName, const QString& gameId, int maxPlayers = 4);
    void joinRoom(const QString& roomId, bool asSpectator = false);
    void leaveRoom();
    void setPlayerName(const QString& name);
    void claimSlot(int slot);

    // Game Control
    void startGame(const QString& mode, bool resyncEnabled, const QString& romHash);
    void endGame();
    void setGameMode(const QString& mode);
    void sendControllerInput(uint32_t frameNumber, uint32_t controllerState);
    void sendFrameSync(uint32_t frameNumber);

    // WebRTC Signaling
    void sendOffer(const QString& targetPlayerId, const QString& sdpOffer);
    void sendAnswer(const QString& targetPlayerId, const QString& sdpAnswer);
    void sendICECandidate(const QString& targetPlayerId, const QString& candidate, int sdpMLineIndex = 0);

    // ROM Sharing
    void setROMSharingEnabled(bool enabled);
    void declareROMReady(bool ready);
    void declareROMInfo(const QString& fileName, const QString& hash, uint32_t fileSize);

    // Game Data Upload
    void sendSyncLog(const QString& matchId, const QJsonArray& entries, const QJsonObject& summary);
    void sendDebugLog(const QString& matchId, const QString& logContent);

    // Chat
    void sendChatMessage(const QString& message);

    // Cheats sync
    void sendCheatsUpdate(const QJsonArray& cheats);

    // Save sync
    void sendSaveSync(const QJsonArray& saveFiles);

    // Room List
    void requestRoomList(bool waiting = false);
    
    void sendInputDelayUpdate(int frames);

    // Getters
    QString getPlayerId() const;
    QString getRoomId() const;
    RoomInfo getCurrentRoom() const;
    QList<PlayerInfo> getPlayerList() const;
    GameConfig getGameConfig() const;

    // Event Callbacks (use signals instead for Qt integration)

signals:
    // Connection signals
    void connected();
    void disconnected();
    void connectionError(const QString& error);

    // Room signals
    void roomCreated(const QString& roomId);
    void roomJoined(const QString& roomId, int slotIndex);
    void roomLeft();
    void roomClosed(const QString& reason);
    void playersUpdated(const QList<PlayerInfo>& players);
    void spectatorCountUpdated(int count);
    void roomsListed(const QJsonArray& rooms);  // Response from list-rooms request

    // Game signals
    void gameStarted(const QString& mode, bool resyncEnabled, const QString& matchId);
    void gameEnded();
    void gameModeChanged(const QString& mode);
    void controllerInputReceived(int slot, uint32_t frameNumber, uint32_t controllerState);
    void frameSyncReceived(int slot, uint32_t frameNumber);

    // WebRTC Signaling signals
    void offerReceived(const QString& fromPlayerId, const QString& sdpOffer);
    void answerReceived(const QString& fromPlayerId, const QString& sdpAnswer);
    void iceCandidateReceived(const QString& fromPlayerId, const QString& candidate, int sdpMLineIndex);

    // ROM Management signals
    void romSharingUpdated(bool enabled);
    void playerROMReady(const QString& playerId);
    void romDeclared(const QString& playerId, const QString& fileName, const QString& hash);

    // Upload Token signals
    void uploadTokenReceived(const QString& token);
    void reconnectTokenReceived(const QString& token);

    // Chat signals
    void chatMessageReceived(const QString& playerName, const QString& message);

    // Cheats signals
    void cheatsUpdated(const QJsonArray& cheats);

    // Save sync signals
    void saveSyncReceived(const QJsonArray& saveFiles);

    // Input delay sync
    void inputDelayReceived(int frames);

private slots:
    void on_connected();
    void on_disconnected();
    void on_textMessageReceived(const QString& message);
    void on_error(QAbstractSocket::SocketError error);
    void on_sslErrors(const QList<QSslError>& errors);
    void on_pong(quint64 elapsedTime, const QByteArray& payload);

private:
    // Socket.IO message handling
    void handleSocketIOMessage(const QString& message);
    void parseEngineIOFrame(const QString& data);
    void handleEvent(const QString& eventName, const QJsonArray& args);

    // Emit Socket.IO events
    void emitEvent(const QString& eventName, const QJsonObject& payload);
    void emitEvent(const QString& eventName, const QJsonArray& payload);

    // JSON payload builders
    QJsonObject buildOpenRoomPayload(const QString& roomName, const QString& gameId, int maxPlayers);
    QJsonObject buildJoinRoomPayload(const QString& roomId, bool spectate);
    QJsonObject buildWebRTCSignal(const QString& target, const QString& type, const QString& payload);

    // State management
    void updateRoomState(const QJsonObject& roomData);
    void updatePlayerList(const QJsonArray& players);

    // Internal state
    std::unique_ptr<QWebSocket> m_webSocket;
    QString m_serverUrl;
    QString m_playerId;
    QString m_playerName;
    QString m_roomId;
    ConnectionState m_connectionState;
    RoomInfo m_currentRoom;
    GameConfig m_gameConfig;
    QString m_sessionId;
    QString m_persistentId;

    // Keep-alive
    QTimer* m_pingTimer;
};

} // namespace UserInterface::Netplay

#endif // SOCKETIOCLIENT_HPP
