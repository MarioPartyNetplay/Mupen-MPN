#ifndef SOCKET_IO_SERVER_HPP
#define SOCKET_IO_SERVER_HPP

#include "../NatTraversal/NatTraversalProtocol.hpp"
#include <QObject>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QMap>
#include <QHash>
#include <QSet>
#include <QList>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <memory>

/**
 * @brief C++ Socket.IO server for peer-to-peer game signaling
 *
 * Implements Socket.IO protocol over WebSocket.
 * Handles:
 * - Room management (create/join/leave)
 * - Player slot management
 * - WebRTC signaling relay (SDP offers/answers, ICE candidates)
 * - Game state broadcast
 *
 * Usage:
 *   SocketIOServer server;
 *   server.startServer(2626);
 *   // Clients connect via Socket.IO to ws://host:2626
 */
class SocketIOServer : public QObject
{
    Q_OBJECT

public:
    explicit SocketIOServer(QObject* parent = nullptr);
    ~SocketIOServer();

    /**
     * @brief Start listening for connections
     * @param port Port to listen on (default 2626)
     * @return true if server started successfully
     */
    bool startServer(int port = UserInterface::Netplay::kDefaultNetplayHostingPort);

    /**
     * @brief Stop the server
     */
    void stopServer();

    /**
     * @brief Create initial room for hosting
     * Pre-creates an empty room with the given ID so clients can join it
     */
    void createInitialRoom(const QString& roomId, const QString& hostName, const QString& gameName = "Unknown");

    /**
     * @brief Start game for a hosted room (host is embedded, not a websocket client)
     */
    bool startHostedGame(const QString& roomId, const QString& mode, bool resyncEnabled, const QString& romHash,
                         const QJsonArray& cheats = QJsonArray(), const QJsonArray& saveFiles = QJsonArray());

    void broadcastControllerInput(const QString& roomId, int slot, uint32_t frameNumber, uint32_t controllerState);
    void broadcastFrameSync(const QString& roomId, int slot, uint32_t frameNumber);

    /**
     * @brief Broadcast updated cheats to all clients in a room
     */
    void broadcastCheatsUpdate(const QString& roomId, const QJsonArray& cheats);
    void broadcastSaveSync(const QString& roomId, const QJsonArray& saveFiles);
    void broadcastCoreSettingsSync(const QString& roomId, const QJsonObject& coreSettings);
    void broadcastChatMessage(const QString& roomId, const QString& playerName, const QString& message);
    void broadcastInputDelayUpdate(const QString& roomId, int frames);
    void broadcastEmulationPauseUpdate(const QString& roomId, bool paused);

    /**
     * @brief Relay a WebRTC signal from the embedded host to a connected client
     */
    bool relayHostedWebRTCSignal(const QString& roomId, const QString& fromPlayerId, const QJsonObject& signal);

    /**
     * @brief Check if server is running
     */
    bool isRunning() const { return m_server != nullptr && m_server->isListening(); }

    /**
     * @brief Get the port the server is listening on
     */
    int getPort() const;

    /**
     * @brief Get number of connected clients
     */
    int getConnectedClientCount() const { return m_clients.size(); }

signals:
    /**
     * @brief Emitted when a client connects
     */
    void clientConnected(const QString& clientId);

    /**
     * @brief Emitted when a client disconnects
     */
    void clientDisconnected(const QString& clientId);

    /**
     * @brief Emitted when a room is created
     */
    void roomCreated(const QString& roomId);

    /**
     * @brief Emitted when a player joins a room
     */
    void playerJoined(const QString& roomId, const QString& playerId, int slotIndex);

    /**
     * @brief Emitted when a player leaves a room
     */
    void playerLeft(const QString& roomId, const QString& playerId);

    /**
     * @brief Emitted when game starts
     */
    void gameStarted(const QString& roomId);

    /**
     * @brief Emitted when room player list changes
     */
    void roomPlayersUpdated(const QString& roomId, const QJsonArray& players);

    /**
     * @brief Emitted when controller input is received for a room
     */
    void controllerInputReceived(const QString& roomId, int slot, uint32_t frameNumber, uint32_t controllerState);
    void frameSyncReceived(const QString& roomId, int slot, uint32_t frameNumber);

    /**
     * @brief Emitted when a chat message is received from a client in a room
     */
    void chatMessageReceived(const QString& roomId, const QString& playerName, const QString& message);

    /**
     * @brief Emitted when cheats are updated by a client (embedded host has no websocket)
     */
    void cheatsUpdated(const QString& roomId, const QJsonArray& cheats);

    /**
     * @brief Emitted when core timing settings are synced by the embedded host
     */
    void coreSettingsSyncReceived(const QString& roomId, const QJsonObject& coreSettings);

