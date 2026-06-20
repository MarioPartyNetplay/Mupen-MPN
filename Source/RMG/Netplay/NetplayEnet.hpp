/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#ifndef NETPLAYENET_HPP
#define NETPLAYENET_HPP

#include <enet/enet.h>

#include <QByteArray>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QUdpSocket>

namespace UserInterface::Netplay {

/** One-time ENet library init (reference counted). */
void ensureEnetInitialized();

/** Release ENet when the last netplay session ends. */
void shutdownEnetIfIdle();

bool peerIsConnected(ENetPeer* peer);

bool sendSignalingEvent(ENetPeer* peer, const QString& eventName, const QJsonObject& payload);
bool sendSignalingEvent(ENetPeer* peer, const QString& eventName, const QJsonArray& payload);

/** Parses a UDP signaling packet: [2, "event-name", payload...] */
bool parseSignalingPacket(const QByteArray& data, QString* eventNameOut, QJsonArray* argsOut);

/** Send repeated UDP datagrams to punch through cone NAT (Dolphin-style). */
void sendUdpPunchBursts(QUdpSocket* socket, const QHostAddress& target, quint16 port, int bursts = 10);

} // namespace UserInterface::Netplay

#endif // NETPLAYENET_HPP
