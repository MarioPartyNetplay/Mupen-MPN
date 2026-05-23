/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "NatTraversalIndexClient.hpp"

#include <QDebug>
#include <QHostInfo>
#include <QNetworkDatagram>

namespace UserInterface::Netplay {

namespace {

constexpr int kRequestTimeoutMs = 5000;

QByteArray payloadAfterField(const QByteArray& datagram, const QByteArray& prefix)
{
    if (!datagram.startsWith(prefix)) {
        return {};
    }

    QByteArray payload = datagram.mid(prefix.size());
    if (payload.startsWith("B64:")) {
        payload = QByteArray::fromBase64(payload.mid(4));
    }
    return payload;
}

} // namespace

NatTraversalIndexClient::NatTraversalIndexClient(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QUdpSocket::readyRead, this, &NatTraversalIndexClient::onReadyRead);
    connect(&m_timeoutTimer, &QTimer::timeout, this, &NatTraversalIndexClient::onTimeout);
    m_timeoutTimer.setSingleShot(true);
}

void NatTraversalIndexClient::publish(const QString& key, const QByteArray& data)
{
    cancel();

    if (!isValidIndexKey(key)) {
        emit publishFailed("Invalid index key");
        return;
    }

    m_pendingOp = PendingOp::Publish;
    m_pendingKey = key.trimmed();

    const QByteArray message = QByteArray(kNatIndexProtocol) + "|SET|" + m_pendingKey.toUtf8() + "|B64:" +
                               data.toBase64(QByteArray::Base64Encoding);
    sendMessage(message);
    m_timeoutTimer.start(kRequestTimeoutMs);
}

void NatTraversalIndexClient::fetch(const QString& key)
{
    cancel();

    if (!isValidIndexKey(key)) {
        emit fetchFailed("Invalid index key");
        return;
    }

    m_pendingOp = PendingOp::Fetch;
    m_pendingKey = key.trimmed();

    const QByteArray message = QByteArray(kNatIndexProtocol) + "|GET|" + m_pendingKey.toUtf8();
    sendMessage(message);
    m_timeoutTimer.start(kRequestTimeoutMs);
}

void NatTraversalIndexClient::fetchSession(const QString& hostCode)
{
    const QString key = sessionIndexKey(hostCode);
    if (key.isEmpty()) {
        emit fetchFailed("Invalid host code");
        return;
    }
    fetch(key);
}

void NatTraversalIndexClient::publishSession(const QString& hostCode, const QByteArray& data)
{
    const QString key = sessionIndexKey(hostCode);
    if (key.isEmpty()) {
        emit publishFailed("Invalid host code");
        return;
    }
    publish(key, data);
}

void NatTraversalIndexClient::cancel()
{
    m_timeoutTimer.stop();
    m_pendingOp = PendingOp::None;
    m_pendingKey.clear();
}

bool NatTraversalIndexClient::ensureReady(QString* errorOut)
{
    if (m_socket.state() != QAbstractSocket::BoundState) {
        if (!m_socket.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::DefaultForPlatform)) {
            if (errorOut) {
                *errorOut = m_socket.errorString();
            }
            return false;
        }
    }

    if (!m_serverAddress.isNull()) {
        return true;
    }

    const QString hostname = natTraversalServerHostname();
    QHostAddress address;
    if (address.setAddress(hostname)) {
        m_serverAddress = address;
        qDebug() << "NatIndex: resolved" << hostname << "->" << m_serverAddress.toString() << ":" << kNatTraversalPort;
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

void NatTraversalIndexClient::sendMessage(const QByteArray& message)
{
    QString error;
    if (!ensureReady(&error)) {
        fail(error);
        return;
    }

    qDebug() << "NatIndex: sending" << message.size() << "bytes to" << m_serverAddress.toString() << ":" << kNatTraversalPort
             << "data=" << message.toHex().toUpper();

    qint64 written = m_socket.writeDatagram(message, m_serverAddress, kNatTraversalPort);
    if (written < 0) {
        fail(m_socket.errorString());
    } else if (written != message.size()) {
        qWarning() << "NatIndex: partial write" << written << "of" << message.size();
    }
}

void NatTraversalIndexClient::onReadyRead()
{
    while (m_socket.hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_socket.receiveDatagram();
        handleResponse(datagram.data());
    }
}

void NatTraversalIndexClient::handleResponse(const QByteArray& datagram)
{
    QByteArray sanitized = datagram;
    sanitized.replace('\0', QByteArray());

    const QList<QByteArray> parts = sanitized.split('|');
    if (parts.size() < 2 || parts[0] != QByteArray(kNatIndexProtocol)) {
        return;
    }

    const QByteArray type = parts[1];

    if (type == "SETOK" && parts.size() >= 3) {
        if (m_pendingOp != PendingOp::Publish || QString::fromUtf8(parts[2]) != m_pendingKey) {
            return;
        }

        const QString key = m_pendingKey;
        cancel();
        emit published(key);
        return;
    }

    if (type == "GETOK" && parts.size() >= 3) {
        if (m_pendingOp != PendingOp::Fetch || QString::fromUtf8(parts[2]) != m_pendingKey) {
            return;
        }

        const QByteArray prefix = QByteArray(kNatIndexProtocol) + "|GETOK|" + m_pendingKey.toUtf8() + "|";
        const QByteArray payload = payloadAfterField(datagram, prefix);
        const QString key = m_pendingKey;
        cancel();
        emit fetched(key, payload);
        return;
    }

    if (type == "GETFAIL" && parts.size() >= 4) {
        if (m_pendingOp != PendingOp::Fetch || QString::fromUtf8(parts[2]) != m_pendingKey) {
            return;
        }

        const QString reason = QString::fromUtf8(parts[3]);
        fail(reason == "NOTFOUND" ? "Index key not found" : reason);
        return;
    }

    if (type == "ERR" && parts.size() >= 3) {
        fail(QString::fromUtf8(parts[2]));
    }
}

void NatTraversalIndexClient::onTimeout()
{
    fail("Index request timed out");
}

void NatTraversalIndexClient::fail(const QString& reason)
{
    if (m_pendingOp == PendingOp::Publish) {
        cancel();
        emit publishFailed(reason);
        return;
    }

    if (m_pendingOp == PendingOp::Fetch) {
        cancel();
        emit fetchFailed(reason);
    }
}

} // namespace UserInterface::Netplay
