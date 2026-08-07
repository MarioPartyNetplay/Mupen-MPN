/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "NetplayHostRegistry.hpp"

#include <QDateTime>
#include <QDebug>
#include <QHostInfo>
#include <QNetworkDatagram>

namespace UserInterface::Netplay {

namespace {

constexpr int kMaxHostRegisterAttempts = 30;
constexpr int kHostRegisterIntervalMs = 1000;
constexpr int kHostKeepIntervalMs = 30000;

qint64 monotonicMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QList<QByteArray> splitRegistryParts(QByteArray datagram)
{
    if (datagram.size() < 8) {
        return {};
    }

    const QByteArray magic = datagram.left(8);
    if (magic != QByteArray(kNetplayRegistryProtocol)) {
        return {};
    }

    return datagram.split('|');
}

} // namespace

NetplayHostRegistry::NetplayHostRegistry(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QUdpSocket::readyRead, this, &NetplayHostRegistry::onReadyRead);
    connect(&m_housekeepingTimer, &QTimer::timeout, this, &NetplayHostRegistry::onHousekeepingTimer);
    m_housekeepingTimer.setInterval(1000);
}

NetplayHostRegistry::~NetplayHostRegistry()
{
    stopHosting(true);
}

QString NetplayHostRegistry::hostCode() const
{
    return m_hostCode;
}

void NetplayHostRegistry::startHosting(uint16_t signalingPort, bool listInBrowser)
{
    if (m_isHosting && !m_hostCode.isEmpty()) {
        sendToServer(QByteArray(kNetplayRegistryProtocol) + "|UNREGISTER|" + m_hostCode.toUtf8());
    }

    resetHostState();
    m_isHosting = true;
    m_signalingPort = signalingPort;
    m_listInBrowser = listInBrowser;
    m_hostRegisterAttempts = 0;
    m_nextHostRegisterMs = 0;
    m_nextHostKeepMs = 0;
    m_registrationAbandoned = false;

    if (!m_housekeepingTimer.isActive()) {
        m_housekeepingTimer.start();
    }

    onHousekeepingTimer();
}

void NetplayHostRegistry::resumeHosting(const QString& hostCode, uint16_t signalingPort, bool listInBrowser)
{
    const QString normalizedCode = normalizeTraversalCode(hostCode);
    if (normalizedCode.isEmpty()) {
        return;
    }

    m_isHosting = true;
    m_signalingPort = signalingPort;
    m_listInBrowser = listInBrowser;
    m_hostCode = normalizedCode;
    m_hostRegisterAttempts = 0;
    m_nextHostRegisterMs = 0;
    m_nextHostKeepMs = 0;
    m_registrationAbandoned = false;

    if (!m_housekeepingTimer.isActive()) {
        m_housekeepingTimer.start();
    }

    onHousekeepingTimer();
}

void NetplayHostRegistry::setListInBrowser(bool listInBrowser)
{
    m_listInBrowser = listInBrowser;
}

void NetplayHostRegistry::stopHosting(bool unregister)
{
    if (!m_isHosting) {
        return;
    }

    if (unregister && !m_hostCode.isEmpty()) {
        sendToServer(QByteArray(kNetplayRegistryProtocol) + "|UNREGISTER|" + m_hostCode.toUtf8());
    }

    resetHostState();
    m_isHosting = false;
    m_housekeepingTimer.stop();
}

bool NetplayHostRegistry::ensureSocketBound(QString* errorOut)
{
    if (m_socket.state() == QAbstractSocket::BoundState) {
        return true;
    }

    if (!m_socket.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        if (errorOut) {
            *errorOut = QString("Failed to bind browse registry socket: %1").arg(m_socket.errorString());
        }
        return false;
    }

    return true;
}

bool NetplayHostRegistry::ensureServerResolved(QString* errorOut)
{
    if (!m_serverAddress.isNull()) {
        return true;
    }

    const QString hostname = netplayIndexServerHostname();
    QHostAddress address;
    if (address.setAddress(hostname)) {
        m_serverAddress = address;
        return true;
    }

    QHostInfo info = QHostInfo::fromName(hostname);
    if (info.error() != QHostInfo::NoError) {
        if (errorOut) {
            *errorOut = QString("Failed to resolve browse server %1: %2").arg(hostname, info.errorString());
        }
        return false;
    }

    for (const QHostAddress& resolved : info.addresses()) {
        if (resolved.protocol() == QAbstractSocket::IPv4Protocol) {
            m_serverAddress = resolved;
            return true;
        }
    }

    if (errorOut) {
        *errorOut = QString("Browse server %1 has no IPv4 address").arg(hostname);
    }
    return false;
}

