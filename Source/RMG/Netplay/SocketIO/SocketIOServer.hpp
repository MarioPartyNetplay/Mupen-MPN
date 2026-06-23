#ifndef SOCKET_IO_SERVER_HPP
#define SOCKET_IO_SERVER_HPP

#include "../NetplayEnet.hpp"
#include "../NetplayProtocol.hpp"
#include <QObject>
#include <QMap>
#include <QHash>
#include <QSet>
#include <QList>
#include <QString>
#include <QHostAddress>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>

struct _ENetHost;
struct _ENetPeer;

/**
 * @brief UDP/ENet signaling server for peer-to-peer netplay
 */
class SocketIOServer : public QObject
{
    Q_OBJECT

public:
    explicit SocketIOServer(QObject* parent = nullptr);
    ~SocketIOServer();

    bool startServer(int port = UserInterface::Netplay::kDefaultNetplayHostingPort, QString* errorOut = nullptr);
    void stopServer();

    void createInitialRoom(const QString& roomId, const QString& hostName, const QString& gameName = "Unknown");

    bool startHostedGame(const QString& roomId, const QString& mode, bool resyncEnabled, const QString& romHash,
                         const QJsonArray& cheats = QJsonArray(), const QJsonArray& saveFiles = QJsonArray());

    void broadcastControllerInput(const QString& roomId, int slot, uint32_t frameNumber, uint32_t controllerState);
    void broadcastFrameSync(const QString& roomId, int slot, uint32_t frameNumber, uint32_t stateHash);
    void broadcastCheatsUpdate(const QString& roomId, const QJsonArray& cheats);
    void broadcastSaveSync(const QString& roomId, const QJsonArray& saveFiles);
    void broadcastCoreSettingsSync(const QString& roomId, const QJsonObject& coreSettings);
    void broadcastChatMessage(const QString& roomId, const QString& playerName, const QString& message);
    void broadcastInputDelayUpdate(const QString& roomId, int frames);
    void broadcastEmulationPauseUpdate(const QString& roomId, bool paused);
    void markEmulationReady(const QString& roomId, int slotIndex);

    bool relayHostedWebRTCSignal(const QString& roomId, const QString& fromPlayerId, const QJsonObject& signal);

    /** Host-initiated connect after traversal server coordinates hole punch (connection reversal). */
    void attemptTraversalReversalConnect(const QHostAddress& clientAddress, quint16 clientPort);

    bool isRunning() const { return m_enetHost != nullptr; }
    ENetHost* enetHost() const { return m_enetHost; }
    int getPort() const;
    int getConnectedClientCount() const { return m_clients.size(); }

signals:
    void clientConnected(const QString& clientId);
    void clientDisconnected(const QString& clientId);
    void roomCreated(const QString& roomId);
    void playerJoined(const QString& roomId, const QString& playerId, int slotIndex);
    void playerLeft(const QString& roomId, const QString& playerId);
    void gameStarted(const QString& roomId);
    void roomPlayersUpdated(const QString& roomId, const QJsonArray& players);
    void playerPingsUpdated(const QString& roomId, const QJsonArray& pings);
    void controllerInputReceived(const QString& roomId, int slot, uint32_t frameNumber, uint32_t controllerState);
    void frameSyncReceived(const QString& roomId, int slot, uint32_t frameNumber, uint32_t stateHash);
    void chatMessageReceived(const QString& roomId, const QString& playerName, const QString& message);
    void cheatsUpdated(const QString& roomId, const QJsonArray& cheats);
    void coreSettingsSyncReceived(const QString& roomId, const QJsonObject& coreSettings);
    void saveSyncReceived(const QString& roomId, const QJsonArray& saveFiles);
    void hostedWebRTCSignalReceived(const QString& fromPlayerId, const QJsonObject& signal);
    void emulationBegin(const QString& roomId);

private slots:
    void onServiceTimer();
    void onPingTimer();

private:
    struct ClientConnection
    {
        QString id;
        QString name;
        QString roomId;
        int slotIndex = -1;
        QString playerId;
        ENetPeer* peer = nullptr;
        int lastPingMs = -1;
        QString reconnectToken;
        QString persistentId;
        qint64 disconnectedAtMs = 0;
    };

