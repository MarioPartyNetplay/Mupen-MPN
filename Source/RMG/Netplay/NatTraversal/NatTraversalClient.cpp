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

#include <iterator>

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

namespace {

constexpr quint32 kStunMagicCookie = 0x2112A442;
constexpr quint16 kStunAttrMappedAddress = 0x0001;
constexpr quint16 kStunAttrXorMappedAddress = 0x0020;

struct StunServerEndpoint {
    const char* hostname;
    quint16 port;
};

constexpr StunServerEndpoint kDefaultStunServers[] = {
    {"stun.l.google.com", 19302},
    {"stun1.l.google.com", 19302},
    {"stun.cloudflare.com", 3478},
};

bool parseStunIpv4Endpoint(const QByteArray& data, int valueOffset, quint16 attrLen, quint32 magicCookie,
                           bool xorEncoded, QString* addressOut, int* portOut)
{
    if (attrLen < 8 || valueOffset + 8 > data.size()) {
        return false;
    }

    const unsigned char family = static_cast<unsigned char>(data[valueOffset + 1]);
    if (family != 0x01) {
        return false;
    }

    const quint16 rawPort = (static_cast<unsigned char>(data[valueOffset + 2]) << 8) |
                            static_cast<unsigned char>(data[valueOffset + 3]);
    const quint32 rawAddr = (static_cast<unsigned char>(data[valueOffset + 4]) << 24) |
                            (static_cast<unsigned char>(data[valueOffset + 5]) << 16) |
                            (static_cast<unsigned char>(data[valueOffset + 6]) << 8) |
                            static_cast<unsigned char>(data[valueOffset + 7]);

    const quint16 port = xorEncoded ? static_cast<quint16>(rawPort ^ ((magicCookie >> 16) & 0xFFFF)) : rawPort;
    const quint32 addr = xorEncoded ? (rawAddr ^ magicCookie) : rawAddr;

    if (addressOut) {
        *addressOut = QString::asprintf("%u.%u.%u.%u",
                                        (addr >> 24) & 0xFF,
                                        (addr >> 16) & 0xFF,
                                        (addr >> 8) & 0xFF,
                                        addr & 0xFF);
    }
    if (portOut) {
        *portOut = static_cast<int>(port);
    }
    return true;
}

bool parseStunBindingSuccess(const QByteArray& data, const QByteArray& transactionId, QString* addressOut, int* portOut)
{
    if (data.size() < 20 || data.mid(8, 12) != transactionId) {
        return false;
    }

    const quint16 msgType = (static_cast<unsigned char>(data[0]) << 8) | static_cast<unsigned char>(data[1]);
    if (msgType != 0x0101) {
        return false;
    }

    const quint16 msgLen = (static_cast<unsigned char>(data[2]) << 8) | static_cast<unsigned char>(data[3]);
    if (data.size() < 20 + msgLen) {
        return false;
    }

    int offset = 20;
    QString mappedAddress;
    int mappedPort = 0;
    bool found = false;

    while (offset + 4 <= 20 + msgLen) {
        const quint16 attrType = (static_cast<unsigned char>(data[offset]) << 8) |
                                 static_cast<unsigned char>(data[offset + 1]);
        const quint16 attrLen = (static_cast<unsigned char>(data[offset + 2]) << 8) |
                                static_cast<unsigned char>(data[offset + 3]);
        offset += 4;
        if (offset + attrLen > data.size()) {
            break;
        }

        QString address;
        int port = 0;
        if (attrType == kStunAttrXorMappedAddress &&
            parseStunIpv4Endpoint(data, offset, attrLen, kStunMagicCookie, true, &address, &port)) {
            mappedAddress = address;
            mappedPort = port;
            found = true;
        } else if (attrType == kStunAttrMappedAddress &&
                   parseStunIpv4Endpoint(data, offset, attrLen, kStunMagicCookie, false, &address, &port)) {
            mappedAddress = address;
            mappedPort = port;
            found = true;
        }

        const int padded = ((attrLen + 3) / 4) * 4;
        offset += padded;
    }

    if (!found) {
        return false;
    }

    if (addressOut) {
        *addressOut = mappedAddress;
    }
    if (portOut) {
        *portOut = mappedPort;
    }
    return true;
}

} // namespace

