/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#ifndef JUICETRAVERSAL_HPP
#define JUICETRAVERSAL_HPP

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUdpSocket>

#include <cstdint>

namespace UserInterface::Netplay {

/**
 * libjuice ICE session signaled through the NAT server's N02IDX1 index.
 *
 * The host answers each joiner offer stored on the server. ICE tries host and
 * STUN paths first; TURN is gathered only if that session fails. Once ICE
 * connects, ENet signaling is tunneled with juice_send / juice_recv through a
 * localhost UDP proxy so server-reflexive and TURN candidates can carry the
 * existing signaling protocol.
 *
 * The index socket pings the server so a dead UDP path fails in seconds instead
 * of sitting on "Opening a NAT traversal connection…". Offers and answers are
 * republished until SETOK, and the server can push GETOK to watchers.
 */
class JuiceTraversal : public QObject {
    Q_OBJECT

public:
    explicit JuiceTraversal(QObject* parent = nullptr);
    ~JuiceTraversal() override;

    void startHosting(const QString& hostCode, quint16 signalingPort);
    void stopHosting();
    bool isHosting() const;

    void joinHost(const QString& hostCode);
    void cancelJoin();
    bool isJoinReady() const;
    quint16 joinProxyPort() const;

signals:
    void joinSucceeded(quint16 localProxyPort);
    void joinFailed(const QString& reason);
    void joinRetrying(const QString& reason);
    void joinProgress(const QString& status);

private:
    class JuicePeer;
    struct PendingAnswer {
        QByteArray sdp;
        bool acked = false;
        qint64 nextPublishMs = 0;
    };

    void onTick();
    void onReadyRead();
    bool ensureServerAddress();
    void resolveServerAsync();
    void sendIndex(const QByteArray& message);
    void sendPing();
    void markServerAlive();
    bool serverLooksDead(qint64 nowMs) const;
    void handleIndexMessage(const QByteArray& datagram);
    void publishValue(const QString& key, const QByteArray& value);
    void deleteKey(const QString& key);
    void failJoin(const QString& reason);
    bool tryRetryJoinWithTurn(const QString& reason);
    bool turnRelayReady() const;
    void startJoinerPeer(bool useTurn);
    void resetJoin();
    void acceptOffer(const QString& joinerId, const QByteArray& sdp);
    void setJoinProgress(const QString& status);
    QString offerKey(const QString& hostCode, const QString& joinerId) const;
    QString answerKey(const QString& hostCode, const QString& joinerId) const;

    QUdpSocket m_socket;
    QTimer m_timer;
    QHostAddress m_serverAddress;
    bool m_resolving = false;
    int m_resolveId = 0;

    bool m_hosting = false;
    QString m_hostCode;
    quint16 m_signalingPort = 0;
    qint64 m_nextListMs = 0;

    bool m_joining = false;
    bool m_joinReady = false;
    bool m_joinFinished = false;
    int m_failureToken = 0;
    QString m_joinCode;
    QString m_joinerId;
    QByteArray m_localOffer;
    bool m_offerGathered = false;
    bool m_offerAcked = false;
    bool m_answerApplied = false;
    bool m_turnRetryUsed = false;
    quint16 m_joinProxyPort = 0;
    qint64 m_joinDeadlineMs = 0;
    qint64 m_stunOnlyDeadlineMs = 0;
    qint64 m_turnWaitDeadlineMs = 0;
    qint64 m_nextOfferMs = 0;
    qint64 m_nextAnswerPollMs = 0;
    qint64 m_nextPingMs = 0;
    qint64 m_firstPingMs = 0;
    qint64 m_lastPongMs = 0;
    QString m_lastProgress;

    JuicePeer* m_joiner = nullptr;
    QHash<QString, JuicePeer*> m_hostPeers;
    QHash<QString, PendingAnswer> m_pendingAnswers;
};

} // namespace UserInterface::Netplay

#endif // JUICETRAVERSAL_HPP
