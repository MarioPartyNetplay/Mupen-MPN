/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#ifndef NATTRAVERSALCLIENT_HPP
#define NATTRAVERSALCLIENT_HPP

#include "NatTraversalProtocol.hpp"

#include <QHostAddress>
#include <QObject>
#include <QUdpSocket>
#include <QTimer>

namespace UserInterface::Netplay {

class NatTraversalClient : public QObject {
    Q_OBJECT

public:
    explicit NatTraversalClient(QObject* parent = nullptr);
    ~NatTraversalClient() override;

    bool isHosting() const;
    bool isLookupActive() const;
    QString hostCode() const;

    void startHosting(uint16_t signalingPort, bool listInBrowser = true);
    void resumeHosting(const QString& hostCode, uint16_t signalingPort, bool listInBrowser = true);
    void setListInBrowser(bool listInBrowser);
    void stopHosting(bool unregister = true);

    void lookupHost(const QString& hostCode);
    void cancelLookup();
    // STUN binding on the signaling port when hosting (port-restricted cone NAT).
    void queryStunServer(const QString& hostname = QString(), uint16_t port = 0, uint16_t localBindPort = 0);
    void sendHolePunch(const QString& address, uint16_t targetPort, uint16_t localPort = 0);

signals:
    void hostRegistered(const QString& hostCode, const QString& publicAddress, int signalingPort);
    void hostRegistrationFailed(const QString& reason);
    void hostLookupSucceeded(const QString& address, int port);
    void hostLookupFailed(const QString& reason);
    // STUN: resolve public IP:port by querying a STUN server (RFC 5389)
    void publicAddressResolved(const QString& address, int port);
    void publicAddressFailed(const QString& reason);

private slots:
    void onReadyRead();
    void onHousekeepingTimer();

private:
    enum class Mode {
        Idle,
        Hosting,
        Joining,
    };

    bool ensureSocketBound(QString* errorOut);
    bool ensureServerResolved(QString* errorOut);
    void sendToServer(const QByteArray& message);
    void handleServerMessage(const QByteArray& datagram);
    void failHosting(const QString& reason);
    void failLookup(const QString& reason);
    void resetJoinState();
    void resetHostState();
    void resetPeerPunchState();
    void tryStunServer(int serverIndex);
    void sendStunBindingRequest(const QHostAddress& serverAddr, quint16 port);
    void handlePunchRequest(const QString& address, int port);
    void schedulePeerPunch(const QString& address, int port);

    static QList<QByteArray> splitTraversalParts(QByteArray datagram);


    QUdpSocket m_socket;
    QHostAddress m_serverAddress;
    QTimer m_housekeepingTimer;

    Mode m_mode = Mode::Idle;
    uint16_t m_signalingPort = kDefaultNetplayHostingPort;

    QString m_hostCode;
    bool m_listInBrowser = true;
    int m_hostRegisterAttempts = 0;
    qint64 m_nextHostRegisterMs = 0;
    qint64 m_nextHostKeepMs = 0;

    QString m_joinCode;
    qint64 m_joinDeadlineMs = 0;
    qint64 m_nextJoinRequestMs = 0;
    bool m_joinGotHost = false;
    QString m_joinHostAddress;
    int m_joinHostPort = 0;
    int m_joinPunchAttempts = 0;
    qint64 m_nextJoinPunchMs = 0;
    bool m_joinLookupSucceededEmitted = false;

    QString m_peerPunchAddress;
    int m_peerPunchPort = 0;
    qint64 m_peerPunchDeadlineMs = 0;
    qint64 m_nextPeerPunchMs = 0;

    QString m_stunQueryPrimaryHost;
    quint16 m_stunQueryPrimaryPort = kDefaultStunPort;
    quint16 m_stunQueryLocalPort = 0;
    int m_stunQueryIndex = -1;
};

} // namespace UserInterface::Netplay

#endif // NATTRAVERSALCLIENT_HPP
