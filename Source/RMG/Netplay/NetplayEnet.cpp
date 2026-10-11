/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "NetplayEnet.hpp"
#include "NetplayProtocol.hpp"

#include <QJsonDocument>
#include <QAbstractSocket>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QHostInfo>
#include <QList>
#include <algorithm>
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

void applyTunneledSignalingMtu(ENetHost* host)
{
    if (host && host->mtu > kTunneledSignalingMtu) {
        host->mtu = kTunneledSignalingMtu;
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

void sendNatPunch(ENetHost* host, const QHostAddress& target, quint16 port)
{
    if (!host || target.isNull() || port == 0) {
        return;
    }

    const QByteArray payload(1, '\0');
    for (int i = 0; i < 4; ++i) {
        sendEnetDatagram(host, target, port, payload);
    }
}

namespace {

struct PunchExchange {
    ENetHost* host = nullptr;
    QHostAddress peerAddress;
    quint16 peerPort = 0;
    bool ready = false;
    qint64 lastPunchMs = 0;
    QString error;
};

bool bindPunchSocket(ENetHost* host)
{
    if (!host || host->socket == ENET_SOCKET_NULL) {
        return false;
    }
    if (host->address.port != 0) {
        return true;
    }

    ENetAddress any;
    any.host = ENET_HOST_ANY;
    any.port = 0;
    if (enet_socket_bind(host->socket, &any) < 0) {
        return false;
    }
    enet_socket_get_address(host->socket, &host->address);
    return true;
}

bool resolveTraversalServer(QHostAddress* addressOut, QString* errorOut)
{
    if (!addressOut) {
        return false;
    }

    const QString hostname = netplayIndexServerHostname();
    QHostAddress literal;
    if (literal.setAddress(hostname) && literal.protocol() == QAbstractSocket::IPv4Protocol) {
        *addressOut = literal;
        return true;
    }

    const QHostInfo info = QHostInfo::fromName(hostname);
    if (info.error() != QHostInfo::NoError) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to resolve the NAT traversal server");
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
        *errorOut = QStringLiteral("NAT traversal server has no IPv4 address");
    }
    return false;
}

bool rememberPunchPeer(PunchExchange* state, const QHostAddress& address, quint16 port)
{
    if (!state || !state->host || address.isNull() || address.protocol() != QAbstractSocket::IPv4Protocol ||
        port == 0 || address == QHostAddress::AnyIPv4) {
        return false;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool same = state->ready && state->peerAddress == address && state->peerPort == port;
    state->peerAddress = address;
    state->peerPort = port;
    state->ready = true;
    state->error.clear();
    if (!same || now - state->lastPunchMs >= 50) {
        sendNatPunch(state->host, address, port);
        state->lastPunchMs = now;
    }
    return true;
}

void onPunchExchangeDatagram(const QByteArray& datagram, void* userData)
{
    auto* state = static_cast<PunchExchange*>(userData);
    const QList<QByteArray> parts = datagram.split('|');
    if (!state || parts.size() < 2 || parts.at(0) != QByteArray(kNetplayRegistryProtocol)) {
        return;
    }

    if (parts.at(1) == "LOOKUPOK" && parts.size() >= 5) {
        QHostAddress address;
        if (!address.setAddress(QString::fromUtf8(parts.at(3)).trimmed())) {
            return;
        }
        bool ok = false;
        const uint port = parts.at(4).trimmed().toUInt(&ok);
        if (!ok || port == 0 || port > 65535) {
            return;
        }
        rememberPunchPeer(state, address, static_cast<quint16>(port));
        return;
    }

    if (parts.at(1) == "PUNCH" && parts.size() >= 4) {
        QHostAddress address;
        if (!address.setAddress(QString::fromUtf8(parts.at(2)).trimmed())) {
            return;
        }
        bool ok = false;
        const uint port = parts.at(3).trimmed().toUInt(&ok);
        if (!ok || port == 0 || port > 65535) {
            return;
        }
        rememberPunchPeer(state, address, static_cast<quint16>(port));
        return;
    }

    if (parts.at(1) == "LOOKUPFAIL" && !state->ready) {
        const QByteArray reason = parts.size() >= 4 ? parts.at(3) : QByteArray();
        if (reason == "NOTFOUND") {
            state->error = QStringLiteral("Host is not registered for NAT traversal");
        } else {
            state->error = QStringLiteral("NAT traversal lookup failed");
        }
    }
}

} // namespace

bool exchangeTraversalPunch(ENetHost* host, const QString& hostCode, QString* addressOut, quint16* portOut,
                            QString* errorOut)
{
    if (errorOut) {
        errorOut->clear();
    }
    if (!host || !addressOut || !portOut) {
        if (errorOut) {
            *errorOut = QStringLiteral("NAT punch failed");
        }
        return false;
    }

    const QString code = normalizeTraversalCode(hostCode);
    if (code.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Invalid host code");
        }
        return false;
    }

    QString resolveError;
    QHostAddress serverAddress;
    if (!resolveTraversalServer(&serverAddress, &resolveError)) {
        if (errorOut) {
            *errorOut = resolveError;
        }
        return false;
    }

    if (!bindPunchSocket(host)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to bind the NAT punch socket");
        }
        return false;
    }

    PunchExchange state;
    state.host = host;
    setEnetRegistryDatagramHandler(host, &onPunchExchangeDatagram, &state);

    const QByteArray lookup =
        QByteArray(kNetplayRegistryProtocol) + "|LOOKUP|" + code.toUtf8();
    QElapsedTimer elapsed;
    elapsed.start();
    qint64 nextLookupAt = 0;
    while (elapsed.elapsed() < 1200 && !state.ready && state.error.isEmpty()) {
        if (elapsed.elapsed() >= nextLookupAt) {
            sendEnetDatagram(host, serverAddress, static_cast<quint16>(kNetplayRegistryPort), lookup);
            nextLookupAt = elapsed.elapsed() + 400;
        }
        ENetEvent event;
        enet_host_service(host, &event, 20);
    }

    if (state.ready) {
        sendNatPunch(host, state.peerAddress, state.peerPort);
        sendEnetDatagram(host, serverAddress, static_cast<quint16>(kNetplayRegistryPort), lookup);
    }

    clearEnetSideChannel(host);

    if (!state.ready) {
        if (errorOut) {
            *errorOut = state.error.isEmpty() ? QStringLiteral("NAT traversal server did not answer")
                                              : state.error;
        }
        return false;
    }

    *addressOut = state.peerAddress.toString();
    *portOut = state.peerPort;
    qInfo() << "NAT punch opened toward" << *addressOut << *portOut << "for" << code;
    return true;
}

