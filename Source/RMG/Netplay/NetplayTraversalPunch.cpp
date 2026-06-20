/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "NetplayTraversalPunch.hpp"

#include "NetplayEnet.hpp"
#include "NetplayProtocol.hpp"

#include <QDateTime>
#include <QEventLoop>
#include <QNetworkDatagram>
#include <QTimer>

namespace UserInterface::Netplay {

namespace {

bool parsePunchTarget(const QList<QByteArray>& parts, QHostAddress* addressOut, quint16* portOut)
{
    if (parts.size() < 4) {
        return false;
    }

    const QString ipText = QString::fromUtf8(parts.at(2)).trimmed();
    const int port = QString::fromUtf8(parts.at(3)).toInt();
    if (port < 1 || port > 65535) {
        return false;
    }

    QHostAddress address;
    if (!address.setAddress(ipText)) {
        return false;
    }

    *addressOut = address;
    *portOut = static_cast<quint16>(port);
    return true;
}

} // namespace

bool handleTraversalPunchDatagram(const QByteArray& datagram, QUdpSocket* replySocket)
{
    if (!replySocket || datagram.size() < 8) {
        return false;
    }

    if (datagram.left(8) != QByteArray(kNetplayRegistryProtocol)) {
        return false;
    }

    const QList<QByteArray> parts = datagram.split('|');
    if (parts.size() < 2 || parts.at(1) != "PUNCH") {
        return false;
    }

    QHostAddress targetAddress;
    quint16 targetPort = 0;
    if (!parsePunchTarget(parts, &targetAddress, &targetPort)) {
        return false;
    }

    sendUdpPunchBursts(replySocket, targetAddress, targetPort);
    return true;
}

void performTraversalPunchWindow(QUdpSocket* socket, const QHostAddress& hostAddress, quint16 hostSignalingPort,
                                 int durationMs)
{
    if (!socket || hostAddress.isNull() || hostSignalingPort == 0) {
        return;
    }

    sendUdpPunchBursts(socket, hostAddress, hostSignalingPort);

    QEventLoop loop;
    QTimer timer;
    timer.setInterval(50);
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + durationMs;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        while (socket->hasPendingDatagrams()) {
            const QByteArray datagram = socket->receiveDatagram().data();
            handleTraversalPunchDatagram(datagram, socket);
        }

        if (QDateTime::currentMSecsSinceEpoch() >= deadline) {
            loop.quit();
            return;
        }

        sendUdpPunchBursts(socket, hostAddress, hostSignalingPort, 2);
    });
    timer.start();
    loop.exec();
}

} // namespace UserInterface::Netplay
