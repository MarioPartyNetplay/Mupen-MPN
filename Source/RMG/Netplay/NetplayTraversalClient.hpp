/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#ifndef NETPLAYTRAVERSALCLIENT_HPP
#define NETPLAYTRAVERSALCLIENT_HPP

#include "NetplayEnet.hpp"
#include "NetplayProtocol.hpp"

#include <QObject>
#include <QHostAddress>
#include <QUdpSocket>
#include <QTimer>

struct _ENetHost;

namespace UserInterface::Netplay {

/** Dolphin-style host code lookup and UDP hole punching via the traversal server. */
class NetplayTraversalClient : public QObject {
    Q_OBJECT

public:
    explicit NetplayTraversalClient(QObject* parent = nullptr);
    ~NetplayTraversalClient() override;

    /** When set, hole punch bursts use the ENet signaling socket (same port as netplay). */
    void setEnetHost(ENetHost* enetHost);

    void lookupHost(const QString& hostCode);
    void cancel();

    /** Keep punching the resolved host while the signaling connection is retrying. */
    void continuePunchingHost(const QString& address, int port);

signals:
    void lookupSucceeded(const QString& address, int port);
    void lookupFailed(const QString& reason);

private:
    void resetState();
    bool ensureSocketBound(QString* errorOut = nullptr);
    bool ensureServerResolved(QString* errorOut = nullptr);
    void sendToServer(const QByteArray& message);
    void sendLookup();
    void handleServerMessage(const QByteArray& datagram);
    void startPunching(const QString& address, int port);
    void sendPunchBurst();
    void finishLookupFailure(const QString& reason);

    void onReadyRead();
    void onLookupTimeout();
    void onPunchTimer();

    ENetHost* m_enetHost = nullptr;
    QUdpSocket m_socket;
    QTimer m_lookupTimeoutTimer;
    QTimer m_punchTimer;
    QHostAddress m_serverAddress;

    QString m_hostCode;
    QString m_resolvedAddress;
    int m_resolvedPort = 0;
    QString m_punchAddress;
    int m_punchPort = 0;
    bool m_lookupFinished = false;
    int m_lookupAttempts = 0;
};

/** Sends repeated UDP packets to a peer endpoint to open a NAT mapping. */
void sendTraversalPunchBurst(QUdpSocket& socket, const QHostAddress& target, quint16 port);
void sendTraversalPunchBurst(ENetHost* host, const QHostAddress& target, quint16 port);

} // namespace UserInterface::Netplay

#endif // NETPLAYTRAVERSALCLIENT_HPP