bool peerIsConnected(ENetPeer* peer)
{
    return peer != nullptr && peer->state == ENET_PEER_STATE_CONNECTED;
}

namespace {

constexpr enet_uint32 kSignalingTimeoutMinimumMs = 15000;
constexpr enet_uint32 kSignalingTimeoutMaximumMs = 120000;

} // namespace

void applySignalingPeerTimeout(ENetPeer* peer)
{
    refreshSignalingPeerTimeout(peer);
}

void refreshSignalingPeerTimeout(ENetPeer* peer)
{
    if (!peer) {
        return;
    }

    // Scale the minimum timeout with observed RTT so high-ping links are not
    // dropped during brief packet loss bursts.
    enet_uint32 minimum = kSignalingTimeoutMinimumMs;
    if (peer->roundTripTime > 0) {
        const enet_uint32 rttScaled =
            static_cast<enet_uint32>(peer->roundTripTime) * 40u;
        minimum = std::max(minimum, rttScaled);
    }
    if (minimum > kSignalingTimeoutMaximumMs / 2u) {
        minimum = kSignalingTimeoutMaximumMs / 2u;
    }

    enet_peer_timeout(
        peer,
        ENET_PEER_TIMEOUT_LIMIT,
        minimum,
        kSignalingTimeoutMaximumMs);
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

bool sendGameplaySignalingEvent(ENetPeer* peer, const QString& eventName, const QJsonObject& payload)
{
    if (!peerIsConnected(peer)) {
        return false;
    }

    QJsonArray message;
    message.append(2);
    message.append(eventName);
    message.append(payload);

    const QByteArray encoded =
        QJsonDocument(message).toJson(QJsonDocument::Compact);
    ENetPacket* packet = enet_packet_create(
        encoded.constData(),
        static_cast<size_t>(encoded.size()),
        ENET_PACKET_FLAG_UNSEQUENCED);
    if (!packet) {
        return false;
    }

    return enet_peer_send(peer, 0, packet) == 0;
}

namespace {
constexpr char kControllerInputMagic = static_cast<char>(0xC1);
constexpr int kControllerInputPacketSize = 10;
}

bool sendGameplayControllerInput(
    ENetPeer* peer,
    int slot,
    uint32_t frameNumber,
    uint32_t controllerState)
{
    if (!peerIsConnected(peer)) {
        return false;
    }

    char encoded[kControllerInputPacketSize];
    encoded[0] = kControllerInputMagic;
    encoded[1] = static_cast<char>(slot < 0 || slot > 254 ? 0xFF : slot);
    std::memcpy(encoded + 2, &frameNumber, 4);
    std::memcpy(encoded + 6, &controllerState, 4);

    ENetPacket* packet = enet_packet_create(
        encoded,
        static_cast<size_t>(kControllerInputPacketSize),
        ENET_PACKET_FLAG_RELIABLE);
    if (!packet) {
        return false;
    }

    return enet_peer_send(peer, 0, packet) == 0;
}

bool parseGameplayControllerInput(
    const QByteArray& data,
    int* slotOut,
    uint32_t* frameOut,
    uint32_t* inputOut)
{
    if (data.size() != kControllerInputPacketSize ||
        data[0] != kControllerInputMagic ||
        !slotOut ||
        !frameOut ||
        !inputOut) {
        return false;
    }

    const auto slotByte = static_cast<unsigned char>(data[1]);
    *slotOut = (slotByte == 0xFF) ? -1 : static_cast<int>(slotByte);
    std::memcpy(frameOut, data.constData() + 2, 4);
    std::memcpy(inputOut, data.constData() + 6, 4);
    return true;
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

} // namespace UserInterface::Netplay
