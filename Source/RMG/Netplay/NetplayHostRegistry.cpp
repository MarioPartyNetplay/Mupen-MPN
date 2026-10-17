/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "NetplayHostRegistry.hpp"
#include "NetplayEnet.hpp"

#include <QDateTime>
#include <QDebug>
#include <QHostInfo>
#include <QNetworkDatagram>

namespace UserInterface::Netplay {

namespace {

constexpr int kMaxHostRegisterAttempts = 40;
constexpr int kHostRegisterIntervalMs = 250;
constexpr int kHostRegisterBurst = 3;
constexpr int kHostKeepIntervalMs = 30000;
constexpr int kEnetKeepIntervalMs = 5000;
constexpr int kPunchSustainBursts = 20;

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
    m_housekeepingTimer.setInterval(250);
    resolveServerAsync();
    m_punchTimer.setInterval(100);
    connect(&m_punchTimer, &QTimer::timeout, this, &NetplayHostRegistry::onPunchTimer);
}

void NetplayHostRegistry::attachSignalingHost(_ENetHost* host)
{
    if (m_enetHost == host) {
        return;
    }

    if (m_enetHost) {
        clearEnetSideChannel(reinterpret_cast<ENetHost*>(m_enetHost));
    }

    m_enetHost = host;
    if (m_enetHost) {
        setEnetRegistryDatagramHandler(reinterpret_cast<ENetHost*>(m_enetHost),
                                       &NetplayHostRegistry::onEnetDatagram, this);
        qInfo() << "NAT punch keepalive will use the signaling socket";
    }
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

    resolveServerAsync();
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

    resolveServerAsync();
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
    if (m_isHosting) {
        if (unregister && !m_hostCode.isEmpty()) {
            sendToServer(QByteArray(kNetplayRegistryProtocol) + "|UNREGISTER|" + m_hostCode.toUtf8());
        }

        resetHostState();
        m_isHosting = false;
        m_housekeepingTimer.stop();
    }

    m_punchTimer.stop();
    m_punchTargets.clear();
    if (m_enetHost) {
        clearEnetSideChannel(reinterpret_cast<ENetHost*>(m_enetHost));
        m_enetHost = nullptr;
    }
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

    resolveServerAsync();
    if (errorOut) {
        *errorOut = m_resolving ? QStringLiteral("Resolving browse server")
                                : QString("Browse server %1 has no IPv4 address").arg(hostname);
    }
    return false;
}

void NetplayHostRegistry::resolveServerAsync()
{
    if (!m_serverAddress.isNull() || m_resolving) {
        return;
    }

    const QString hostname = netplayIndexServerHostname();
    QHostAddress address;
    if (address.setAddress(hostname)) {
        m_serverAddress = address;
        return;
    }

    m_resolving = true;
    const int lookupId = ++m_resolveId;
    QHostInfo::lookupHost(hostname, this, [this, lookupId](const QHostInfo& info) {
        if (lookupId != m_resolveId) {
            return;
        }
        m_resolving = false;
        if (info.error() != QHostInfo::NoError) {
            qWarning() << "Browse registry: failed to resolve" << info.errorString();
            return;
        }
        for (const QHostAddress& resolved : info.addresses()) {
            if (resolved.protocol() == QAbstractSocket::IPv4Protocol) {
                m_serverAddress = resolved;
                onHousekeepingTimer();
                return;
            }
        }
    });
}

void NetplayHostRegistry::sendToServer(const QByteArray& message)
{
    // REGISTER replies have to come back on this socket. Sending them from the
    // listening ENet port drops the reply, so the host code never arrives and
    // the room is never published.
    QString error;
    if (!ensureSocketBound(&error)) {
        if (m_hostRegisterAttempts >= kMaxHostRegisterAttempts) {
            m_registrationAbandoned = true;
            failHosting(error);
        }
        return;
    }
    if (!ensureServerResolved(&error)) {
        return;
    }

    if (m_socket.writeDatagram(message, m_serverAddress, kNetplayRegistryPort) < 0) {
        if (m_hostRegisterAttempts >= kMaxHostRegisterAttempts) {
            m_registrationAbandoned = true;
        }
        failHosting(QString("Failed to send browse registry packet: %1").arg(m_socket.errorString()));
    }
}

void NetplayHostRegistry::sendSignalingKeep()
{
    if (m_enetHost == nullptr || m_hostCode.isEmpty()) {
        return;
    }

    QString error;
    if (!ensureServerResolved(&error)) {
        qWarning() << "NAT punch keepalive:" << error;
        return;
    }

    const QByteArray keepMessage =
        QByteArray(kNetplayRegistryProtocol) + "|KEEP|" + m_hostCode.toUtf8() + "|" +
        QByteArray::number(m_signalingPort) + "|" +
        (m_listInBrowser ? "1" : "0");
    bool sent = false;
    for (int i = 0; i < 3; ++i) {
        sent = sendEnetDatagram(reinterpret_cast<ENetHost*>(m_enetHost), m_serverAddress,
                                static_cast<quint16>(kNetplayRegistryPort), keepMessage) || sent;
    }
    if (!sent) {
        qWarning() << "NAT punch keepalive failed on the signaling socket";
        return;
    }

    m_nextHostKeepMs = monotonicMs() + kEnetKeepIntervalMs;
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
        // Move the server's public mapping onto the game socket before the
        // room is published, so a joiner punches that port.
        sendSignalingKeep();
        emit hostRegistered(m_hostCode, publicAddress, signalingPort);
        return;
    }

