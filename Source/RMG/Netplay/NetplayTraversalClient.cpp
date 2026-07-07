/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "NetplayTraversalClient.hpp"

#include <QDateTime>
#include <QDebug>
#include <QHostInfo>
#include <QNetworkDatagram>

namespace UserInterface::Netplay {

namespace {

constexpr int kMaxLookupAttempts = 5;
constexpr int kLookupRetryIntervalMs = 1000;
constexpr int kLookupTimeoutMs = 15000;
constexpr int kPunchIntervalMs = 100;
constexpr int kPunchPacketsPerBurst = 20;

QList<QByteArray> splitTraversalParts(QByteArray datagram)
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

QByteArray traversalPunchPayload()
{
    return QByteArray(kNetplayRegistryProtocol) + "|PUNCH|";
}

} // namespace

void sendTraversalPunchBurst(QUdpSocket& socket, const QHostAddress& target, quint16 port)
{
    if (target.isNull() || port == 0) {
        return;
    }

    const QByteArray payload = traversalPunchPayload();
    for (int i = 0; i < kPunchPacketsPerBurst; ++i) {
        socket.writeDatagram(payload, target, port);
    }
}

void sendTraversalPunchBurst(ENetHost* host, const QHostAddress& target, quint16 port)
{
    if (!host || target.isNull() || port == 0) {
        return;
    }

    const QByteArray payload = traversalPunchPayload();
    for (int i = 0; i < kPunchPacketsPerBurst; ++i) {
        sendEnetDatagram(host, target, port, payload);
    }
}

NetplayTraversalClient::NetplayTraversalClient(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QUdpSocket::readyRead, this, &NetplayTraversalClient::onReadyRead);

    m_lookupTimeoutTimer.setSingleShot(true);
    connect(&m_lookupTimeoutTimer, &QTimer::timeout, this, &NetplayTraversalClient::onLookupTimeout);

    m_punchTimer.setInterval(kPunchIntervalMs);
    connect(&m_punchTimer, &QTimer::timeout, this, &NetplayTraversalClient::onPunchTimer);
}

NetplayTraversalClient::~NetplayTraversalClient()
{
    cancel();
}

void NetplayTraversalClient::setEnetHost(ENetHost* enetHost)
{
    m_enetHost = enetHost;
}

void NetplayTraversalClient::lookupHost(const QString& hostCode)
{
    cancel();

    const QString normalizedCode = normalizeTraversalCode(hostCode);
    if (normalizedCode.isEmpty()) {
        finishLookupFailure(QStringLiteral("Invalid host code"));
        return;
    }

    m_hostCode = normalizedCode;
    m_lookupAttempts = 0;
    m_lookupFinished = false;

    QString error;
    if (!ensureSocketBound(&error) || !ensureServerResolved(&error)) {
        finishLookupFailure(error);
        return;
    }

    if (m_enetHost) {
        setEnetRegistryDatagramHandler(m_enetHost, &NetplayTraversalClient::enetRegistryDatagramHandler, this);
    }

    m_lookupTimeoutTimer.start(kLookupTimeoutMs);
    sendLookup();
}

void NetplayTraversalClient::continuePunchingHost(const QString& address, int port)
{
    if (address.isEmpty() || port < 1024 || port > 65535) {
        return;
    }

    m_resolvedAddress = address;
    m_resolvedPort = port;
    startPunching(address, port);
}

void NetplayTraversalClient::cancel()
{
    m_lookupTimeoutTimer.stop();
    m_punchTimer.stop();
    clearEnetHandler();
    resetState();
}

void NetplayTraversalClient::clearEnetHandler()
{
    if (m_enetHost) {
        setEnetRegistryDatagramHandler(m_enetHost, nullptr, nullptr);
    }
}

void NetplayTraversalClient::resetState()
{
    m_hostCode.clear();
    m_resolvedAddress.clear();
    m_resolvedPort = 0;
    m_punchAddress.clear();
    m_punchPort = 0;
    m_lookupFinished = false;
    m_lookupAttempts = 0;
}

void NetplayTraversalClient::enetRegistryDatagramHandler(const QByteArray& datagram, void* userData)
{
    auto* self = static_cast<NetplayTraversalClient*>(userData);
    if (self) {
        self->handleServerMessage(datagram);
    }
}

bool NetplayTraversalClient::ensureSocketBound(QString* errorOut)
{
    if (m_enetHost) {
        return true;
    }

    if (m_socket.state() == QAbstractSocket::BoundState) {
        return true;
    }

    if (!m_socket.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to bind traversal lookup socket: %1").arg(m_socket.errorString());
        }
        return false;
    }

    return true;
}

bool NetplayTraversalClient::ensureServerResolved(QString* errorOut)
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
            *errorOut = QStringLiteral("Failed to resolve traversal server %1: %2")
                            .arg(hostname, info.errorString());
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
        *errorOut = QStringLiteral("Traversal server %1 has no IPv4 address").arg(hostname);
    }
    return false;
}

