/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#ifndef NETPLAYHOSTREGISTRY_HPP
#define NETPLAYHOSTREGISTRY_HPP

#include "NetplayProtocol.hpp"

#include <QObject>
#include <QHostAddress>
#include <QUdpSocket>
#include <QTimer>

namespace UserInterface::Netplay {

/** Registers the host with the browse server (UDP) for session browser listing. */
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
};

} // namespace UserInterface::Netplay

#endif // NETPLAYHOSTREGISTRY_HPP
