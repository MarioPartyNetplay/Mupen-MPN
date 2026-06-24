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

/** Non-ENet UDP packets filtered by netplayEnetIntercept (Dolphin-style). */
constexpr int kEnetSkippableEvent = 42;

/** One-time ENet library init (reference counted). */
void ensureEnetInitialized();

/** Release ENet when the last netplay session ends. */
void shutdownEnetIfIdle();

/**
 * Like enet_host_create(), but sets SO_REUSEADDR before bind so the host registry
 * can share the signaling port when needed.
 */
ENetHost* createSignalingEnetHost(const ENetAddress* address, size_t peerCount, size_t channelLimit,
                                  enet_uint32 incomingBandwidth = 0, enet_uint32 outgoingBandwidth = 0);

void installNetplayEnetIntercept(ENetHost* host);

bool peerIsConnected(ENetPeer* peer);

/** Apply signaling peer timeouts tolerant of high-latency internet play. */
void applySignalingPeerTimeout(ENetPeer* peer);

/** Re-apply timeouts using the peer's current RTT (call after ping updates). */
void refreshSignalingPeerTimeout(ENetPeer* peer);

bool sendSignalingEvent(ENetPeer* peer, const QString& eventName, const QJsonObject& payload);
bool sendSignalingEvent(ENetPeer* peer, const QString& eventName, const QJsonArray& payload);

/** Parses a UDP signaling packet: [2, "event-name", payload...] */
bool parseSignalingPacket(const QByteArray& data, QString* eventNameOut, QJsonArray* argsOut);

bool sendEnetDatagram(ENetHost* host, const QHostAddress& target, quint16 port, const QByteArray& payload);

using EnetRegistryDatagramHandler = void (*)(const QByteArray& datagram, void* userData);
void setEnetRegistryDatagramHandler(ENetHost* host, EnetRegistryDatagramHandler handler, void* userData);
void clearEnetSideChannel(ENetHost* host);

} // namespace UserInterface::Netplay

#endif // NETPLAYENET_HPP
