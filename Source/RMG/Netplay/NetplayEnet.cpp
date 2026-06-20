/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "NetplayEnet.hpp"
#include "NetplayTraversalPunch.hpp"
#include "NetplayProtocol.hpp"

#include <QJsonDocument>
#include <QAbstractSocket>
#include <atomic>
#include <cstring>
#include <unordered_map>

namespace UserInterface::Netplay {

namespace {

std::atomic<int> g_enetRefCount{0};

struct EnetSideChannelState {
    EnetRegistryDatagramHandler registryHandler = nullptr;
    void* registryUserData = nullptr;
};

std::unordered_map<ENetHost*, EnetSideChannelState>& enetSideChannels()
{
    static std::unordered_map<ENetHost*, EnetSideChannelState> channels;
    return channels;
}

EnetSideChannelState& sideChannelForHost(ENetHost* host)
{
    return enetSideChannels()[host];
}

void eraseEnetSideChannel(ENetHost* host)
{
    enetSideChannels().erase(host);
}

bool hostAddressToEnet(const QHostAddress& host, quint16 port, ENetAddress* addressOut)
{
    if (!addressOut || host.isNull() || port == 0) {
        return false;
    }

    addressOut->port = port;
    if (host.protocol() == QAbstractSocket::IPv4Protocol) {
        addressOut->host = host.toIPv4Address();
        return true;
    }

    const QByteArray hostBytes = host.toString().toUtf8();
    return enet_address_set_host(addressOut, hostBytes.constData()) == 0;
}

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

ENetHost* createSignalingEnetHost(const ENetAddress* address, size_t peerCount, size_t channelLimit,
                                  enet_uint32 incomingBandwidth, enet_uint32 outgoingBandwidth)
{
    if (peerCount > ENET_PROTOCOL_MAXIMUM_PEER_ID) {
        return nullptr;
    }

    ENetHost* host = static_cast<ENetHost*>(enet_malloc(sizeof(ENetHost)));
    if (!host) {
        return nullptr;
    }
    std::memset(host, 0, sizeof(ENetHost));

    host->peers = static_cast<ENetPeer*>(enet_malloc(peerCount * sizeof(ENetPeer)));
    if (!host->peers) {
        enet_free(host);
        return nullptr;
    }
    std::memset(host->peers, 0, peerCount * sizeof(ENetPeer));

    host->socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (host->socket == ENET_SOCKET_NULL) {
        enet_free(host->peers);
        enet_free(host);
        return nullptr;
    }

    enet_socket_set_option(host->socket, ENET_SOCKOPT_REUSEADDR, 1);

    if (address != nullptr && enet_socket_bind(host->socket, address) < 0) {
        enet_socket_destroy(host->socket);
        enet_free(host->peers);
        enet_free(host);
        return nullptr;
    }

    enet_socket_set_option(host->socket, ENET_SOCKOPT_NONBLOCK, 1);
    enet_socket_set_option(host->socket, ENET_SOCKOPT_BROADCAST, 1);
    enet_socket_set_option(host->socket, ENET_SOCKOPT_RCVBUF, ENET_HOST_RECEIVE_BUFFER_SIZE);
    enet_socket_set_option(host->socket, ENET_SOCKOPT_SNDBUF, ENET_HOST_SEND_BUFFER_SIZE);

    if (address != nullptr && enet_socket_get_address(host->socket, &host->address) < 0) {
        host->address = *address;
    }

    if (!channelLimit || channelLimit > ENET_PROTOCOL_MAXIMUM_CHANNEL_COUNT) {
        channelLimit = ENET_PROTOCOL_MAXIMUM_CHANNEL_COUNT;
    } else if (channelLimit < ENET_PROTOCOL_MINIMUM_CHANNEL_COUNT) {
        channelLimit = ENET_PROTOCOL_MINIMUM_CHANNEL_COUNT;
    }

    host->randomSeed = static_cast<enet_uint32>(reinterpret_cast<size_t>(host));
    host->randomSeed += enet_host_random_seed();
    host->randomSeed = (host->randomSeed << 16) | (host->randomSeed >> 16);
    host->channelLimit = channelLimit;
    host->incomingBandwidth = incomingBandwidth;
    host->outgoingBandwidth = outgoingBandwidth;
    host->bandwidthThrottleEpoch = 0;
    host->recalculateBandwidthLimits = 0;
    host->mtu = ENET_HOST_DEFAULT_MTU;
    host->peerCount = peerCount;
    host->commandCount = 0;
    host->bufferCount = 0;
    host->checksum = nullptr;
    host->receivedAddress.host = ENET_HOST_ANY;
    host->receivedAddress.port = 0;
    host->receivedData = nullptr;
    host->receivedDataLength = 0;
    host->totalSentData = 0;
    host->totalSentPackets = 0;
    host->totalReceivedData = 0;
    host->totalReceivedPackets = 0;
    host->totalQueued = 0;
    host->connectedPeers = 0;
    host->bandwidthLimitedPeers = 0;
    host->duplicatePeers = ENET_PROTOCOL_MAXIMUM_PEER_ID;
    host->maximumPacketSize = ENET_HOST_DEFAULT_MAXIMUM_PACKET_SIZE;
    host->maximumWaitingData = ENET_HOST_DEFAULT_MAXIMUM_WAITING_DATA;
    host->compressor.context = nullptr;
    host->compressor.compress = nullptr;
    host->compressor.decompress = nullptr;
    host->compressor.destroy = nullptr;
    host->intercept = nullptr;

    enet_list_clear(&host->dispatchQueue);

    for (ENetPeer* currentPeer = host->peers; currentPeer < &host->peers[host->peerCount]; ++currentPeer) {
        currentPeer->host = host;
        currentPeer->incomingPeerID = currentPeer - host->peers;
        currentPeer->outgoingSessionID = currentPeer->incomingSessionID = 0xFF;
        currentPeer->data = nullptr;
        enet_list_clear(&currentPeer->acknowledgements);
        enet_list_clear(&currentPeer->sentReliableCommands);
        enet_list_clear(&currentPeer->outgoingCommands);
        enet_list_clear(&currentPeer->outgoingSendReliableCommands);
        enet_list_clear(&currentPeer->dispatchedCommands);
        enet_peer_reset(currentPeer);
    }

    installNetplayEnetIntercept(host);
    return host;
}

int ENET_CALLBACK netplayEnetIntercept(ENetHost* host, ENetEvent* event)
{
    if (!host || !event) {
        return 0;
    }

    if (host->receivedDataLength == 1 && host->receivedData[0] == 0) {
        event->type = static_cast<ENetEventType>(kEnetSkippableEvent);
        return 1;
    }

    if (host->receivedDataLength >= 8 &&
        std::memcmp(host->receivedData, kNetplayRegistryProtocol, 8) == 0) {
        const QByteArray datagram(reinterpret_cast<const char*>(host->receivedData),
                                  static_cast<int>(host->receivedDataLength));
        handleTraversalPunchDatagram(datagram, host);

        const EnetSideChannelState& sideChannel = sideChannelForHost(host);
        if (sideChannel.registryHandler != nullptr) {
            sideChannel.registryHandler(datagram, sideChannel.registryUserData);
        }

        event->type = static_cast<ENetEventType>(kEnetSkippableEvent);
        return 1;
    }

    return 0;
}

void installNetplayEnetIntercept(ENetHost* host)
{
    if (host) {
        host->intercept = netplayEnetIntercept;
    }
}

void setEnetRegistryDatagramHandler(ENetHost* host, EnetRegistryDatagramHandler handler, void* userData)
{
    if (!host) {
        return;
    }

    EnetSideChannelState& sideChannel = sideChannelForHost(host);
    sideChannel.registryHandler = handler;
    sideChannel.registryUserData = userData;
}

void clearEnetSideChannel(ENetHost* host)
{
    if (!host) {
        return;
    }

    eraseEnetSideChannel(host);
}

bool sendEnetDatagram(ENetHost* host, const QHostAddress& target, quint16 port, const QByteArray& payload)
{
    if (!host || payload.isEmpty()) {
        return false;
    }

    ENetAddress address;
    if (!hostAddressToEnet(target, port, &address)) {
        return false;
    }

    ENetBuffer buffer;
    buffer.data = const_cast<char*>(payload.constData());
    buffer.dataLength = static_cast<size_t>(payload.size());
    return enet_socket_send(host->socket, &address, &buffer, 1) >= 0;
}

void sendEnetPunchBursts(ENetHost* host, const QHostAddress& target, quint16 port, int bursts)
{
    if (!host || target.isNull() || port == 0) {
        return;
    }

    static const QByteArray kPunchPayload = QByteArray(kNetplayRegistryProtocol) + "|PUNCHACK";
    ENetAddress address;
    if (!hostAddressToEnet(target, port, &address)) {
        return;
    }

    ENetBuffer buffer;
    buffer.data = const_cast<char*>(kPunchPayload.constData());
    buffer.dataLength = static_cast<size_t>(kPunchPayload.size());
    for (int i = 0; i < bursts; ++i) {
        enet_socket_send(host->socket, &address, &buffer, 1);
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