    struct SignalingRoom
    {
        QString id;
        QString hostId;
        QString roomName;
        QString gameName;
        QString gameId;
        int maxPlayers = 4;
        QMap<int, ClientConnection*> players;
        QList<ClientConnection*> lobbyOrder;
        bool started = false;

        QJsonArray activeCheats;
        QJsonArray activeSaves;
        QJsonObject activeCoreSettings;
        int inputDelayFrames = 4;
        QMap<int, uint32_t> lastFrameSyncBySlot;
        QSet<int> emulationReadySlots;
        bool emulationBeginSent = false;
    };

    struct ChunkedCheatUpdate
    {
        QMap<int, QJsonArray> chunks;
        int chunkCount = 0;
    };

    ENetHost* m_enetHost = nullptr;
    int m_listenPort = 0;
    QMap<ENetPeer*, ClientConnection*> m_clients;
    QMap<QString, ClientConnection*> m_clientsById;
    QHash<QString, ClientConnection*> m_disconnectedClientsByToken;
    QMap<QString, SignalingRoom> m_rooms;
    QHash<QString, ChunkedCheatUpdate> m_pendingCheatUpdates;

    void handleSignalingPacket(ENetPeer* peer, const QByteArray& payload);
    void handleEvent(ENetPeer* peer, const QJsonArray& args);

    void handle_OpenRoom(ENetPeer* peer, const QJsonObject& msg);
    void handle_JoinRoom(ENetPeer* peer, const QJsonObject& msg);
    void handle_ReconnectRoom(ENetPeer* peer, const QJsonObject& msg);
    void handle_LeaveRoom(ENetPeer* peer, const QJsonObject& msg);
    void handle_ClaimSlot(ENetPeer* peer, const QJsonObject& msg);
    void handle_SetName(ENetPeer* peer, const QJsonObject& msg);
    void handle_WebRTCSignal(ENetPeer* peer, const QJsonObject& msg);
    void handle_StartGame(ENetPeer* peer, const QJsonObject& msg);
    void handle_ListRooms(ENetPeer* peer, const QJsonObject& msg);
    void handle_ChatMessage(ENetPeer* peer, const QJsonObject& msg);
    void handle_CheatsUpdate(ENetPeer* peer, const QJsonObject& msg);
    void handle_SaveSyncUpdate(ENetPeer* peer, const QJsonObject& msg);
    void handle_ControllerInput(ENetPeer* peer, const QJsonObject& msg);
    void handle_FrameSync(ENetPeer* peer, const QJsonObject& msg);
    void handle_InputDelayUpdate(ENetPeer* peer, const QJsonObject& msg);
    void handle_EmulationPauseUpdate(ENetPeer* peer, const QJsonObject& msg);
    void handle_EmulationReady(ENetPeer* peer, const QJsonObject& msg);
    void handle_DirectRamPatch(ENetPeer* peer, const QJsonObject& msg);

    void onClientDisconnected(ENetPeer* peer);
    void onClientConnected(ENetPeer* peer);
    void purgeExpiredDisconnectedClients();
    void sendReconnectToken(ClientConnection* client);
    void sendRoomCatchUp(const QString& roomId, ClientConnection* client);
    void removeClientFromRoom(ClientConnection* client);

    void broadcastRoomPlayerPings(const QString& roomId);
    void tryBroadcastEmulationBegin(SignalingRoom* room);

    QTimer* m_serviceTimer = nullptr;
    QTimer* m_pingTimer = nullptr;
    QSet<QString> m_traversalReversalTargets;

    ClientConnection* getClientFromPeer(ENetPeer* peer);
    ClientConnection* getClientById(const QString& clientId);
    SignalingRoom* getRoomById(const QString& roomId);
    QString generateRoomId();
    QString generateClientId();

    void emitToRoom(const QString& roomId, const QString& eventName, const QJsonObject& data);
    void emitToConnectedRoomClients(const QString& roomId, const QString& eventName, const QJsonObject& data);
    void emitToClient(const QString& clientId, const QString& eventName, const QJsonObject& data);
    void emitToClient(const QString& clientId, const QString& eventName, const QJsonArray& data);
    void broadcastRoomUpdate(const QString& roomId);
    void rebuildLobbySlots(SignalingRoom& room);
};

#endif // SOCKET_IO_SERVER_HPP
