/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "JuiceTraversal.hpp"

#include "Netplay/NetplayProtocol.hpp"
#include "Netplay/WebRTC/TurnCredentialClient.hpp"

#include <juice/juice.h>

#include <QDateTime>
#include <QDebug>
#include <QHostInfo>
#include <QNetworkDatagram>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QVector>

#include <atomic>
#include <functional>
#include <vector>

namespace UserInterface::Netplay {

namespace {

constexpr int kGatherTimeoutMs = 20000;
constexpr int kAnswerWaitMs = 25000;
constexpr int kOfferIntervalMs = 250;
constexpr int kAnswerPollMs = 250;
constexpr int kListIntervalMs = 250;
constexpr int kAnswerRepublishGapMs = 250;
constexpr int kAckedAnswerRefreshMs = 2000;
constexpr int kPingIntervalMs = 1000;
constexpr int kServerSilenceFailMs = 8000;
constexpr int kTurnWaitMs = 8000;
constexpr int kMaxTurnServers = 2;
constexpr int kStunOnlyIceWaitMs = 6000;
constexpr int kTickIntervalMs = 50;
constexpr int kEarlyGatherPublishMs = 400;

QByteArray g_resolvedStunIpv4;

bool isIpv4Literal(const QString& host)
{
    QHostAddress address;
    return address.setAddress(host) && address.protocol() == QAbstractSocket::IPv4Protocol;
}

void beginStunResolve(QObject* context)
{
    const QString host = stunServerHost();
    if (isIpv4Literal(host)) {
        g_resolvedStunIpv4 = host.toUtf8();
        return;
    }
    if (!g_resolvedStunIpv4.isEmpty() || context == nullptr) {
        return;
    }

    QHostInfo::lookupHost(host, context, [host](const QHostInfo& info) {
        if (!g_resolvedStunIpv4.isEmpty()) {
            return;
        }
        if (info.error() != QHostInfo::NoError) {
            qWarning() << "JuiceTraversal: STUN resolve failed for" << host << info.errorString();
            return;
        }
        for (const QHostAddress& resolved : info.addresses()) {
            if (resolved.protocol() == QAbstractSocket::IPv4Protocol) {
                g_resolvedStunIpv4 = resolved.toString().toUtf8();
                qInfo() << "JuiceTraversal: STUN server is" << resolved.toString() << "port"
                        << stunServerPort();
                return;
            }
        }
        qWarning() << "JuiceTraversal: no IPv4 address for STUN host" << host;
    });
}

qint64 monotonicMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

struct ResolvedIce {
    QByteArray stunHost;
    uint16_t stunPort = 0;
    struct Turn {
        QByteArray host;
        QByteArray username;
        QByteArray password;
        uint16_t port = 0;
    };
    QVector<Turn> turns;
};

bool iceSdpHasRelay(const QByteArray& sdp)
{
    return sdp.contains("typ relay");
}

bool turnRelayAvailable()
{
    TurnCredentialClient& iceClient = TurnCredentialClient::instance();
    if (!iceClient.isConfigured()) {
        return false;
    }
    if (!iceClient.hasFreshCredentials()) {
        iceClient.prefetch();
        return false;
    }
    return !iceClient.turnServers().empty();
}

ResolvedIce resolveIceServers(bool includeTurn)
{
    ResolvedIce ice;
    if (includeTurn) {
        for (const rtc::IceServer& server : buildIceServers()) {
            if (server.hostname.empty() || server.port == 0) {
                continue;
            }
            if (server.type == rtc::IceServer::Type::Stun) {
                if (ice.stunHost.isEmpty()) {
                    ice.stunHost = QByteArray::fromStdString(server.hostname);
                    ice.stunPort = static_cast<uint16_t>(server.port);
                }
                continue;
            }
            if (ice.turns.size() >= kMaxTurnServers || server.username.empty() || server.password.empty()) {
                continue;
            }
            ResolvedIce::Turn turn;
            turn.host = QByteArray::fromStdString(server.hostname);
            turn.username = QByteArray::fromStdString(server.username);
            turn.password = QByteArray::fromStdString(server.password);
            turn.port = static_cast<uint16_t>(server.port);
            ice.turns.push_back(std::move(turn));
        }
    }

    if (ice.stunHost.isEmpty()) {
        const QString configured = stunServerHost();
        if (!g_resolvedStunIpv4.isEmpty()) {
            ice.stunHost = g_resolvedStunIpv4;
            ice.stunPort = stunServerPort();
        } else if (isIpv4Literal(configured)) {
            ice.stunHost = configured.toUtf8();
            ice.stunPort = stunServerPort();
        } else {
            qInfo() << "JuiceTraversal: STUN hostname is not resolved yet; gathering host candidates only";
        }
    }
    return ice;
}

void installJuiceLogHandler()
{
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;
    juice_set_log_level(JUICE_LOG_LEVEL_INFO);
    juice_set_log_handler([](juice_log_level_t level, const char* message) {
        if (message == nullptr) {
            return;
        }
        if (level >= JUICE_LOG_LEVEL_WARN) {
            qWarning() << "libjuice:" << message;
        } else {
            qInfo() << "libjuice:" << message;
        }
    });
}

QString makeJoinerId()
{
    const quint32 value = QRandomGenerator::global()->generate();
    return QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0')).right(8).toLower();
}

bool isJoinerId(const QString& id)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{8}$"));
    return pattern.match(id).hasMatch();
}

} // namespace

