/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "NetplayTraversalLookup.hpp"
#include "NetplayProtocol.hpp"
#include "NetplayTraversalPunch.hpp"
#include "NetplayEnet.hpp"

#include <QEventLoop>
#include <QHostInfo>
#include <QNetworkDatagram>
#include <QThread>
#include <QTimer>
#include <QElapsedTimer>
#include <QUdpSocket>

#include <enet/enet.h>

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

bool parseLookupOkReply(
    const QList<QByteArray>& parts,
    const QString& normalized,
    TraversalLookupResult* resultOut)
{
    if (parts.size() < 5 || parts.at(1) != "LOOKUPOK") {
        return false;
    }

    const QString code = normalizeTraversalCode(QString::fromUtf8(parts.at(2)));
    if (code != normalized) {
        return false;
    }

    const QString address = QString::fromUtf8(parts.at(3)).trimmed();
    const int port = QString::fromUtf8(parts.at(4)).toInt();
    if (!isUsableConnectAddress(address) || port < 1024 || port > 65535) {
        resultOut->error = QStringLiteral("Traversal server returned an invalid endpoint");
        return true;
    }

    resultOut->success = true;
    resultOut->address = address;
    resultOut->port = port;
    return true;
}

bool parseLookupFailReply(
    const QList<QByteArray>& parts,
    const QString& normalized,
    TraversalLookupResult* resultOut)
{
    if (parts.size() < 3 || parts.at(1) != "LOOKUPFAIL") {
        return false;
    }

    const QString code = normalizeTraversalCode(QString::fromUtf8(parts.at(2)));
    if (code != normalized) {
        return false;
    }

    const QString reason = parts.size() >= 4
                               ? QString::fromUtf8(parts.at(3))
                               : QStringLiteral("NOTFOUND");
    resultOut->error = QStringLiteral("Traversal lookup failed: %1").arg(reason);
    return true;
}

void handleLookupDatagram(
    const QByteArray& datagram,
    const QString& normalized,
    TraversalLookupResult* resultOut,
    bool* finishedOut)
{
    const QList<QByteArray> parts = splitRegistryParts(datagram);
    if (parts.size() < 2 || parts.first() != kNetplayRegistryProtocol) {
        return;
    }

    if (parseLookupOkReply(parts, normalized, resultOut)) {
        *finishedOut = true;
        return;
    }

    if (parseLookupFailReply(parts, normalized, resultOut)) {
        *finishedOut = true;
    }
}

struct ENetLookupState {
    QString expectedCode;
    TraversalLookupResult result;
    bool finished = false;
};

void enetLookupRegistryHandler(const QByteArray& datagram, void* userData)
{
    auto* state = static_cast<ENetLookupState*>(userData);
    handleLookupDatagram(datagram, state->expectedCode, &state->result, &state->finished);
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
    if (!socket.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
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

    bool finished = false;
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

            handleLookupDatagram(datagram, normalized, &result, &finished);
            if (finished) {
                loop.quit();
                return;
            }
        }
    });

    timer.start(8000);
    loop.exec();

    if (!result.success && result.error.isEmpty()) {
        result.error = QStringLiteral("Traversal lookup timed out");
    }

    if (result.success) {
        result.localUdpPort = static_cast<quint16>(socket.localPort());
    }

    return result;
}

TraversalLookupResult lookupTraversalHostViaEnet(ENetHost* host, const QString& hostCode)
{
    TraversalLookupResult result;
    const QString normalized = normalizeTraversalCode(hostCode);
    if (normalized.isEmpty()) {
        result.error = QStringLiteral("Invalid traversal code");
        return result;
    }

    if (!host) {
        result.error = QStringLiteral("Traversal lookup requires a bound ENet host");
        return result;
    }

    QHostAddress serverAddress;
    if (!resolveRegistryServer(&serverAddress, &result.error)) {
        return result;
    }

    ENetLookupState state;
    state.expectedCode = normalized;
    setEnetRegistryDatagramHandler(host, enetLookupRegistryHandler, &state);

    const QByteArray message =
        QByteArray(kNetplayRegistryProtocol) + "|LOOKUP|" + normalized.toLatin1();
    if (!sendEnetDatagram(host, serverAddress, kNetplayRegistryPort, message)) {
        clearEnetSideChannel(host);
        result.error = QStringLiteral("Failed to send traversal lookup");
        return result;
    }

    QElapsedTimer timer;
    timer.start();
    while (!state.finished && timer.elapsed() < 8000) {
        ENetEvent event;
        while (enet_host_service(host, &event, 0) > 0) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(event.packet);
            }
        }
        QThread::msleep(5);
    }

    clearEnetSideChannel(host);

    if (state.finished) {
        result = state.result;
    } else if (result.error.isEmpty()) {
        result.error = QStringLiteral("Traversal lookup timed out");
    }

    if (result.success) {
        result.localUdpPort = host->address.port;
        QHostAddress hostAddress;
        if (hostAddress.setAddress(result.address)) {
            performTraversalPunchWindowForEnet(host, hostAddress, static_cast<quint16>(result.port));
        }
    }

    return result;
}

} // namespace UserInterface::Netplay