void NetplayHostRegistry::sendToServer(const QByteArray& message)
{
    QString error;
    if (!ensureSocketBound(&error) || !ensureServerResolved(&error)) {
        if (m_hostRegisterAttempts >= kMaxHostRegisterAttempts) {
            m_registrationAbandoned = true;
        }
        failHosting(error);
        return;
    }

    if (m_socket.writeDatagram(message, m_serverAddress, kNetplayRegistryPort) < 0) {
        if (m_hostRegisterAttempts >= kMaxHostRegisterAttempts) {
            m_registrationAbandoned = true;
        }
        failHosting(QString("Failed to send browse registry packet: %1").arg(m_socket.errorString()));
    }
}

void NetplayHostRegistry::onReadyRead()
{
    while (m_socket.hasPendingDatagrams()) {
        handleServerMessage(m_socket.receiveDatagram().data());
    }
}

void NetplayHostRegistry::handleServerMessage(const QByteArray& datagram)
{
    const QList<QByteArray> parts = splitRegistryParts(datagram);
    if (parts.size() < 2) {
        return;
    }

    const QByteArray type = parts[1];
    if (type == "REGISTEROK" && parts.size() >= 3) {
        const QString code = normalizeTraversalCode(QString::fromUtf8(parts[2]));
        if (code.isEmpty()) {
            failHosting("Invalid host code from browse server");
            return;
        }

        QString publicAddress;
        int signalingPort = m_signalingPort;
        if (parts.size() >= 5) {
            publicAddress = QString::fromUtf8(parts[3]).trimmed();
            const int parsedPort = QString::fromUtf8(parts[4]).toInt();
            if (parsedPort >= 1024 && parsedPort <= 65535) {
                signalingPort = parsedPort;
            }
        }

        m_hostCode = code;
        m_signalingPort = static_cast<uint16_t>(signalingPort);
        m_hostRegisterAttempts = 0;
        m_nextHostRegisterMs = 0;
        m_nextHostKeepMs = 0;
        emit hostRegistered(m_hostCode, publicAddress, signalingPort);
        return;
    }

    if (type == "ERR" && parts.size() >= 3) {
        failHosting("Browse server error: " + QString::fromUtf8(parts[2]));
    }
}

void NetplayHostRegistry::onHousekeepingTimer()
{
    if (!m_isHosting || m_registrationAbandoned) {
        return;
    }

    const qint64 now = monotonicMs();

    if (m_hostCode.isEmpty()) {
        if (m_nextHostRegisterMs == 0 || now >= m_nextHostRegisterMs) {
            if (m_hostRegisterAttempts >= kMaxHostRegisterAttempts) {
                m_registrationAbandoned = true;
                failHosting("Failed to register with the browse server");
                return;
            }

            const QByteArray registerMessage =
                QByteArray(kNetplayRegistryProtocol) + "|REGISTER|" +
                QByteArray::number(m_signalingPort) + "|" +
                (m_listInBrowser ? "1" : "0");
            sendToServer(registerMessage);
            ++m_hostRegisterAttempts;
            m_nextHostRegisterMs = now + kHostRegisterIntervalMs;
        }
        return;
    }

    if (m_nextHostKeepMs == 0 || now >= m_nextHostKeepMs) {
        const QByteArray keepMessage =
            QByteArray(kNetplayRegistryProtocol) + "|KEEP|" + m_hostCode.toUtf8() + "|" +
            QByteArray::number(m_signalingPort) + "|" +
            (m_listInBrowser ? "1" : "0");
        sendToServer(keepMessage);
        m_nextHostKeepMs = now + kHostKeepIntervalMs;
    }
}

void NetplayHostRegistry::failHosting(const QString& reason)
{
    qWarning() << "NetplayHostRegistry:" << reason;
    emit hostRegistrationFailed(reason);
}

void NetplayHostRegistry::resetHostState()
{
    m_hostCode.clear();
    m_hostRegisterAttempts = 0;
    m_nextHostRegisterMs = 0;
    m_nextHostKeepMs = 0;
    m_registrationAbandoned = false;
}

} // namespace UserInterface::Netplay