class JuiceTraversal::JuicePeer : public QObject {
public:
    enum class Role {
        Joiner,
        Host,
    };

    JuicePeer(Role role, quint16 signalingPort, QObject* parent)
        : QObject(parent)
        , m_role(role)
        , m_signalingPort(signalingPort)
    {
    }

    ~JuicePeer() override
    {
        detach();
        if (m_agent != nullptr) {
            juice_agent_t* agent = m_agent;
            m_agent = nullptr;
            juice_destroy(agent);
        }
    }

    void detach()
    {
        m_alive.store(false);
        onLocalDescription = {};
        onConnected = {};
        onFailed = {};
    }

    bool start(const QByteArray& remoteSdp, bool useTurn)
    {
        installJuiceLogHandler();
        m_useTurn = useTurn;
        m_startedRemoteSdp = remoteSdp;
        const ResolvedIce ice = resolveIceServers(useTurn);

        std::vector<juice_turn_server_t> turnServers;
        turnServers.reserve(static_cast<size_t>(ice.turns.size()));
        for (const ResolvedIce::Turn& turn : ice.turns) {
            juice_turn_server_t server{};
            server.host = turn.host.constData();
            server.username = turn.username.constData();
            server.password = turn.password.constData();
            server.port = turn.port;
            turnServers.push_back(server);
        }

        juice_config_t config{};
        config.concurrency_mode = JUICE_CONCURRENCY_MODE_THREAD;
        if (!ice.stunHost.isEmpty() && ice.stunPort != 0) {
            config.stun_server_host = ice.stunHost.constData();
            config.stun_server_port = ice.stunPort;
        }
        if (!turnServers.empty()) {
            config.turn_servers = turnServers.data();
            config.turn_servers_count = static_cast<int>(turnServers.size());
        }
        config.cb_state_changed = &JuicePeer::onStateChanged;
        config.cb_candidate = &JuicePeer::onCandidate;
        config.cb_gathering_done = &JuicePeer::onGatheringDone;
        config.cb_recv = &JuicePeer::onRecv;
        config.user_ptr = this;

        m_agent = juice_create(&config);
        if (m_agent == nullptr) {
            fail(QStringLiteral("Failed to create libjuice ICE agent"));
            return false;
        }

        if (useTurn && !ice.turns.isEmpty()) {
            qInfo() << "libjuice gathering with" << ice.turns.size()
                    << "TURN server(s); ICE still prefers a direct path when one works";
        } else {
            qInfo() << "libjuice gathering STUN/direct candidates only";
        }

        if (!remoteSdp.isEmpty()) {
            if (juice_set_remote_description(m_agent, remoteSdp.constData()) != 0) {
                fail(QStringLiteral("Failed to apply remote ICE description"));
                return false;
            }
            m_remoteApplied = true;
        }

        if (juice_gather_candidates(m_agent) != 0) {
            fail(QStringLiteral("Failed to gather ICE candidates"));
            return false;
        }
        scheduleLocalDescriptionPublish();
        return true;
    }

    bool isLinked() const { return m_linked; }
    bool hasSameRemote(const QByteArray& sdp) const { return !sdp.isEmpty() && sdp == m_startedRemoteSdp; }

    void applyRemoteDescription(const QByteArray& sdp)
    {
        if (sdp.isEmpty()) {
            return;
        }
        m_pendingRemote = sdp;
        tryApplyRemote();
    }

    std::function<void(const QByteArray&)> onLocalDescription;
    std::function<void(quint16)> onConnected;
    std::function<void(const QString&)> onFailed;

private:
    void fail(const QString& reason)
    {
        if (m_failed) {
            return;
        }
        m_failed = true;
        qWarning() << "JuicePeer:" << reason;
        if (onFailed) {
            onFailed(reason);
        }
    }

    void tryApplyRemote()
    {
        if (m_agent == nullptr || m_pendingRemote.isEmpty() || m_remoteApplied) {
            return;
        }
        if (juice_set_remote_description(m_agent, m_pendingRemote.constData()) != 0) {
            fail(QStringLiteral("Failed to apply remote ICE description"));
            return;
        }
        m_remoteApplied = true;
        m_pendingRemote.clear();
    }

    void scheduleLocalDescriptionPublish()
    {
        if (!m_alive.load() || m_failed || m_publishScheduled || !m_lastPublishedSdp.isEmpty()) {
            return;
        }
        m_publishScheduled = true;
        QTimer::singleShot(kEarlyGatherPublishMs, this, [this]() {
            m_publishScheduled = false;
            if (m_alive.load()) {
                publishLocalDescription(false);
            }
        });
    }