void NatTraversalClient::queryStunServer(const QString& hostname, uint16_t port)
{
    QString normalizedHost = hostname.trimmed();
    quint16 normalizedPort = port;

    if (normalizedHost.startsWith("stun://", Qt::CaseInsensitive)) {
        normalizedHost = normalizedHost.mid(7);
    } else if (normalizedHost.startsWith("stuns://", Qt::CaseInsensitive)) {
        normalizedHost = normalizedHost.mid(8);
        if (normalizedPort == 19302) {
            normalizedPort = 5349;
        }
    } else if (normalizedHost.startsWith("stun:", Qt::CaseInsensitive)) {
        normalizedHost = normalizedHost.mid(5);
    } else if (normalizedHost.startsWith("stuns:", Qt::CaseInsensitive)) {
        normalizedHost = normalizedHost.mid(6);
        if (normalizedPort == 19302) {
            normalizedPort = 5349;
        }
    }

    const int slashIndex = normalizedHost.indexOf('/');
    if (slashIndex != -1) {
        normalizedHost = normalizedHost.left(slashIndex);
    }

    if (normalizedHost.startsWith('[') && normalizedHost.contains(']')) {
        const int closeIndex = normalizedHost.indexOf(']');
        const QString hostPart = normalizedHost.mid(1, closeIndex - 1).trimmed();
        const QString portPart = normalizedHost.mid(closeIndex + 1).trimmed();
        if (portPart.startsWith(':')) {
            bool ok = false;
            const int parsedPort = portPart.mid(1).toInt(&ok);
            if (ok && parsedPort >= 1 && parsedPort <= 65535) {
                normalizedPort = static_cast<quint16>(parsedPort);
            }
        }
        normalizedHost = hostPart;
    } else {
        const int colonIndex = normalizedHost.lastIndexOf(':');
        if (colonIndex > 0 && normalizedHost.indexOf(':') == colonIndex) {
            bool ok = false;
            const int parsedPort = normalizedHost.mid(colonIndex + 1).toInt(&ok);
            if (ok && parsedPort >= 1 && parsedPort <= 65535) {
                normalizedPort = static_cast<quint16>(parsedPort);
                normalizedHost = normalizedHost.left(colonIndex);
            }
        }
    }

    m_stunQueryPrimaryHost = normalizedHost;
    m_stunQueryPrimaryPort = normalizedPort;
    m_stunQueryIndex = 0;
    tryStunServer(0);
}

void NatTraversalClient::tryStunServer(int serverIndex)
{
    QString hostname;
    quint16 port = m_stunQueryPrimaryPort;

    if (serverIndex == 0 && !m_stunQueryPrimaryHost.isEmpty()) {
        hostname = m_stunQueryPrimaryHost;
    } else {
        const int fallbackIndex = serverIndex - (m_stunQueryPrimaryHost.isEmpty() ? 0 : 1);
        if (fallbackIndex < 0 || fallbackIndex >= static_cast<int>(std::size(kDefaultStunServers))) {
            emit publicAddressFailed(QStringLiteral("STUN discovery failed on all servers"));
            m_stunQueryIndex = -1;
            return;
        }
        hostname = QString::fromLatin1(kDefaultStunServers[fallbackIndex].hostname);
        port = kDefaultStunServers[fallbackIndex].port;
    }

    m_stunQueryIndex = serverIndex;

    QHostAddress serverAddr;
    if (serverAddr.setAddress(hostname)) {
        sendStunBindingRequest(serverAddr, port);
        return;
    }

    QHostInfo::lookupHost(hostname, this, [this, hostname, port, serverIndex](const QHostInfo& info) {
        if (m_stunQueryIndex != serverIndex) {
            return;
        }

        if (info.error() != QHostInfo::NoError) {
            qWarning() << "STUN DNS lookup failed for" << hostname << ":" << info.errorString();
            tryStunServer(serverIndex + 1);
            return;
        }

        QHostAddress resolved;
        for (const QHostAddress& candidate : info.addresses()) {
            if (candidate.protocol() == QAbstractSocket::IPv4Protocol) {
                resolved = candidate;
                break;
            }
        }

        if (resolved.isNull()) {
            qWarning() << "STUN DNS lookup returned no IPv4 address for" << hostname;
            tryStunServer(serverIndex + 1);
            return;
        }

        sendStunBindingRequest(resolved, port);
    });
}