    if (type == "PUNCH" && parts.size() >= 4) {
        QHostAddress address;
        if (!address.setAddress(QString::fromUtf8(parts[2]).trimmed())) {
            return;
        }
        bool ok = false;
        const uint port = parts[3].trimmed().toUInt(&ok);
        if (!ok || port == 0 || port > 65535) {
            return;
        }
        notePunchTarget(address, static_cast<quint16>(port));
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
            for (int i = 0; i < kHostRegisterBurst; ++i) {
                sendToServer(registerMessage);
            }
            ++m_hostRegisterAttempts;
            m_nextHostRegisterMs = now + kHostRegisterIntervalMs;
        }
        return;
    }

    if (m_nextHostKeepMs == 0 || now >= m_nextHostKeepMs) {
        if (m_enetHost != nullptr) {
            sendSignalingKeep();
            if (m_nextHostKeepMs > now) {
                return;
            }
        }

        const QByteArray keepMessage =
            QByteArray(kNetplayRegistryProtocol) + "|KEEP|" + m_hostCode.toUtf8() + "|" +
            QByteArray::number(m_signalingPort) + "|" +
            (m_listInBrowser ? "1" : "0");
        sendToServer(keepMessage);
        m_nextHostKeepMs = now + (m_enetHost != nullptr ? kEnetKeepIntervalMs : kHostKeepIntervalMs);
    }
}

void NetplayHostRegistry::onEnetDatagram(const QByteArray& datagram, void* userData)
{
    if (auto* self = static_cast<NetplayHostRegistry*>(userData)) {
        self->handleServerMessage(datagram);
    }
}

void NetplayHostRegistry::notePunchTarget(const QHostAddress& address, quint16 port)
{
    if (!m_enetHost || address.isNull() || address.protocol() != QAbstractSocket::IPv4Protocol || port == 0) {
        return;
    }

    int index = -1;
    for (int i = 0; i < m_punchTargets.size(); ++i) {
        if (m_punchTargets.at(i).address == address && m_punchTargets.at(i).port == port) {
            index = i;
            break;
        }
    }

    const bool fresh = index < 0 || m_punchTargets.at(index).burstsLeft <= 0;
    if (index < 0) {
        if (m_punchTargets.size() >= 4) {
            m_punchTargets.removeFirst();
        }
        PunchTarget target;
        target.address = address;
        target.port = port;
        m_punchTargets.append(target);
        index = m_punchTargets.size() - 1;
    }

    PunchTarget& target = m_punchTargets[index];
    target.burstsLeft = kPunchSustainBursts;
    const qint64 now = monotonicMs();
    if (fresh || now - target.lastSendMs >= 50) {
        if (fresh) {
            qInfo() << "NAT punch to" << address.toString() << port;
        }
        sendNatPunch(reinterpret_cast<ENetHost*>(m_enetHost), target.address, target.port);
        target.lastSendMs = now;
    }
    if (!m_punchTimer.isActive()) {
        m_punchTimer.start();
    }
}

void NetplayHostRegistry::onPunchTimer()
{
    if (!m_enetHost) {
        m_punchTargets.clear();
        m_punchTimer.stop();
        return;
    }

    bool any = false;
    for (PunchTarget& target : m_punchTargets) {
        if (target.burstsLeft <= 0 || target.port == 0) {
            continue;
        }
        sendNatPunch(reinterpret_cast<ENetHost*>(m_enetHost), target.address, target.port);
        target.lastSendMs = monotonicMs();
        --target.burstsLeft;
        any = any || target.burstsLeft > 0;
    }

    if (!any) {
        m_punchTargets.clear();
        m_punchTimer.stop();
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