    void publishLocalDescription(bool gatheringDone)
    {
        if (!m_alive.load() || m_agent == nullptr || m_failed) {
            return;
        }
        if (gatheringDone) {
            m_gatheringDone = true;
        } else if (!m_lastPublishedSdp.isEmpty()) {
            return;
        }

        char sdp[JUICE_MAX_SDP_STRING_LEN];
        if (juice_get_local_description(m_agent, sdp, sizeof(sdp)) != 0) {
            if (gatheringDone) {
                fail(QStringLiteral("Failed to read local ICE description"));
            }
            return;
        }

        const QByteArray snapshot(sdp);
        if (snapshot == m_lastPublishedSdp) {
            tryApplyRemote();
            return;
        }
        if (m_remoteApplied && !gatheringDone) {
            return;
        }
        m_lastPublishedSdp = snapshot;
        if (onLocalDescription) {
            onLocalDescription(snapshot);
        }
        tryApplyRemote();
    }

    void handleGatheringDone()
    {
        publishLocalDescription(true);
    }

    void handleState(juice_state_t state)
    {
        if (!m_alive.load()) {
            return;
        }
        qInfo() << "libjuice state" << juice_state_to_string(state);
        if (state == JUICE_STATE_FAILED) {
            fail(QStringLiteral("ICE connection failed"));
            return;
        }
        if (state != JUICE_STATE_CONNECTED && state != JUICE_STATE_COMPLETED) {
            return;
        }
        if (m_linked) {
            return;
        }
        m_linked = true;

        char localAddress[JUICE_MAX_ADDRESS_STRING_LEN];
        char remoteAddress[JUICE_MAX_ADDRESS_STRING_LEN];
        if (juice_get_selected_addresses(m_agent, localAddress, sizeof(localAddress), remoteAddress,
                                         sizeof(remoteAddress)) == 0) {
            qInfo() << "libjuice selected" << localAddress << "->" << remoteAddress;
        }

        char localCandidate[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
        char remoteCandidate[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
        if (juice_get_selected_candidates(m_agent, localCandidate, sizeof(localCandidate), remoteCandidate,
                                          sizeof(remoteCandidate)) == 0) {
            const QByteArray localSdp(localCandidate);
            const QByteArray remoteSdp(remoteCandidate);
            const bool usedRelay = iceSdpHasRelay(localSdp) || iceSdpHasRelay(remoteSdp);
            qInfo() << "libjuice candidates" << localCandidate << "->" << remoteCandidate;
            qInfo() << "libjuice path" << (usedRelay ? "TURN relay" : "direct/STUN")
                    << (m_useTurn ? "(TURN was available)" : "(TURN not gathered)");
        }

        if (!m_proxy.bind(QHostAddress::LocalHost, 0, QUdpSocket::DontShareAddress)) {
            fail(QStringLiteral("Failed to bind NAT traversal proxy: %1").arg(m_proxy.errorString()));
            return;
        }
        connect(&m_proxy, &QUdpSocket::readyRead, this, [this]() { readProxy(); });

        if (m_role == Role::Joiner && onConnected) {
            onConnected(m_proxy.localPort());
        }
    }

    void readProxy()
    {
        if (m_agent == nullptr || !m_linked) {
            return;
        }
        while (m_proxy.hasPendingDatagrams()) {
            const QNetworkDatagram datagram = m_proxy.receiveDatagram();
            if (m_role == Role::Joiner && datagram.senderPort() > 0) {
                m_enetPort = static_cast<quint16>(datagram.senderPort());
            }
            const QByteArray payload = datagram.data();
            if (payload.isEmpty()) {
                continue;
            }
            const int result = juice_send(m_agent, payload.constData(), static_cast<size_t>(payload.size()));
            if (result == JUICE_ERR_TOO_LARGE) {
                qWarning() << "JuicePeer: dropped oversized signaling datagram" << payload.size();
            }
        }
    }

    void forwardFromJuice(const QByteArray& payload)
    {
        if (!m_linked || payload.isEmpty()) {
            return;
        }
        if (m_role == Role::Host) {
            if (m_signalingPort == 0) {
                return;
            }
            m_proxy.writeDatagram(payload, QHostAddress::LocalHost, m_signalingPort);
            return;
        }
        if (m_enetPort == 0) {
            return;
        }
        m_proxy.writeDatagram(payload, QHostAddress::LocalHost, m_enetPort);
    }

    static void onStateChanged(juice_agent_t*, juice_state_t state, void* user)
    {
        auto* peer = static_cast<JuicePeer*>(user);
        if (peer == nullptr || !peer->m_alive.load()) {
            return;
        }
        QTimer::singleShot(0, peer, [peer, state]() {
            if (peer->m_alive.load()) {
                peer->handleState(state);
            }
        });
    }

    static void onCandidate(juice_agent_t*, const char*, void* user)
    {
        auto* peer = static_cast<JuicePeer*>(user);
        if (peer == nullptr || !peer->m_alive.load()) {
            return;
        }
        QTimer::singleShot(0, peer, [peer]() {
            if (peer->m_alive.load()) {
                peer->scheduleLocalDescriptionPublish();
            }
        });
    }

    static void onGatheringDone(juice_agent_t*, void* user)
    {
        auto* peer = static_cast<JuicePeer*>(user);
        if (peer == nullptr || !peer->m_alive.load()) {
            return;
        }
        QTimer::singleShot(0, peer, [peer]() {
            if (peer->m_alive.load()) {
                peer->handleGatheringDone();
            }
        });
    }

    static void onRecv(juice_agent_t*, const char* data, size_t size, void* user)
    {
        auto* peer = static_cast<JuicePeer*>(user);
        if (peer == nullptr || !peer->m_alive.load() || data == nullptr || size == 0) {
            return;
        }
        const QByteArray payload(data, static_cast<int>(size));
        QTimer::singleShot(0, peer, [peer, payload]() {
            if (peer->m_alive.load()) {
                peer->forwardFromJuice(payload);
            }
        });
    }

    Role m_role = Role::Joiner;
    bool m_useTurn = false;
    quint16 m_signalingPort = 0;
    quint16 m_enetPort = 0;
    juice_agent_t* m_agent = nullptr;
    QUdpSocket m_proxy;
    QByteArray m_startedRemoteSdp;
    QByteArray m_pendingRemote;
    QByteArray m_lastPublishedSdp;
    bool m_publishScheduled = false;
    bool m_gatheringDone = false;
    bool m_remoteApplied = false;
    bool m_linked = false;
    bool m_failed = false;
    std::atomic<bool> m_alive{true};
};

JuiceTraversal::JuiceTraversal(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QUdpSocket::readyRead, this, &JuiceTraversal::onReadyRead);
    connect(&m_timer, &QTimer::timeout, this, &JuiceTraversal::onTick);
    m_timer.setInterval(kTickIntervalMs);
    resolveServerAsync();
    beginStunResolve(this);
}

JuiceTraversal::~JuiceTraversal()
{
    stopHosting();
    cancelJoin();
}

void JuiceTraversal::startHosting(const QString& hostCode, quint16 signalingPort)
{
    const QString normalized = normalizeTraversalCode(hostCode);
    if (normalized.isEmpty() || signalingPort == 0) {
        return;
    }
    if (m_hosting && m_hostCode == normalized && m_signalingPort == signalingPort) {
        return;
    }

    stopHosting();
    m_hosting = true;
    m_hostCode = normalized;
    m_signalingPort = signalingPort;
    m_nextListMs = 0;
    m_nextPingMs = 0;
    resolveServerAsync();
    beginStunResolve(this);
    if (!m_timer.isActive()) {
        m_timer.start();
    }
    qInfo() << "NAT traversal is running for" << m_hostCode
            << "— watching joiners and pinging the index every" << (kPingIntervalMs / 1000) << "second";
    onTick();
}

void JuiceTraversal::stopHosting()
{
    if (!m_hosting && m_hostPeers.isEmpty()) {
        return;
    }

    const QStringList joinerIds = m_hostPeers.keys();
    for (const QString& joinerId : joinerIds) {
        deleteKey(offerKey(m_hostCode, joinerId));
        deleteKey(answerKey(m_hostCode, joinerId));
        if (JuicePeer* peer = m_hostPeers.take(joinerId)) {
            peer->detach();
            peer->deleteLater();
        }
    }
    m_pendingAnswers.clear();
    m_hosting = false;
    m_hostCode.clear();
    m_signalingPort = 0;
    if (!m_joining) {
        m_timer.stop();
    }
}

bool JuiceTraversal::isHosting() const
{
    return m_hosting;
}

void JuiceTraversal::joinHost(const QString& hostCode)
{
    const QString normalized = normalizeTraversalCode(hostCode);
    if (normalized.isEmpty()) {
        failJoin(QStringLiteral("Invalid host code"));
        return;
    }

    cancelJoin();
    m_joining = true;
    m_joinReady = false;
    m_joinFinished = false;
    m_offerGathered = false;
    m_answerApplied = false;
    m_localOffer.clear();
    m_joinProxyPort = 0;
    m_joinCode = normalized;
    m_joinerId = makeJoinerId();
    m_turnRetryUsed = false;
    m_offerAcked = false;
    m_joinDeadlineMs = monotonicMs() + kGatherTimeoutMs;
    m_stunOnlyDeadlineMs = 0;
    m_turnWaitDeadlineMs = 0;
    m_nextOfferMs = 0;
    m_nextAnswerPollMs = 0;
    m_nextPingMs = 0;
    m_firstPingMs = 0;
    m_lastPongMs = 0;
    m_lastProgress.clear();

    TurnCredentialClient::instance().prefetch();
    resolveServerAsync();
    beginStunResolve(this);
    setJoinProgress(QStringLiteral("Contacting the NAT traversal server…"));
    startJoinerPeer(false);
    if (!m_joining) {
        return;
    }

    if (!m_timer.isActive()) {
        m_timer.start();
    }
    qInfo() << "NAT traversal is running for" << m_joinCode << "as" << m_joinerId
            << "— STUN/direct first, TURN only if that path fails";
}

void JuiceTraversal::cancelJoin()
{
    ++m_failureToken;
    if (m_joining && !m_joinerId.isEmpty() && !m_joinCode.isEmpty()) {
        deleteKey(offerKey(m_joinCode, m_joinerId));
        deleteKey(answerKey(m_joinCode, m_joinerId));
    }
    resetJoin();
    if (!m_hosting) {
        m_timer.stop();
    }
}

bool JuiceTraversal::isJoinReady() const
{
    return m_joinReady;
}

quint16 JuiceTraversal::joinProxyPort() const
{
    return m_joinProxyPort;
}

void JuiceTraversal::onTick()
{
    const qint64 now = monotonicMs();

    if ((m_hosting || m_joining) && (m_nextPingMs == 0 || now >= m_nextPingMs)) {
        sendPing();
        m_nextPingMs = now + kPingIntervalMs;
    }

    if (m_hosting) {
        if (m_nextListMs == 0 || now >= m_nextListMs) {
            const QByteArray prefix = QByteArray("ice/") + m_hostCode.toUtf8() + "/";
            sendIndex(QByteArray(kNetplayIndexProtocol) + "|LIST|" + prefix);
            m_nextListMs = now + kListIntervalMs;
        }
        for (auto it = m_pendingAnswers.begin(); it != m_pendingAnswers.end(); ++it) {
            if (it->sdp.isEmpty() || !m_hostPeers.contains(it.key())) {
                continue;
            }
            if (it->nextPublishMs != 0 && now < it->nextPublishMs) {
                continue;
            }
            publishValue(answerKey(m_hostCode, it.key()), it->sdp);
            it->nextPublishMs = now + (it->acked ? kAckedAnswerRefreshMs : kAnswerRepublishGapMs);
        }
    }

    if (!m_joining || m_joinFinished) {
        return;
    }

    if (m_serverAddress.isNull() && !m_resolving && m_firstPingMs > 0 &&
        now - m_firstPingMs >= kServerSilenceFailMs) {
        failJoin(QStringLiteral("Failed to resolve the NAT traversal server"));
        return;
    }

    if (serverLooksDead(now)) {
        failJoin(m_lastPongMs == 0 ? QStringLiteral("NAT traversal server did not answer")
                                   : QStringLiteral("Lost contact with the NAT traversal server"));
        return;
    }

    if (m_turnWaitDeadlineMs > 0) {
        if (turnRelayReady()) {
            m_turnWaitDeadlineMs = 0;
            if (!tryRetryJoinWithTurn(QStringLiteral("Direct ICE did not connect"))) {
                failJoin(QStringLiteral("Direct ICE did not connect"));
            }
            return;
        }
        if (now >= m_turnWaitDeadlineMs) {
            failJoin(QStringLiteral("Relay credentials were not ready"));
            return;
        }
        return;
    }

    if (m_offerGathered && !m_answerApplied && m_nextOfferMs == 0) {
        /* Gathering already consumed the first deadline. The host still has to
           gather its own candidates before the answer exists. */
        m_joinDeadlineMs = now + kAnswerWaitMs;
        setJoinProgress(QStringLiteral("Waiting for the host to answer…"));
    }

    if (m_stunOnlyDeadlineMs > 0 && now >= m_stunOnlyDeadlineMs) {
        failJoin(QStringLiteral("Direct ICE did not connect"));
        return;
    }

    if (now >= m_joinDeadlineMs) {
        failJoin(m_answerApplied ? QStringLiteral("ICE connection timed out")
                 : m_offerGathered ? QStringLiteral("Timed out waiting for the host ICE answer")
                                   : QStringLiteral("Timed out gathering ICE candidates"));
        return;
    }

    if (m_offerGathered && !m_answerApplied && (m_nextOfferMs == 0 || now >= m_nextOfferMs)) {
        publishValue(offerKey(m_joinCode, m_joinerId), m_localOffer);
        m_nextOfferMs = now + (m_offerAcked ? kAckedAnswerRefreshMs : kOfferIntervalMs);
    }

    if (m_offerGathered && !m_answerApplied && (m_nextAnswerPollMs == 0 || now >= m_nextAnswerPollMs)) {
        const QByteArray key = answerKey(m_joinCode, m_joinerId).toUtf8();
        sendIndex(QByteArray(kNetplayIndexProtocol) + "|GET|" + key);
        m_nextAnswerPollMs = now + kAnswerPollMs;
    }
}

void JuiceTraversal::onReadyRead()
{
    while (m_socket.hasPendingDatagrams()) {
        handleIndexMessage(m_socket.receiveDatagram().data());
    }
}

bool JuiceTraversal::ensureServerAddress()
{
    if (!m_serverAddress.isNull()) {
        return true;
    }

    const QString hostname = netplayIndexServerHostname();
    QHostAddress address;
    if (address.setAddress(hostname)) {
        m_serverAddress = address;
        return true;
    }

    resolveServerAsync();
    return false;
}

void JuiceTraversal::resolveServerAsync()
{
    if (!m_serverAddress.isNull() || m_resolving) {
        return;
    }

    const QString hostname = netplayIndexServerHostname();
    QHostAddress address;
    if (address.setAddress(hostname)) {
        m_serverAddress = address;
        return;
    }

    m_resolving = true;
    const int lookupId = ++m_resolveId;
    qInfo() << "JuiceTraversal: resolving" << hostname;
    QHostInfo::lookupHost(hostname, this, [this, lookupId, hostname](const QHostInfo& info) {
        if (lookupId != m_resolveId) {
            return;
        }
        m_resolving = false;
        if (info.error() != QHostInfo::NoError) {
            qWarning() << "JuiceTraversal: failed to resolve" << hostname << info.errorString();
            if (m_joining) {
                failJoin(QStringLiteral("Failed to resolve the NAT traversal server"));
            }
            return;
        }
        for (const QHostAddress& resolved : info.addresses()) {
            if (resolved.protocol() == QAbstractSocket::IPv4Protocol) {
                m_serverAddress = resolved;
                qInfo() << "JuiceTraversal: index server is" << resolved.toString();
                onTick();
                return;
            }
        }
        qWarning() << "JuiceTraversal: no IPv4 address for" << hostname;
        if (m_joining) {
            failJoin(QStringLiteral("NAT traversal server has no IPv4 address"));
        }
    });
}

void JuiceTraversal::sendIndex(const QByteArray& message)
{
    if (message.isEmpty()) {
        return;
    }
    if (m_socket.state() != QAbstractSocket::BoundState) {
        if (!m_socket.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            qWarning() << "JuiceTraversal: index socket bind failed:" << m_socket.errorString();
            return;
        }
    }
    if (!ensureServerAddress()) {
        return;
    }
    if (m_socket.writeDatagram(message, m_serverAddress, static_cast<quint16>(kNetplayRegistryPort)) < 0) {
        qWarning() << "JuiceTraversal: index send failed:" << m_socket.errorString();
        return;
    }
    if (m_firstPingMs == 0 && !message.contains("|PING")) {
        m_firstPingMs = monotonicMs();
    }
}

void JuiceTraversal::sendPing()
{
    sendIndex(QByteArray(kNetplayIndexProtocol) + "|PING");
}

void JuiceTraversal::markServerAlive()
{
    m_lastPongMs = monotonicMs();
}

bool JuiceTraversal::serverLooksDead(qint64 nowMs) const
{
    if (m_firstPingMs == 0 || m_serverAddress.isNull()) {
        return false;
    }
    const qint64 lastHeard = m_lastPongMs > 0 ? m_lastPongMs : m_firstPingMs;
    return nowMs - lastHeard >= kServerSilenceFailMs;
}

void JuiceTraversal::setJoinProgress(const QString& status)
{
    if (status.isEmpty() || status == m_lastProgress) {
        return;
    }
    m_lastProgress = status;
    emit joinProgress(status);
}

bool JuiceTraversal::turnRelayReady() const
{
    return turnRelayAvailable();
}

void JuiceTraversal::handleIndexMessage(const QByteArray& datagram)
{
    if (!datagram.startsWith(QByteArray(kNetplayIndexProtocol))) {
        return;
    }

    const QList<QByteArray> parts = datagram.split('|');
    if (parts.size() < 2) {
        return;
    }

    markServerAlive();

    if (parts[1] == "PONG") {
        return;
    }

    if (parts[1] == "SETOK" && parts.size() >= 3) {
        const QString key = QString::fromUtf8(parts[2]);
        if (m_joining && key == offerKey(m_joinCode, m_joinerId)) {
            m_offerAcked = true;
        }
        if (m_hosting && key.endsWith(QStringLiteral(".ans"))) {
            const QString prefix = QStringLiteral("ice/%1/").arg(m_hostCode);
            if (key.startsWith(prefix)) {
                QString joinerId = key.mid(prefix.size());
                joinerId.chop(4);
                if (m_pendingAnswers.contains(joinerId)) {
                    m_pendingAnswers[joinerId].acked = true;
                }
            }
        }
        return;
    }

    if (parts[1] == "LISTOK" && m_hosting) {
        const QStringList keys = parts.size() >= 3 ? QString::fromUtf8(parts[2]).split(',', Qt::SkipEmptyParts)
                                                    : QStringList();
        const QString prefix = QStringLiteral("ice/%1/").arg(m_hostCode);
        for (const QString& key : keys) {
            if (!key.startsWith(prefix) || key.endsWith(QStringLiteral(".ans"))) {
                continue;
            }
            const QString joinerId = key.mid(prefix.size());
            if (!isJoinerId(joinerId)) {
                continue;
            }
            if (JuicePeer* existing = m_hostPeers.value(joinerId)) {
                if (existing->isLinked()) {
                    continue;
                }
            }
            sendIndex(QByteArray(kNetplayIndexProtocol) + "|GET|" + key.toUtf8());
        }
        return;
    }

    if (parts[1] == "GETOK" && parts.size() >= 4) {
        const QString key = QString::fromUtf8(parts[2]);
        QByteArray value = parts[3];
        if (value.startsWith("B64:")) {
            value = QByteArray::fromBase64(value.mid(4));
        }
        if (value.isEmpty()) {
            return;
        }

        if (m_joining && key == answerKey(m_joinCode, m_joinerId) && !m_answerApplied && m_joiner != nullptr) {
            m_answerApplied = true;
            qInfo() << "libjuice received host ICE answer";
            setJoinProgress(QStringLiteral("Opening the connection…"));
            m_joiner->applyRemoteDescription(value);
            if (!m_turnRetryUsed) {
                m_stunOnlyDeadlineMs = monotonicMs() + kStunOnlyIceWaitMs;
            }
            return;
        }

        if (m_hosting) {
            const QString prefix = QStringLiteral("ice/%1/").arg(m_hostCode);
            if (key.startsWith(prefix) && !key.endsWith(QStringLiteral(".ans"))) {
                const QString joinerId = key.mid(prefix.size());
                if (isJoinerId(joinerId)) {
                    acceptOffer(joinerId, value);
                }
            }
        }
    }
}

void JuiceTraversal::publishValue(const QString& key, const QByteArray& value)
{
    if (!isValidIndexKey(key) || value.isEmpty()) {
        return;
    }
    const QByteArray message = QByteArray(kNetplayIndexProtocol) + "|SET|" + key.toUtf8() + "|B64:" +
                               value.toBase64();
    sendIndex(message);
}

void JuiceTraversal::deleteKey(const QString& key)
{
    if (!isValidIndexKey(key)) {
        return;
    }
    sendIndex(QByteArray(kNetplayIndexProtocol) + "|DEL|" + key.toUtf8());
}

void JuiceTraversal::startJoinerPeer(bool useTurn)
{
    m_joiner = new JuicePeer(JuicePeer::Role::Joiner, 0, this);
    m_joiner->onLocalDescription = [this](const QByteArray& sdp) {
        if (!m_joining) {
            return;
        }
        m_localOffer = sdp;
        m_offerGathered = true;
        m_nextOfferMs = 0;
        qInfo() << "libjuice gathered local description for" << m_joinCode
                << (iceSdpHasRelay(sdp) ? "with TURN relay candidates" : "without TURN relay candidates");
        setJoinProgress(QStringLiteral("Waiting for the host to answer…"));
    };
    m_joiner->onConnected = [this](quint16 port) {
        if (!m_joining || m_joinFinished) {
            return;
        }
        m_joinReady = true;
        m_joinFinished = true;
        m_stunOnlyDeadlineMs = 0;
        m_joinProxyPort = port;
        qInfo() << "libjuice NAT traversal connected, local proxy port" << port;
        emit joinSucceeded(port);
    };
    m_joiner->onFailed = [this](const QString& reason) {
        failJoin(reason);
    };

    setJoinProgress(useTurn ? QStringLiteral("Gathering relay candidates…")
                            : QStringLiteral("Gathering connection candidates…"));
    if (!m_joiner->start(QByteArray(), useTurn)) {
        failJoin(QStringLiteral("Failed to start ICE gathering"));
    }
}

bool JuiceTraversal::tryRetryJoinWithTurn(const QString& reason)
{
    if (m_turnRetryUsed || !m_joining || m_joinCode.isEmpty()) {
        return false;
    }
    const bool iceMiss = reason.contains(QLatin1String("ICE connection")) ||
                         reason.contains(QLatin1String("Direct ICE")) ||
                         reason.contains(QLatin1String("host ICE answer")) ||
                         reason.contains(QLatin1String("gathering ICE candidates"));
    if (!iceMiss) {
        return false;
    }
    if (!turnRelayReady()) {
        TurnCredentialClient& iceClient = TurnCredentialClient::instance();
        if (iceClient.isConfigured()) {
            iceClient.prefetch();
            if (m_turnWaitDeadlineMs == 0) {
                m_turnWaitDeadlineMs = monotonicMs() + kTurnWaitMs;
                setJoinProgress(QStringLiteral("Waiting for relay credentials…"));
                emit joinRetrying(reason);
            }
            return true;
        }
        qInfo() << "NAT traversal needs a relay after" << reason << "but no TURN server is available";
        return false;
    }

    const QString hostCode = m_joinCode;
    const QString previousJoinerId = m_joinerId;
    ++m_failureToken;
    m_turnRetryUsed = true;
    m_joinReady = false;
    m_joinFinished = false;
    m_offerGathered = false;
    m_offerAcked = false;
    m_answerApplied = false;
    m_localOffer.clear();
    m_joinProxyPort = 0;
    m_stunOnlyDeadlineMs = 0;
    m_turnWaitDeadlineMs = 0;
    m_joinDeadlineMs = monotonicMs() + kGatherTimeoutMs;
    m_nextOfferMs = 0;
    m_nextAnswerPollMs = 0;
    m_joinerId = makeJoinerId();

    if (m_joiner != nullptr) {
        m_joiner->detach();
        m_joiner->deleteLater();
        m_joiner = nullptr;
    }
    if (!previousJoinerId.isEmpty()) {
        deleteKey(offerKey(hostCode, previousJoinerId));
        deleteKey(answerKey(hostCode, previousJoinerId));
    }

    qInfo() << "Direct NAT traversal did not connect (" << reason << "); retrying with TURN as" << m_joinerId;
    emit joinRetrying(reason);
    startJoinerPeer(true);
    return m_joining && m_joiner != nullptr;
}

void JuiceTraversal::failJoin(const QString& reason)
{
    // A successful connect sets m_joinFinished while the agent is still live.
    // A later failure still has to be reported, or the UI keeps dialing the
    // localhost proxy after the tunnel is gone.
    if (!m_joining || (m_joinFinished && !m_joinReady)) {
        return;
    }
    if (tryRetryJoinWithTurn(reason)) {
        return;
    }
    m_joinFinished = true;
    m_joinReady = false;
    qWarning() << "JuiceTraversal:" << reason;

    const QString hostCode = m_joinCode;
    const QString joinerId = m_joinerId;
    m_joining = false;
    m_joinCode.clear();
    m_joinerId.clear();
    m_localOffer.clear();
    m_offerGathered = false;
    m_answerApplied = false;
    m_turnRetryUsed = false;
    m_offerAcked = false;
    m_joinProxyPort = 0;
    m_stunOnlyDeadlineMs = 0;
    m_turnWaitDeadlineMs = 0;
    m_lastProgress.clear();
    if (m_joiner != nullptr) {
        m_joiner->detach();
        m_joiner->deleteLater();
        m_joiner = nullptr;
    }
    if (!m_hosting) {
        m_timer.stop();
    }
    if (!hostCode.isEmpty() && !joinerId.isEmpty()) {
        deleteKey(offerKey(hostCode, joinerId));
        deleteKey(answerKey(hostCode, joinerId));
    }

    // Defer the signal so ICE callbacks and agent teardown are off the stack
    // before the UI opens a dialog. A newer join or cancel bumps the token.
    const int token = ++m_failureToken;
    QTimer::singleShot(0, this, [this, reason, token]() {
        if (token != m_failureToken) {
            return;
        }
        emit joinFailed(reason);
    });
}

void JuiceTraversal::resetJoin()
{
    m_joining = false;
    m_joinReady = false;
    m_joinFinished = false;
    m_offerGathered = false;
    m_offerAcked = false;
    m_answerApplied = false;
    m_turnRetryUsed = false;
    m_localOffer.clear();
    m_joinCode.clear();
    m_joinerId.clear();
    m_joinProxyPort = 0;
    m_stunOnlyDeadlineMs = 0;
    m_turnWaitDeadlineMs = 0;
    m_firstPingMs = 0;
    m_lastPongMs = 0;
    m_nextPingMs = 0;
    m_lastProgress.clear();
    if (m_joiner != nullptr) {
        m_joiner->detach();
        m_joiner->deleteLater();
        m_joiner = nullptr;
    }
}

void JuiceTraversal::acceptOffer(const QString& joinerId, const QByteArray& sdp)
{
    if (!m_hosting || sdp.isEmpty()) {
        return;
    }
    if (JuicePeer* existing = m_hostPeers.value(joinerId)) {
        if (existing->isLinked() || existing->hasSameRemote(sdp)) {
            return;
        }
        m_hostPeers.remove(joinerId);
        m_pendingAnswers.remove(joinerId);
        existing->detach();
        existing->deleteLater();
        qInfo() << "NAT traversal replacing ICE session for" << joinerId;
    }

    auto* peer = new JuicePeer(JuicePeer::Role::Host, m_signalingPort, this);
    peer->onLocalDescription = [this, joinerId](const QByteArray& answer) {
        if (!m_hosting) {
            return;
        }
        PendingAnswer pending;
        pending.sdp = answer;
        pending.acked = false;
        pending.nextPublishMs = 0;
        m_pendingAnswers.insert(joinerId, pending);
        qInfo() << "NAT traversal answering" << joinerId << "— publishing the ICE answer until the server acks";
        publishValue(answerKey(m_hostCode, joinerId), answer);
    };
    peer->onFailed = [this, joinerId](const QString& reason) {
        qWarning() << "libjuice joiner" << joinerId << "failed:" << reason;
        m_pendingAnswers.remove(joinerId);
        if (JuicePeer* existing = m_hostPeers.take(joinerId)) {
            existing->detach();
            existing->deleteLater();
        }
    };
    m_hostPeers.insert(joinerId, peer);
    const bool useTurn = iceSdpHasRelay(sdp);
    qInfo() << "NAT traversal answering" << joinerId
            << (useTurn ? "with TURN because the offer includes a relay" : "STUN/direct first");
    peer->start(sdp, useTurn);
}

QString JuiceTraversal::offerKey(const QString& hostCode, const QString& joinerId) const
{
    return QStringLiteral("ice/%1/%2").arg(hostCode, joinerId);
}

QString JuiceTraversal::answerKey(const QString& hostCode, const QString& joinerId) const
{
    return offerKey(hostCode, joinerId) + QStringLiteral(".ans");
}

} // namespace UserInterface::Netplay
