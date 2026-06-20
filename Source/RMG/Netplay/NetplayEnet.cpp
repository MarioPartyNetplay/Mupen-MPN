/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "NetplayEnet.hpp"
#include "NetplayProtocol.hpp"

#include <QJsonDocument>
#include <atomic>

namespace UserInterface::Netplay {

namespace {

std::atomic<int> g_enetRefCount{0};

} // namespace

void ensureEnetInitialized()
{
    if (g_enetRefCount.fetch_add(1) == 0) {
        if (enet_initialize() != 0) {
            g_enetRefCount.fetch_sub(1);
        }
    }
}

void shutdownEnetIfIdle()
{
    const int remaining = g_enetRefCount.fetch_sub(1) - 1;
    if (remaining <= 0) {
        g_enetRefCount.store(0);
        enet_deinitialize();
    }
}

bool peerIsConnected(ENetPeer* peer)
{
    return peer != nullptr && peer->state == ENET_PEER_STATE_CONNECTED;
}

static bool sendSignalingPayload(ENetPeer* peer, const QJsonArray& message)
{
    if (!peerIsConnected(peer)) {
        return false;
    }

    const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    ENetPacket* packet = enet_packet_create(payload.constData(), static_cast<size_t>(payload.size()),
                                            ENET_PACKET_FLAG_RELIABLE);
    if (!packet) {
        return false;
    }

    return enet_peer_send(peer, 0, packet) == 0;
}

bool sendSignalingEvent(ENetPeer* peer, const QString& eventName, const QJsonObject& payload)
{
    QJsonArray message;
    message.append(2);
    message.append(eventName);
    message.append(payload);
    return sendSignalingPayload(peer, message);
}

bool sendSignalingEvent(ENetPeer* peer, const QString& eventName, const QJsonArray& payload)
{
    QJsonArray message;
    message.append(2);
    message.append(eventName);
    for (const QJsonValue& value : payload) {
        message.append(value);
    }
    return sendSignalingPayload(peer, message);
}

bool parseSignalingPacket(const QByteArray& data, QString* eventNameOut, QJsonArray* argsOut)
{
    if (!eventNameOut || !argsOut) {
        return false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isArray()) {
        return false;
    }

    const QJsonArray arr = doc.array();
    if (arr.isEmpty()) {
        return false;
    }

    int eventIndex = 0;
    if (arr[0].isDouble() && arr[0].toInt() == 2) {
        eventIndex = 1;
    }

    if (arr.size() <= eventIndex || !arr[eventIndex].isString()) {
        return false;
    }

    *eventNameOut = arr[eventIndex].toString();
    QJsonArray args;
    for (int i = eventIndex + 1; i < arr.size(); ++i) {
        args.append(arr[i]);
    }
    *argsOut = args;
    return true;
}

void sendUdpPunchBursts(QUdpSocket* socket, const QHostAddress& target, quint16 port, int bursts)
{
    if (!socket || target.isNull() || port == 0) {
        return;
    }

    static const QByteArray kPunchPayload = QByteArray(kNetplayRegistryProtocol) + "|PUNCHACK";
    for (int i = 0; i < bursts; ++i) {
        socket->writeDatagram(kPunchPayload, target, port);
    }
}

} // namespace UserInterface::Netplay
