/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#ifndef NETPLAYTRAVERSALPUNCH_HPP
#define NETPLAYTRAVERSALPUNCH_HPP

#include <QByteArray>
#include <QHostAddress>
#include <QUdpSocket>

struct _ENetHost;

namespace UserInterface::Netplay {

/** Handle N02TRAV1|PUNCH|ip|port from the traversal server; returns true if handled. */
bool handleTraversalPunchDatagram(const QByteArray& datagram, QUdpSocket* replySocket);
bool handleTraversalPunchDatagram(const QByteArray& datagram, _ENetHost* replyHost);

/** After LOOKUPOK, punch the host signaling endpoint and listen briefly for reciprocal punches. */
void performTraversalPunchWindow(QUdpSocket* socket, const QHostAddress& hostAddress, quint16 hostSignalingPort,
                                 int durationMs = 5000);

/** Same as performTraversalPunchWindow, but reuses the ENet signaling socket. */
void performTraversalPunchWindowForEnet(_ENetHost* host, const QHostAddress& hostAddress, quint16 hostSignalingPort,
                                        int durationMs = 5000);

} // namespace UserInterface::Netplay

#endif // NETPLAYTRAVERSALPUNCH_HPP
