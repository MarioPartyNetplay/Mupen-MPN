#ifndef SOCKET_IO_SERVER_HPP
#define SOCKET_IO_SERVER_HPP

#include <QObject>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QMap>
#include <QSet>
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
 *   server.startServer(27886);
 *   // Clients connect via Socket.IO to ws://host:27886
 */
class SocketIOServer : public QObject
{
    Q_OBJECT

public:
    explicit SocketIOServer(QObject* parent = nullptr);
    ~SocketIOServer();

    /**
     * @brief Start listening for connections
     * @param port Port to listen on (default 27886)
     * @return true if server started successfully
     */
    bool startServer(int port = 27886);

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
    bool startHostedGame(const QString& roomId, const QString& mode, bool resyncEnabled, const QString& romHash);

    void broadcastControllerInput(const QString& roomId, int slot, uint32_t frameNumber, uint32_t controllerState);

    /**
     * @brief Broadcast updated cheats to all clients in a room
     */
    void broadcastCheatsUpdate(const QString& roomId, const QJsonArray& cheats);
    void broadcastSaveSync(const QString& roomId, const QJsonArray& saveFiles);
    void broadcastChatMessage(const QString& roomId, const QString& playerName, const QString& message);

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

    /**
     * @brief Emitted when a chat message is received from a client in a room
     */
    void chatMessageReceived(const QString& roomId, const QString& playerName, const QString& message);

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
        bool started = false;
    };

    // Server state
    std::unique_ptr<QWebSocketServer> m_server;
    QMap<QWebSocket*, ClientConnection*> m_clients;  // socket -> client
    QMap<QString, ClientConnection*> m_clientsById;  // id -> client
    QMap<QString, SignalingRoom> m_rooms;  // roomId -> room

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

    // Utilities
    ClientConnection* getClientFromSocket(QWebSocket* socket);
    ClientConnection* getClientById(const QString& clientId);
    SignalingRoom* getRoomById(const QString& roomId);
    QString generateRoomId();
    QString generateClientId();

    // Response helpers
    void sendSocketIOMessage(QWebSocket* socket, int type, const QJsonArray& args);
    void emitToRoom(const QString& roomId, const QString& eventName, const QJsonObject& data);
    void emitToClient(const QString& clientId, const QString& eventName, const QJsonObject& data);
    void broadcastRoomUpdate(const QString& roomId);
};

#endif // SOCKET_IO_SERVER_HPP
