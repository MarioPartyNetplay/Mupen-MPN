/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "NatTraversalClient.hpp"

#include <QDateTime>
#include <QDebug>
#include <QHostInfo>
#include <QNetworkDatagram>

namespace UserInterface::Netplay {

namespace {

constexpr int kJoinTimeoutMs = 15000;
constexpr int kMaxHostRegisterAttempts = 4;

qint64 monotonicMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

NatTraversalClient::NatTraversalClient(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QUdpSocket::readyRead, this, &NatTraversalClient::onReadyRead);
    connect(&m_housekeepingTimer, &QTimer::timeout, this, &NatTraversalClient::onHousekeepingTimer);
    m_housekeepingTimer.setInterval(1000);
}

NatTraversalClient::~NatTraversalClient()
{
    stopHosting();
}

bool NatTraversalClient::isHosting() const
{
    return m_mode == Mode::Hosting;
}

bool NatTraversalClient::isLookupActive() const
{
    return m_mode == Mode::Joining;
}

QString NatTraversalClient::hostCode() const
{
    return m_hostCode;
}

void NatTraversalClient::startHosting(uint16_t signalingPort)
{
    cancelLookup();
    resetHostState();

    m_mode = Mode::Hosting;
    m_signalingPort = signalingPort;
    m_hostRegisterAttempts = 0;
    m_nextHostRegisterMs = 0;
    m_nextHostKeepMs = 0;

    if (!m_housekeepingTimer.isActive()) {
        m_housekeepingTimer.start();
    }

    onHousekeepingTimer();
}

void NatTraversalClient::stopHosting()
{
    if (m_mode != Mode::Hosting) {
        return;
    }

    if (!m_hostCode.isEmpty()) {
        sendToServer(QByteArray(kNatTraversalProtocol) + "|UNREGISTER|" + m_hostCode.toUtf8());
    }

    resetHostState();
    m_mode = Mode::Idle;

    if (!isLookupActive()) {
        m_housekeepingTimer.stop();
    }
}

void NatTraversalClient::lookupHost(const QString& hostCode)
{
    stopHosting();
    resetJoinState();

    const QString normalizedCode = normalizeTraversalCode(hostCode);
    if (normalizedCode.isEmpty()) {
        emit hostLookupFailed("Invalid host code");
        return;
    }

    m_mode = Mode::Joining;
    m_joinCode = normalizedCode;
    m_joinDeadlineMs = monotonicMs() + kJoinTimeoutMs;
    m_nextJoinRequestMs = 0;

    if (!m_housekeepingTimer.isActive()) {
        m_housekeepingTimer.start();
    }

    onHousekeepingTimer();
}

void NatTraversalClient::cancelLookup()
{
    if (m_mode != Mode::Joining) {
        return;
    }

    resetJoinState();
    m_mode = Mode::Idle;

    if (!isHosting()) {
        m_housekeepingTimer.stop();
    }
}

bool NatTraversalClient::ensureSocketBound(QString* errorOut)
{
    if (m_socket.state() == QAbstractSocket::BoundState) {
        return true;
    }

    if (!m_socket.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::DefaultForPlatform)) {
        if (errorOut) {
            *errorOut = QString("Failed to bind NAT traversal socket: %1").arg(m_socket.errorString());
        }
        return false;
    }

    return true;
}

bool NatTraversalClient::ensureServerResolved(QString* errorOut)
{
    if (!m_serverAddress.isNull()) {
        return true;
    }

    const QString hostname = natTraversalServerHostname();
    QHostAddress address;
    if (address.setAddress(hostname)) {
        m_serverAddress = address;
        return true;
    }

    const QHostInfo hostInfo = QHostInfo::fromName(hostname);
    if (hostInfo.error() != QHostInfo::NoError) {
        if (errorOut) {
            *errorOut = QString("Failed to resolve %1: %2").arg(hostname, hostInfo.errorString());
        }
        return false;
    }

    for (const QHostAddress& candidate : hostInfo.addresses()) {
        if (candidate.protocol() == QAbstractSocket::IPv4Protocol) {
            m_serverAddress = candidate;
            return true;
        }
    }

    if (errorOut) {
        *errorOut = QString("No addresses found for %1").arg(hostname);
    }
    return false;
}

void NatTraversalClient::sendToServer(const QByteArray& message)
{
    if (message.isEmpty()) {
        return;
    }

    QString error;
    if (!ensureSocketBound(&error)) {
        qWarning() << error;
        if (m_mode == Mode::Hosting) {
            failHosting(error);
        } else if (m_mode == Mode::Joining) {
            failLookup(error);
        }
        return;
    }

    if (!ensureServerResolved(&error)) {
        qWarning() << error;
        if (m_mode == Mode::Hosting) {
            failHosting(error);
        } else if (m_mode == Mode::Joining) {
            failLookup(error);
        }
        return;
    }

    if (m_socket.writeDatagram(message, m_serverAddress, kNatTraversalPort) < 0) {
        const QString sendError = QString("Failed to send NAT traversal packet: %1").arg(m_socket.errorString());
        qWarning() << sendError;
        if (m_mode == Mode::Hosting) {
            failHosting(sendError);
        } else if (m_mode == Mode::Joining) {
            failLookup(sendError);
        }
    }
}

