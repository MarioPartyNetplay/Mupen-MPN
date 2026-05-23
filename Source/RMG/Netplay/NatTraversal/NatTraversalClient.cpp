/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "NatTraversalClient.hpp"

#include <QDateTime>
#include <QDebug>
#include <QHostInfo>
#include <QNetworkDatagram>
#include <QRandomGenerator>
#include <QTimer>

namespace UserInterface::Netplay {

namespace {

constexpr int kJoinTimeoutMs = 30000;
constexpr int kMaxHostRegisterAttempts = 30;
constexpr int kHostRegisterIntervalMs = 1000;

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
    stopHosting(true);
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

void NatTraversalClient::resumeHosting(const QString& hostCode, uint16_t signalingPort)
{
    cancelLookup();

    const QString normalizedCode = normalizeTraversalCode(hostCode);
    if (normalizedCode.isEmpty()) {
        return;
    }

    m_mode = Mode::Hosting;
    m_signalingPort = signalingPort;
    m_hostCode = normalizedCode;
    m_hostRegisterAttempts = 0;
    m_nextHostRegisterMs = 0;
    m_nextHostKeepMs = 0;

    if (!m_housekeepingTimer.isActive()) {
        m_housekeepingTimer.start();
    }

    onHousekeepingTimer();
}

void NatTraversalClient::stopHosting(bool unregister)
{
    if (m_mode != Mode::Hosting) {
        return;
    }

    if (unregister && !m_hostCode.isEmpty()) {
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

QByteArray sanitizeProtocolDatagram(QByteArray datagram)
{
    datagram.replace('\0', QByteArray());
    return datagram;
}

QList<QByteArray> NatTraversalClient::splitTraversalParts(QByteArray datagram)
{
    datagram = sanitizeProtocolDatagram(std::move(datagram));
    QList<QByteArray> parts = datagram.split('|');
    if (parts.isEmpty()) {
        return {};
    }

    const QByteArray magic = parts[0];
    if (magic != QByteArray(kNatTraversalProtocol) && magic != QByteArray(kNatIndexProtocol)) {
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
    const QByteArray sanitized = sanitizeProtocolDatagram(datagram);
    qDebug() << "NAT traversal RX:" << sanitized.left(160);

    const QList<QByteArray> parts = splitTraversalParts(sanitized);
    if (parts.size() < 2) {
        return;
    }

    const QByteArray type = parts[1];

    if (type == "REGISTEROK" && parts.size() >= 3) {
        const QString code = normalizeTraversalCode(QString::fromUtf8(parts[2]));
        if (code.isEmpty()) {
            if (m_mode == Mode::Hosting) {
                failHosting("Invalid host code from NAT server");
            }
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

        if (m_mode == Mode::Hosting || m_mode == Mode::Idle) {
            m_mode = Mode::Hosting;
            emit hostRegistered(m_hostCode, publicAddress, signalingPort);
        }
        return;
    }

    if (parts[0] != QByteArray(kNatTraversalProtocol)) {
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
                m_nextHostRegisterMs = now + kHostRegisterIntervalMs;
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

void NatTraversalClient::queryStunServer(const QString& hostname, uint16_t port)
{
    // Resolve hostname (numeric or DNS)
    QHostAddress serverAddr;
    if (!serverAddr.setAddress(hostname)) {
        const QHostInfo info = QHostInfo::fromName(hostname);
        if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
            emit publicAddressFailed(QString("Failed to resolve STUN server %1: %2").arg(hostname, info.errorString()));
            return;
        }
        for (const QHostAddress& a : info.addresses()) {
            if (a.protocol() == QAbstractSocket::IPv4Protocol) {
                serverAddr = a;
                break;
            }
        }
        if (serverAddr.isNull()) {
            emit publicAddressFailed(QString("No IPv4 address for STUN server %1").arg(hostname));
            return;
        }
    }

    QUdpSocket* stunSocket = new QUdpSocket(this);
    if (!stunSocket->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        emit publicAddressFailed(QString("Failed to bind STUN socket: %1").arg(stunSocket->errorString()));
        stunSocket->deleteLater();
        return;
    }

    // Build STUN binding request (RFC5389)
    const quint32 magicCookie = 0x2112A442;
    QByteArray transactionId(12, 0);
    for (int i = 0; i < 12; ++i) {
        transactionId[i] = static_cast<char>(QRandomGenerator::global()->generate() & 0xFF);
    }

    QByteArray request;
    // Type: 0x0001 (Binding Request)
    request.append(char(0x00)); request.append(char(0x01));
    // Length: 0x0000 (no attributes)
    request.append(char(0x00)); request.append(char(0x00));
    // Magic cookie
    request.append(char((magicCookie >> 24) & 0xFF));
    request.append(char((magicCookie >> 16) & 0xFF));
    request.append(char((magicCookie >> 8) & 0xFF));
    request.append(char((magicCookie >> 0) & 0xFF));
    // Transaction ID
    request.append(transactionId);

    // Setup timeout
    QTimer* timeoutTimer = new QTimer(this);
    timeoutTimer->setSingleShot(true);
    timeoutTimer->setInterval(3000);

    connect(timeoutTimer, &QTimer::timeout, this, [stunSocket, timeoutTimer, this]() {
        stunSocket->close();
        stunSocket->deleteLater();
        timeoutTimer->deleteLater();
        emit publicAddressFailed("STUN request timed out");
    });

    connect(stunSocket, &QUdpSocket::readyRead, this, [stunSocket, transactionId, magicCookie, timeoutTimer, this]() mutable {
        while (stunSocket->hasPendingDatagrams()) {
            QNetworkDatagram dg = stunSocket->receiveDatagram();
            const QByteArray data = dg.data();
            if (data.size() < 20) {
                continue;
            }
            // Check transaction ID
            if (data.mid(8, 12) != transactionId) {
                continue;
            }
            // Message type 0x0101 (Binding Success Response)
            const quint16 msgType = (static_cast<unsigned char>(data[0]) << 8) | static_cast<unsigned char>(data[1]);
            if (msgType != 0x0101) {
                continue;
            }

            // Parse attributes starting at offset 20
            int offset = 20;
            while (offset + 4 <= data.size()) {
                const quint16 attrType = (static_cast<unsigned char>(data[offset]) << 8) | static_cast<unsigned char>(data[offset+1]);
                const quint16 attrLen = (static_cast<unsigned char>(data[offset+2]) << 8) | static_cast<unsigned char>(data[offset+3]);
                offset += 4;
                if (offset + attrLen > data.size()) break;

                if (attrType == 0x0020 && attrLen >= 8) { // XOR-MAPPED-ADDRESS
                    const unsigned char family = static_cast<unsigned char>(data[offset + 1]);
                    if (family == 0x01 && attrLen >= 8) { // IPv4
                        const quint16 xport = (static_cast<unsigned char>(data[offset+2]) << 8) | static_cast<unsigned char>(data[offset+3]);
                        const quint16 port = xport ^ static_cast<quint16>((magicCookie >> 16) & 0xFFFF);
                        const quint32 xaddr = (static_cast<unsigned char>(data[offset+4]) << 24) |
                                              (static_cast<unsigned char>(data[offset+5]) << 16) |
                                              (static_cast<unsigned char>(data[offset+6]) << 8) |
                                              (static_cast<unsigned char>(data[offset+7]));
                        const quint32 addr = xaddr ^ magicCookie;
                        QHostAddress mappedAddr;
                        mappedAddr.setAddress(QString::asprintf("%u.%u.%u.%u",
                                                                (addr >> 24) & 0xFF,
                                                                (addr >> 16) & 0xFF,
                                                                (addr >> 8) & 0xFF,
                                                                (addr >> 0) & 0xFF));
                        timeoutTimer->stop();
                        timeoutTimer->deleteLater();
                        stunSocket->close();
                        stunSocket->deleteLater();
                        emit publicAddressResolved(mappedAddr.toString(), static_cast<int>(port));
                        return;
                    }
                }

                // Advance (attributes are padded to 4-byte boundary)
                int padded = ((attrLen + 3) / 4) * 4;
                offset += padded;
            }
        }
    });

    // Send request
    stunSocket->writeDatagram(request, serverAddr, port);
    timeoutTimer->start();
}

} // namespace UserInterface::Netplay