void NatTraversalClient::sendStunBindingRequest(const QHostAddress& serverAddr, quint16 port)
{
    const int serverIndex = m_stunQueryIndex;

    QUdpSocket* stunSocket = new QUdpSocket(this);
    if (!stunSocket->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        qWarning() << "Failed to bind STUN socket:" << stunSocket->errorString();
        stunSocket->deleteLater();
        tryStunServer(serverIndex + 1);
        return;
    }

    QByteArray transactionId(12, 0);
    for (int i = 0; i < 12; ++i) {
        transactionId[i] = static_cast<char>(QRandomGenerator::global()->generate() & 0xFF);
    }

    QByteArray request;
    request.append(char(0x00));
    request.append(char(0x01));
    request.append(char(0x00));
    request.append(char(0x00));
    request.append(char((kStunMagicCookie >> 24) & 0xFF));
    request.append(char((kStunMagicCookie >> 16) & 0xFF));
    request.append(char((kStunMagicCookie >> 8) & 0xFF));
    request.append(char((kStunMagicCookie >> 0) & 0xFF));
    request.append(transactionId);

    QTimer* timeoutTimer = new QTimer(this);
    timeoutTimer->setSingleShot(true);
    timeoutTimer->setInterval(4000);

    auto failAndContinue = [this, stunSocket, timeoutTimer, serverIndex](const QString& reason) {
        if (m_stunQueryIndex != serverIndex) {
            stunSocket->deleteLater();
            timeoutTimer->deleteLater();
            return;
        }
        qWarning() << reason;
        stunSocket->close();
        stunSocket->deleteLater();
        timeoutTimer->deleteLater();
        tryStunServer(serverIndex + 1);
    };

    connect(timeoutTimer, &QTimer::timeout, this, [failAndContinue]() {
        failAndContinue(QStringLiteral("STUN request timed out"));
    });

    connect(stunSocket, &QUdpSocket::readyRead, this,
            [this, stunSocket, transactionId, timeoutTimer, serverIndex, failAndContinue]() {
                while (stunSocket->hasPendingDatagrams()) {
                    const QByteArray data = stunSocket->receiveDatagram().data();
                    QString address;
                    int mappedPort = 0;
                    if (!parseStunBindingSuccess(data, transactionId, &address, &mappedPort)) {
                        continue;
                    }

                    if (m_stunQueryIndex != serverIndex) {
                        return;
                    }

                    timeoutTimer->stop();
                    timeoutTimer->deleteLater();
                    stunSocket->close();
                    stunSocket->deleteLater();
                    m_stunQueryIndex = -1;
                    qDebug() << "STUN mapped endpoint" << address << mappedPort;
                    emit publicAddressResolved(address, mappedPort);
                    return;
                }
            });

    if (stunSocket->writeDatagram(request, serverAddr, port) < 0) {
        failAndContinue(QString("Failed to send STUN request: %1").arg(stunSocket->errorString()));
        return;
    }

    timeoutTimer->start();
}

} // namespace UserInterface::Netplay
