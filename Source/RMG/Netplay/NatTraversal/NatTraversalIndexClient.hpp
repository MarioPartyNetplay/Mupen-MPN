/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#ifndef NATTRAVERSALINDEXCLIENT_HPP
#define NATTRAVERSALINDEXCLIENT_HPP

#include "NatTraversalProtocol.hpp"

#include <QHostAddress>
#include <QObject>
#include <QUdpSocket>
#include <QTimer>

namespace UserInterface::Netplay {

class NatTraversalIndexClient : public QObject {
    Q_OBJECT

public:
    explicit NatTraversalIndexClient(QObject* parent = nullptr);

    void publish(const QString& key, const QByteArray& data);
    void fetch(const QString& key);
    void publishSession(const QString& hostCode, const QByteArray& data);
    void fetchSession(const QString& hostCode);
    void cancel();

signals:
    void published(const QString& key);
    void publishFailed(const QString& reason);
    void fetched(const QString& key, const QByteArray& data);
    void fetchFailed(const QString& reason);

private slots:
    void onReadyRead();
    void onTimeout();

private:
    enum class PendingOp {
        None,
        Publish,
        Fetch,
    };

    bool ensureReady(QString* errorOut);
    void sendMessage(const QByteArray& message);
    void handleResponse(const QByteArray& datagram);
    void fail(const QString& reason);

    QUdpSocket m_socket;
    QHostAddress m_serverAddress;
    QTimer m_timeoutTimer;

    PendingOp m_pendingOp = PendingOp::None;
    QString m_pendingKey;
};

} // namespace UserInterface::Netplay

#endif // NATTRAVERSALINDEXCLIENT_HPP