    /**
     * @brief Emitted when save data is synced by a client
     */
    void saveSyncReceived(const QString& roomId, const QJsonArray& saveFiles);

    /**
     * @brief Emitted when a WebRTC signal is destined for the embedded host (no websocket)
     */
    void hostedWebRTCSignalReceived(const QString& fromPlayerId, const QJsonObject& signal);

private slots:
    /**
     * @brief Handle new WebSocket connection
     */
    void onNewConnection();

    /**
     * @brief Handle text message from client
     */
    void onTextMessageReceived(const QString& message);

    /**
     * @brief Handle client disconnection
     */
    void onClientDisconnected();

private:
    /**
     * @brief Container for client connection state
     */
    struct ClientConnection
    {
        QString id;           // Unique client ID
        QString name;         // Player name
        QString roomId;       // Current room ID
        int slotIndex = -1;   // Player slot (0-3) or -1 if not claimed
        QString playerId;     // Game player ID
        QWebSocket* socket = nullptr;
    };

    /**
     * @brief Container for room state
     */
    struct SignalingRoom
    {
        QString id;
        QString hostId;       // Player 1 (host)
        QString roomName;
        QString gameName;
        QString gameId;
        int maxPlayers = 4;
        QMap<int, ClientConnection*> players;  // slot -> player
        QList<ClientConnection*> lobbyOrder;    // join order -> player
        bool started = false;

        // --- ADD THESE FIELDS TO STORE STATE ---
        QJsonArray activeCheats;
        QJsonArray activeSaves;
        QJsonObject activeCoreSettings;
        int inputDelayFrames = 4; // Default standard netplay lag buffer
        QMap<int, uint32_t> lastFrameSyncBySlot;
    };

    struct ChunkedCheatUpdate
    {
        QMap<int, QJsonArray> chunks;
        int chunkCount = 0;
    };

    // Server state
    std::unique_ptr<QWebSocketServer> m_server;
    QMap<QWebSocket*, ClientConnection*> m_clients;  // socket -> client
    QMap<QString, ClientConnection*> m_clientsById;  // id -> client
    QMap<QString, SignalingRoom> m_rooms;  // roomId -> room
    QHash<QString, ChunkedCheatUpdate> m_pendingCheatUpdates;

    // Message dispatch
    void handleSocketIOMessage(QWebSocket* socket, const QString& message);
    void handleEvent(QWebSocket* socket, const QJsonArray& args);

    // Event handlers
    void handle_OpenRoom(QWebSocket* socket, const QJsonObject& msg);
    void handle_JoinRoom(QWebSocket* socket, const QJsonObject& msg);
    void handle_LeaveRoom(QWebSocket* socket, const QJsonObject& msg);
    void handle_ClaimSlot(QWebSocket* socket, const QJsonObject& msg);
    void handle_SetName(QWebSocket* socket, const QJsonObject& msg);
    void handle_WebRTCSignal(QWebSocket* socket, const QJsonObject& msg);
    void handle_StartGame(QWebSocket* socket, const QJsonObject& msg);
    void handle_ListRooms(QWebSocket* socket, const QJsonObject& msg);
    void handle_ChatMessage(QWebSocket* socket, const QJsonObject& msg);
    void handle_CheatsUpdate(QWebSocket* socket, const QJsonObject& msg);
    void handle_SaveSyncUpdate(QWebSocket* socket, const QJsonObject& msg);
    void handle_ControllerInput(QWebSocket* socket, const QJsonObject& msg);
    void handle_FrameSync(QWebSocket* socket, const QJsonObject& msg);
    void handle_InputDelayUpdate(QWebSocket* socket, const QJsonObject& msg);
    void handle_EmulationPauseUpdate(QWebSocket* socket, const QJsonObject& msg);
    void handle_DirectRamPatch(QWebSocket* socket, const QJsonObject& msg);

    // Utilities
    ClientConnection* getClientFromSocket(QWebSocket* socket);
    ClientConnection* getClientById(const QString& clientId);
    SignalingRoom* getRoomById(const QString& roomId);
    QString generateRoomId();
    QString generateClientId();

    // Response helpers
    void sendSocketIOMessage(QWebSocket* socket, int type, const QJsonArray& args);
    void emitToRoom(const QString& roomId, const QString& eventName, const QJsonObject& data);
    void emitToConnectedRoomClients(const QString& roomId, const QString& eventName, const QJsonObject& data);
    void emitToClient(const QString& clientId, const QString& eventName, const QJsonObject& data);
    void emitToClient(const QString& clientId, const QString& eventName, const QJsonArray& data);
    void broadcastRoomUpdate(const QString& roomId);
    void rebuildLobbySlots(SignalingRoom& room);
};

#endif // SOCKET_IO_SERVER_HPP
