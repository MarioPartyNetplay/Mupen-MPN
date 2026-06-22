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

#include "../NetplayEnet.hpp"
#include "../NetplayProtocol.hpp"

#include <QString>
#include <QMap>
#include <QHash>
#include <QObject>
#include <QUrl>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
#include <QHostAddress>
#include <QUdpSocket>
#include <functional>
#include <memory>

struct _ENetHost;
struct _ENetPeer;

namespace UserInterface::Netplay {

/**
 * @class SocketIOClient
 * @brief UDP/ENet client for netplay signaling server communication
 */
class SocketIOClient : public QObject {
    Q_OBJECT

public:
    enum ConnectionState {
        Disconnected,
        Connecting,
        Connected,
        Reconnecting,
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
        QString mode;
        bool resyncEnabled;
        QString romHash;
    };

    struct ChunkedCheatUpdate
    {
        QMap<int, QJsonArray> chunks;
        int chunkCount = 0;
    };

    explicit SocketIOClient(const QString& serverUrl = "udp://localhost:2626", QObject* parent = nullptr);
    ~SocketIOClient();


    void connectToServer(const QString& playerName, quint16 bindUdpPort = 0, bool useTraversalPunch = false);
    void disconnect();
    ConnectionState getConnectionState() const;

    void openRoom(const QString& roomName, const QString& gameId, int maxPlayers = 4);
    void joinRoom(const QString& roomId, bool asSpectator = false);
    void leaveRoom();
    void setPlayerName(const QString& name);
    void claimSlot(int slot);

    void startGame(const QString& mode, bool resyncEnabled, const QString& romHash);
    void endGame();
    void setGameMode(const QString& mode);
    void sendControllerInput(uint32_t frameNumber, uint32_t controllerState);
    void sendFrameSync(uint32_t frameNumber, uint32_t stateHash);

    void sendOffer(const QString& targetPlayerId, const QString& sdpOffer);
    void sendAnswer(const QString& targetPlayerId, const QString& sdpAnswer);
    void sendICECandidate(const QString& targetPlayerId, const QString& candidate, int sdpMLineIndex = 0);

    void setROMSharingEnabled(bool enabled);
    void declareROMReady(bool ready);
    void declareROMInfo(const QString& fileName, const QString& hash, uint32_t fileSize);

    void sendSyncLog(const QString& matchId, const QJsonArray& entries, const QJsonObject& summary);
    void sendDebugLog(const QString& matchId, const QString& logContent);

    void sendChatMessage(const QString& message);
    void sendCheatsUpdate(const QJsonArray& cheats);
    void sendSaveSync(const QJsonArray& saveFiles);
    void sendCoreSettingsSync(const QJsonObject& coreSettings);

    void requestRoomList(bool waiting = false);
    
    void sendInputDelayUpdate(int frames);
    void sendEmulationPauseUpdate(bool paused);
    void sendEmulationReady();

    QString getPlayerId() const;
    QString getRoomId() const;
    RoomInfo getCurrentRoom() const;
    QList<PlayerInfo> getPlayerList() const;
    GameConfig getGameConfig() const;
    int getLastPingMs() const;

signals:
    void connected();
    void disconnected();
    void reconnecting();
    void reconnected();
    void connectionError(const QString& error);

    void roomCreated(const QString& roomId);
    void roomJoined(const QString& roomId, int slotIndex);
    void roomLeft();
    void roomClosed(const QString& reason);
    void playersUpdated(const QList<PlayerInfo>& players);
    void pingUpdated(int pingMs);
    void playerPingsReceived(const QJsonArray& pings);
    void spectatorCountUpdated(int count);
    void roomsListed(const QJsonArray& rooms);

    void gameStarted(const QString& mode, bool resyncEnabled, const QString& matchId);
    void gameEnded();
    void gameModeChanged(const QString& mode);
    void controllerInputReceived(int slot, uint32_t frameNumber, uint32_t controllerState);
    void frameSyncReceived(int slot, uint32_t frameNumber, uint32_t stateHash);

    void offerReceived(const QString& fromPlayerId, const QString& sdpOffer);
    void answerReceived(const QString& fromPlayerId, const QString& sdpAnswer);
    void iceCandidateReceived(const QString& fromPlayerId, const QString& candidate, int sdpMLineIndex);

    void romSharingUpdated(bool enabled);
    void playerROMReady(const QString& playerId);
    void romDeclared(const QString& playerId, const QString& fileName, const QString& hash);

    void uploadTokenReceived(const QString& token);
    void reconnectTokenReceived(const QString& token);

    void chatMessageReceived(const QString& playerName, const QString& message);
    void cheatsUpdated(const QJsonArray& cheats);
    void saveSyncReceived(const QJsonArray& saveFiles);
    void coreSettingsSyncReceived(const QJsonObject& coreSettings);
    void inputDelayReceived(int frames);
    void emulationPauseReceived(bool paused);
    void emulationBeginReceived();

private slots:
    void on_serviceTimer();
    void on_connectTimeout();
    void on_reconnectTimer();

private:
    void beginReconnect();
    void failReconnect();
    void sendReconnectRoom();
    bool startTransportConnect(quint16 bindUdpPort, bool useTraversalPunch);
    void handleSignalingPacket(const QByteArray& payload);
    void handleEvent(const QString& eventName, const QJsonArray& args);

    void emitEvent(const QString& eventName, const QJsonObject& payload);
    void emitEvent(const QString& eventName, const QJsonArray& payload);

    QJsonObject buildOpenRoomPayload(const QString& roomName, const QString& gameId, int maxPlayers);
    QJsonObject buildJoinRoomPayload(const QString& roomId, bool spectate);
    QJsonObject buildWebRTCSignal(const QString& target, const QString& type, const QString& payload);

    void updateRoomState(const QJsonObject& roomData);
    void updatePlayerList(const QJsonArray& players);
    void destroyEnetClient();

    void sendConnectPunchBursts();

    bool parseServerEndpoint(const QString& serverUrl, QHostAddress* addressOut, quint16* portOut) const;
    bool setEnetPeerAddress(ENetAddress* addressOut) const;

    ENetHost* m_enetHost = nullptr;
    ENetPeer* m_serverPeer = nullptr;
    QTimer* m_serviceTimer = nullptr;
    QTimer* m_connectTimer = nullptr;
    QTimer* m_pingTimer = nullptr;
    QTimer* m_punchTimer = nullptr;
    QTimer* m_reconnectTimer = nullptr;

    QString m_serverUrl;
    QString m_serverHostname;
    QHostAddress m_serverAddress;
    quint16 m_serverPort = kDefaultNetplayHostingPort;
    bool m_useTraversalPunch = false;
    QString m_playerId;
    QString m_playerName;
    QString m_roomId;
    ConnectionState m_connectionState;
    RoomInfo m_currentRoom;
    GameConfig m_gameConfig;
    QString m_sessionId;
    QString m_persistentId;
    QHash<QString, ChunkedCheatUpdate> m_pendingCheatUpdates;
    uint32_t m_lastSentFrameSync = 0;
    int m_lastPingMs = -1;
    QString m_reconnectToken;
    bool m_intentionalDisconnect = false;
    bool m_awaitingReconnectAck = false;
    int m_reconnectAttempts = 0;
    quint16 m_savedBindUdpPort = 0;
    bool m_savedUseTraversalPunch = false;
};

} // namespace UserInterface::Netplay

#endif // SOCKETIOCLIENT_HPP
