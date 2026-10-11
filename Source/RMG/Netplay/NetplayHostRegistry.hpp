/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#ifndef NETPLAYHOSTREGISTRY_HPP
#define NETPLAYHOSTREGISTRY_HPP

#include "NetplayProtocol.hpp"

#include <QObject>
#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QUdpSocket>
#include <QTimer>

struct _ENetHost;

namespace UserInterface::Netplay {

/** Registers the host with the browse server (UDP) for session browser listing. */
class NetplayHostRegistry : public QObject {
    Q_OBJECT

public:
    explicit NetplayHostRegistry(QObject* parent = nullptr);
    ~NetplayHostRegistry() override;

    QString hostCode() const;
    /** Send REGISTER/KEEP from the hosting ENet socket so its NAT mapping is the one joiners punch. */
    void attachSignalingHost(_ENetHost* host);
    void startHosting(uint16_t signalingPort, bool listInBrowser);
    void resumeHosting(const QString& hostCode, uint16_t signalingPort, bool listInBrowser);
    void setListInBrowser(bool listInBrowser);
    void stopHosting(bool unregister = true);

signals:
    void hostRegistered(const QString& hostCode, const QString& publicAddress, int signalingPort);
    void hostRegistrationFailed(const QString& reason);

private:
    void sendToServer(const QByteArray& message);
    bool ensureSocketBound(QString* errorOut = nullptr);
    bool ensureServerResolved(QString* errorOut = nullptr);
    void handleServerMessage(const QByteArray& datagram);
    void failHosting(const QString& reason);
    void resetHostState();

    void onReadyRead();
    void onHousekeepingTimer();
    void onPunchTimer();
    void notePunchTarget(const QHostAddress& address, quint16 port);
    static void onEnetDatagram(const QByteArray& datagram, void* userData);

    QUdpSocket m_socket;
    QTimer m_housekeepingTimer;
    QHostAddress m_serverAddress;

    bool m_listInBrowser = false;
    uint16_t m_signalingPort = kDefaultNetplayHostingPort;
    QString m_hostCode;
    int m_hostRegisterAttempts = 0;
    qint64 m_nextHostRegisterMs = 0;
    qint64 m_nextHostKeepMs = 0;
    bool m_isHosting = false;
    bool m_registrationAbandoned = false;

    struct PunchTarget {
        QHostAddress address;
        quint16 port = 0;
        int burstsLeft = 0;
        qint64 lastSendMs = 0;
    };

    _ENetHost* m_enetHost = nullptr;
    QTimer m_punchTimer;
    QList<PunchTarget> m_punchTargets;
};

} // namespace UserInterface::Netplay

#endif // NETPLAYHOSTREGISTRY_HPP
