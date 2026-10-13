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

private:
    class JuicePeer;

    void onTick();
    void onReadyRead();
    bool ensureServerAddress();
    void sendIndex(const QByteArray& message);
    void handleIndexMessage(const QByteArray& datagram);
    void publishValue(const QString& key, const QByteArray& value);
    void deleteKey(const QString& key);
    void failJoin(const QString& reason);
    bool tryRetryJoinWithTurn(const QString& reason);
    void startJoinerPeer(bool useTurn);
    void resetJoin();
    void acceptOffer(const QString& joinerId, const QByteArray& sdp);
    QString offerKey(const QString& hostCode, const QString& joinerId) const;
    QString answerKey(const QString& hostCode, const QString& joinerId) const;

    QUdpSocket m_socket;
    QTimer m_timer;
    QHostAddress m_serverAddress;

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
    bool m_answerApplied = false;
    bool m_turnRetryUsed = false;
    int m_offerPublishes = 0;
    quint16 m_joinProxyPort = 0;
    qint64 m_joinDeadlineMs = 0;
    qint64 m_stunOnlyDeadlineMs = 0;
    qint64 m_nextOfferMs = 0;
    qint64 m_nextAnswerPollMs = 0;

    JuicePeer* m_joiner = nullptr;
    QHash<QString, JuicePeer*> m_hostPeers;
};

} // namespace UserInterface::Netplay

#endif // JUICETRAVERSAL_HPP