QList<QByteArray> NatTraversalClient::splitTraversalParts(QByteArray datagram)
{
    QList<QByteArray> parts = datagram.split('|');
    if (parts.isEmpty() || parts[0] != QByteArray(kNatTraversalProtocol)) {
        return {};
    }
    return parts;
}

void NatTraversalClient::onReadyRead()
{
    while (m_socket.hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_socket.receiveDatagram();
        handleServerMessage(datagram.data());
    }
}

void NatTraversalClient::handleServerMessage(const QByteArray& datagram)
{
    const QList<QByteArray> parts = splitTraversalParts(datagram);
    if (parts.size() < 2) {
        return;
    }

    const QByteArray type = parts[1];

    if (type == "REGISTEROK" && parts.size() >= 3) {
        if (m_mode != Mode::Hosting) {
            return;
        }

        m_hostCode = normalizeTraversalCode(QString::fromUtf8(parts[2]));
        if (m_hostCode.isEmpty()) {
            failHosting("Invalid host code from NAT server");
            return;
        }

        m_hostRegisterAttempts = 0;
        m_nextHostRegisterMs = 0;
        m_nextHostKeepMs = 0;
        emit hostRegistered(m_hostCode);
        return;
    }

    if (type == "LOOKUPOK" && parts.size() >= 5) {
        if (m_mode != Mode::Joining) {
            return;
        }

        const QString code = normalizeTraversalCode(QString::fromUtf8(parts[2]));
        const QString address = QString::fromUtf8(parts[3]).trimmed();
        const int port = QString::fromUtf8(parts[4]).toInt();

        if (code != m_joinCode || address.isEmpty() || port <= 0) {
            failLookup("Invalid host lookup response");
            return;
        }

        resetJoinState();
        m_mode = Mode::Idle;
        m_housekeepingTimer.stop();
        emit hostLookupSucceeded(address, port);
        return;
    }

    if (type == "LOOKUPFAIL" && parts.size() >= 4) {
        if (m_mode != Mode::Joining) {
            return;
        }

        const QString reason = QString::fromUtf8(parts[3]);
        if (reason == "NOTFOUND") {
            failLookup("Host code not found");
        } else {
            failLookup("Host lookup failed: " + reason);
        }
        return;
    }

    if (type == "ERR" && parts.size() >= 3) {
        const QString reason = QString::fromUtf8(parts[2]);
        if (m_mode == Mode::Hosting) {
            failHosting("NAT server error: " + reason);
        } else if (m_mode == Mode::Joining) {
            failLookup("NAT server error: " + reason);
        }
    }
}

void NatTraversalClient::onHousekeepingTimer()
{
    const qint64 now = monotonicMs();

    if (m_mode == Mode::Hosting) {
        if (m_hostCode.isEmpty()) {
            if (m_nextHostRegisterMs == 0 || now >= m_nextHostRegisterMs) {
                if (m_hostRegisterAttempts >= kMaxHostRegisterAttempts) {
                    failHosting("Failed to get a host code from the NAT server");
                    return;
                }

                sendToServer(QByteArray(kNatTraversalProtocol) + "|REGISTER|" +
                             QByteArray::number(m_signalingPort));
                ++m_hostRegisterAttempts;
                m_nextHostRegisterMs = now + 2000;
            }
            return;
        }

        if (m_nextHostKeepMs == 0 || now >= m_nextHostKeepMs) {
            sendToServer(QByteArray(kNatTraversalProtocol) + "|KEEP|" + m_hostCode.toUtf8());
            m_nextHostKeepMs = now + 15000;
        }
        return;
    }

    if (m_mode == Mode::Joining) {
        if (m_joinDeadlineMs != 0 && now >= m_joinDeadlineMs) {
            failLookup("Timed out waiting for host");
            return;
        }

        if (m_nextJoinRequestMs == 0 || now >= m_nextJoinRequestMs) {
            sendToServer(QByteArray(kNatTraversalProtocol) + "|LOOKUP|" + m_joinCode.toUtf8());
            m_nextJoinRequestMs = now + 2000;
        }
    }
}

void NatTraversalClient::failHosting(const QString& reason)
{
    if (m_mode != Mode::Hosting) {
        return;
    }

    resetHostState();
    m_mode = Mode::Idle;
    m_housekeepingTimer.stop();
    emit hostRegistrationFailed(reason);
}

void NatTraversalClient::failLookup(const QString& reason)
{
    if (m_mode != Mode::Joining) {
        return;
    }

    resetJoinState();
    m_mode = Mode::Idle;
    m_housekeepingTimer.stop();
    emit hostLookupFailed(reason);
}

void NatTraversalClient::resetHostState()
{
    m_hostCode.clear();
    m_hostRegisterAttempts = 0;
    m_nextHostRegisterMs = 0;
    m_nextHostKeepMs = 0;
}

void NatTraversalClient::resetJoinState()
{
    m_joinCode.clear();
    m_joinDeadlineMs = 0;
    m_nextJoinRequestMs = 0;
}

} // namespace UserInterface::Netplay
