/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#ifndef NETPLAYHOSTREGISTRY_HPP
#define NETPLAYHOSTREGISTRY_HPP

#include "NetplayEnet.hpp"
#include "NetplayProtocol.hpp"

#include <QObject>
#include <QHostAddress>
#include <QUdpSocket>
#include <QTimer>
#include <QSet>

struct _ENetHost;

namespace UserInterface::Netplay {

/** Registers the host with the browse server (UDP) and handles traversal hole-punch packets. */
class NetplayHostRegistry : public QObject {
    Q_OBJECT

public:
    explicit NetplayHostRegistry(QObject* parent = nullptr);
    ~NetplayHostRegistry() override;

    QString hostCode() const;
    void startHosting(uint16_t signalingPort, bool listInBrowser);
    void resumeHosting(const QString& hostCode, uint16_t signalingPort, bool listInBrowser);
    void setListInBrowser(bool listInBrowser);
    void stopHosting(bool unregister = true);

    /** Dolphin-style: send REGISTER/KEEP and punch from the ENet signaling socket. */
    void attachEnetSignalingHost(ENetHost* host);
    void detachEnetSignalingHost();

signals:
    void hostRegistered(const QString& hostCode, const QString& publicAddress, int signalingPort);
    void hostRegistrationFailed(const QString& reason);
    /** Emitted when the traversal server coordinates a joiner punch (host should connect back). */
    void traversalConnectRequested(const QHostAddress& clientAddress, quint16 clientPort);

private:
    static void enetRegistryDatagramHandler(const QByteArray& datagram, void* userData);

    void sendToServer(const QByteArray& message);
    bool ensureSocketBound(QString* errorOut = nullptr);
    bool ensureServerResolved(QString* errorOut = nullptr);
    void handleServerMessage(const QByteArray& datagram);
    void requestTraversalConnect(const QHostAddress& clientAddress, quint16 clientPort);
    void failHosting(const QString& reason);
    void resetHostState();

    void onReadyRead();
    void onHousekeepingTimer();

    QUdpSocket m_socket;
    QTimer m_housekeepingTimer;
    QHostAddress m_serverAddress;
    ENetHost* m_enetHost = nullptr;
    QSet<QString> m_pendingTraversalConnectKeys;

    bool m_listInBrowser = false;
    uint16_t m_signalingPort = kDefaultNetplayHostingPort;
    QString m_hostCode;
    int m_hostRegisterAttempts = 0;
    qint64 m_nextHostRegisterMs = 0;
    qint64 m_nextHostKeepMs = 0;
    bool m_isHosting = false;
};

} // namespace UserInterface::Netplay

#endif // NETPLAYHOSTREGISTRY_HPP