void NetplayTraversalClient::sendToServer(const QByteArray& message)
{
    QString error;
    if (!ensureSocketBound(&error) || !ensureServerResolved(&error)) {
        finishLookupFailure(error);
        return;
    }

    if (m_enetHost) {
        if (!sendEnetDatagram(m_enetHost, m_serverAddress, kNetplayRegistryPort, message)) {
            finishLookupFailure(QStringLiteral("Failed to send traversal lookup packet via signaling socket"));
        }
        return;
    }

    if (m_socket.writeDatagram(message, m_serverAddress, kNetplayRegistryPort) < 0) {
        finishLookupFailure(QStringLiteral("Failed to send traversal lookup packet: %1").arg(m_socket.errorString()));
    }
}

void NetplayTraversalClient::sendLookup()
{
    if (m_hostCode.isEmpty() || m_lookupFinished) {
        return;
    }

    const QByteArray lookupMessage =
        QByteArray(kNetplayRegistryProtocol) + "|LOOKUP|" + m_hostCode.toUtf8();
    sendToServer(lookupMessage);
    ++m_lookupAttempts;
}

void NetplayTraversalClient::onReadyRead()
{
    while (m_socket.hasPendingDatagrams()) {
        handleServerMessage(m_socket.receiveDatagram().data());
    }
}

void NetplayTraversalClient::handleServerMessage(const QByteArray& datagram)
{
    const QList<QByteArray> parts = splitTraversalParts(datagram);
    if (parts.size() < 2) {
        return;
    }

    const QByteArray type = parts[1];
    if (type == "LOOKUPOK" && parts.size() >= 5) {
        const QString code = normalizeTraversalCode(QString::fromUtf8(parts[2]));
        if (code.isEmpty() || code != m_hostCode) {
            return;
        }

        const QString address = QString::fromUtf8(parts[3]).trimmed();
        const int port = QString::fromUtf8(parts[4]).toInt();
        if (address.isEmpty() || port < 1024 || port > 65535) {
            finishLookupFailure(QStringLiteral("Traversal server returned an invalid host endpoint"));
            return;
        }

        m_resolvedAddress = address;
        m_resolvedPort = port;
        m_lookupFinished = true;
        m_lookupTimeoutTimer.stop();

        qInfo() << "NetplayTraversalClient: LOOKUPOK" << code << "->" << address << port
                << (m_enetHost ? "(via ENet signaling socket)" : "(via standalone UDP socket)");
        startPunching(address, port);
        emit lookupSucceeded(address, port);
        return;
    }

    if (type == "LOOKUPFAIL" && parts.size() >= 3) {
        const QString code = normalizeTraversalCode(QString::fromUtf8(parts[2]));
        if (!code.isEmpty() && code != m_hostCode) {
            return;
        }

        const QString reason = parts.size() >= 4 ? QString::fromUtf8(parts[3]) : QStringLiteral("NOTFOUND");
        finishLookupFailure(QStringLiteral("Host lookup failed: %1").arg(reason));
        return;
    }

    if (type == "PUNCH" && parts.size() >= 4) {
        const QString address = QString::fromUtf8(parts[2]).trimmed();
        const int port = QString::fromUtf8(parts[3]).toInt();
        if (address.isEmpty() || port < 1 || port > 65535) {
            return;
        }

        startPunching(address, port);
    }
}

void NetplayTraversalClient::startPunching(const QString& address, int port)
{
    m_punchAddress = address;
    m_punchPort = port;

    if (!m_punchTimer.isActive()) {
        m_punchTimer.start();
    }

    sendPunchBurst();
}

void NetplayTraversalClient::sendPunchBurst()
{
    if (m_punchAddress.isEmpty() || m_punchPort <= 0) {
        return;
    }

    QHostAddress target;
    if (!target.setAddress(m_punchAddress)) {
        return;
    }

    if (m_enetHost) {
        sendTraversalPunchBurst(m_enetHost, target, static_cast<quint16>(m_punchPort));
    } else {
        sendTraversalPunchBurst(m_socket, target, static_cast<quint16>(m_punchPort));
    }
}

void NetplayTraversalClient::onLookupTimeout()
{
    if (m_lookupFinished) {
        return;
    }

    if (m_lookupAttempts < kMaxLookupAttempts) {
        QTimer::singleShot(kLookupRetryIntervalMs, this, [this]() {
            if (!m_lookupFinished) {
                sendLookup();
            }
        });
        m_lookupTimeoutTimer.start(kLookupTimeoutMs);
        return;
    }

    finishLookupFailure(QStringLiteral("Timed out while looking up host code"));
}

void NetplayTraversalClient::onPunchTimer()
{
    sendPunchBurst();
}

void NetplayTraversalClient::finishLookupFailure(const QString& reason)
{
    if (m_lookupFinished) {
        return;
    }

    m_lookupFinished = true;
    m_lookupTimeoutTimer.stop();
    clearEnetHandler();
    qWarning() << "NetplayTraversalClient:" << reason;
    emit lookupFailed(reason);
}

} // namespace UserInterface::Netplay
