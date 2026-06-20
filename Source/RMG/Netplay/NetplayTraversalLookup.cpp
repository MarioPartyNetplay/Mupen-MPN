/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "NetplayTraversalLookup.hpp"
#include "NetplayProtocol.hpp"
#include "NetplayTraversalPunch.hpp"

#include <QEventLoop>
#include <QHostInfo>
#include <QNetworkDatagram>
#include <QTimer>
#include <QUdpSocket>

namespace UserInterface::Netplay {

namespace {

QList<QByteArray> splitRegistryParts(const QByteArray& datagram)
{
    return datagram.split('|');
}

bool resolveRegistryServer(QHostAddress* addressOut, QString* errorOut)
{
    const QString hostname = netplayIndexServerHostname();
    QHostAddress address;
    if (address.setAddress(hostname)) {
        *addressOut = address;
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
            *addressOut = resolved;
            return true;
        }
    }

    if (errorOut) {
        *errorOut = QStringLiteral("Traversal server %1 has no IPv4 address").arg(hostname);
    }
    return false;
}

} // namespace

TraversalLookupResult lookupTraversalHost(const QString& hostCode)
{
    TraversalLookupResult result;
    const QString normalized = normalizeTraversalCode(hostCode);
    if (normalized.isEmpty()) {
        result.error = QStringLiteral("Invalid traversal code");
        return result;
    }

    QUdpSocket socket;
    if (!socket.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::DefaultForPlatform)) {
        result.error = QStringLiteral("Failed to bind traversal lookup socket: %1")
                           .arg(socket.errorString());
        return result;
    }

    QHostAddress serverAddress;
    if (!resolveRegistryServer(&serverAddress, &result.error)) {
        return result;
    }

    const QByteArray message =
        QByteArray(kNetplayRegistryProtocol) + "|LOOKUP|" + normalized.toLatin1();
    if (socket.writeDatagram(message, serverAddress, kNetplayRegistryPort) < 0) {
        result.error = QStringLiteral("Failed to send traversal lookup: %1").arg(socket.errorString());
        return result;
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QUdpSocket::readyRead, &loop, [&]() {
        while (socket.hasPendingDatagrams()) {
            const QByteArray datagram = socket.receiveDatagram().data();
            if (handleTraversalPunchDatagram(datagram, &socket)) {
                continue;
            }

            const QList<QByteArray> parts = splitRegistryParts(datagram);
            if (parts.size() < 2 || parts.first() != kNetplayRegistryProtocol) {
                continue;
            }

            const QByteArray type = parts.at(1);
            if (type == "LOOKUPOK" && parts.size() >= 5) {
                const QString code = normalizeTraversalCode(QString::fromUtf8(parts.at(2)));
                if (code != normalized) {
                    continue;
                }

                const QString address = QString::fromUtf8(parts.at(3)).trimmed();
                const int port = QString::fromUtf8(parts.at(4)).toInt();
                if (!isUsableConnectAddress(address) || port < 1024 || port > 65535) {
                    result.error = QStringLiteral("Traversal server returned an invalid endpoint");
                    loop.quit();
                    return;
                }

                result.success = true;
                result.address = address;
                result.port = port;

                QHostAddress hostAddress;
                if (hostAddress.setAddress(address)) {
                    performTraversalPunchWindow(&socket, hostAddress, static_cast<quint16>(port));
                }

                loop.quit();
                return;
            }

            if (type == "LOOKUPFAIL" && parts.size() >= 3) {
                const QString code = normalizeTraversalCode(QString::fromUtf8(parts.at(2)));
                if (code == normalized) {
                    const QString reason = parts.size() >= 4
                                               ? QString::fromUtf8(parts.at(3))
                                               : QStringLiteral("NOTFOUND");
                    result.error = QStringLiteral("Traversal lookup failed: %1").arg(reason);
                    loop.quit();
                    return;
                }
            }
        }
    });

    timer.start(3000);
    loop.exec();

    if (!result.success && result.error.isEmpty()) {
        result.error = QStringLiteral("Traversal lookup timed out");
    }

    return result;
}

} // namespace UserInterface::Netplay
