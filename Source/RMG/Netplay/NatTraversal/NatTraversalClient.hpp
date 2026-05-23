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

    void startHosting(uint16_t signalingPort);
    void stopHosting();

    void lookupHost(const QString& hostCode);
    void cancelLookup();

signals:
    void hostRegistered(const QString& hostCode);
    void hostRegistrationFailed(const QString& reason);
    void hostLookupSucceeded(const QString& address, int port);
    void hostLookupFailed(const QString& reason);

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

    static QList<QByteArray> splitTraversalParts(QByteArray datagram);

    QUdpSocket m_socket;
    QHostAddress m_serverAddress;
    QTimer m_housekeepingTimer;

    Mode m_mode = Mode::Idle;
    uint16_t m_signalingPort = 9290;

    QString m_hostCode;
    int m_hostRegisterAttempts = 0;
    qint64 m_nextHostRegisterMs = 0;
    qint64 m_nextHostKeepMs = 0;

    QString m_joinCode;
    qint64 m_joinDeadlineMs = 0;
    qint64 m_nextJoinRequestMs = 0;
};

} // namespace UserInterface::Netplay

#endif // NATTRAVERSALCLIENT_HPP
