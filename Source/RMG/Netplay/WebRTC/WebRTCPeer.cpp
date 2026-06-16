/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "WebRTCPeer.hpp"
#include <RMG-Core/Netplay/WebRTC/WebRTCDataChannel.hpp>
#include "../NatTraversal/NatTraversalProtocol.hpp"

#include <rtc/rtc.hpp>

#include <QDebug>
#include <QMap>

#include <sstream>

using namespace UserInterface::Netplay;

namespace {

std::vector<rtc::IceServer> buildIceServers()
{
    std::vector<rtc::IceServer> servers;

    const QString stunHost = stunServerHost().trimmed();
    const quint16 stunPort = stunServerPort();
    if (!stunHost.isEmpty()) {
        const std::string endpoint = stunPort > 0
            ? QStringLiteral("%1:%2").arg(stunHost).arg(stunPort).toStdString()
            : stunHost.toStdString();
        servers.emplace_back(endpoint);
    }

    return servers;
}

int candidateMidToLineIndex(const rtc::Candidate& candidate)
{
    const QString mid = QString::fromStdString(candidate.mid());
    bool ok = false;
    const int parsed = mid.toInt(&ok);
    return ok ? parsed : 0;
}

template <typename T>
QString enumToString(T value)
{
    std::ostringstream stream;
    stream << value;
    return QString::fromStdString(stream.str());
}

} // namespace

WebRTCPeer::WebRTCPeer(const QString& peerId, bool initiator, QObject* parent)
    : QObject(parent)
    , m_peerId(peerId)
    , m_initiator(initiator)
    , m_connectionState(New)
{
    qDebug() << "WebRTCPeer created:" << peerId << "initiator=" << initiator;
    initializePeerConnection();
}

WebRTCPeer::~WebRTCPeer()
{
    close();
}

void WebRTCPeer::createOffer()
{
    if (!m_initiator) {
        qWarning() << "WebRTCPeer: Cannot create offer - not initiator";
        return;
    }

    if (!m_peerConnection) {
        m_lastError = QStringLiteral("Peer connection not initialized");
        emit connectionFailed(m_lastError);
        return;
    }

    qDebug() << "WebRTCPeer: Creating offer for" << m_peerId;

    try {
        m_peerConnection->setLocalDescription(rtc::Description::Type::Offer);
        m_localDescriptionSent = true;
    } catch (const std::exception& exception) {
        m_lastError = QString::fromStdString(exception.what());
        emit connectionFailed(m_lastError);
    }
}

void WebRTCPeer::setRemoteDescription(const QString& sdpAnswer)
{
    if (!m_peerConnection) {
        m_lastError = QStringLiteral("Peer connection not initialized");
        emit connectionFailed(m_lastError);
        return;
    }

    qDebug() << "WebRTCPeer: Setting remote description for" << m_peerId;

    try {
        m_peerConnection->setRemoteDescription(rtc::Description(sdpAnswer.toStdString()));

        if (!m_initiator && !m_localDescriptionSent) {
            m_peerConnection->setLocalDescription();
            m_localDescriptionSent = true;
        }
    } catch (const std::exception& exception) {
        m_lastError = QString::fromStdString(exception.what());
        emit connectionFailed(m_lastError);
    }
}

void WebRTCPeer::addICECandidate(const QString& candidate, int sdpMLineIndex)
{
    if (!m_peerConnection) {
        return;
    }

    qDebug() << "WebRTCPeer: Adding ICE candidate for" << m_peerId;

    try {
        m_peerConnection->addRemoteCandidate(
            rtc::Candidate(candidate.toStdString(), std::to_string(sdpMLineIndex)));
    } catch (const std::exception& exception) {
        m_lastError = QString::fromStdString(exception.what());
        emit connectionFailed(m_lastError);
    }
}

void WebRTCPeer::close()
{
    qDebug() << "WebRTCPeer: Closing connection with" << m_peerId;

    if (m_peerConnection) {
        m_peerConnection->close();
        m_peerConnection.reset();
    }

    updateConnectionState(Closed);
    emit connectionClosed();
}

std::shared_ptr<WebRTCDataChannel> WebRTCPeer::createDataChannel(const QString& label)
{
    qDebug() << "WebRTCPeer: Creating data channel" << label << "for" << m_peerId;

    if (!m_peerConnection) {
        m_lastError = QStringLiteral("Peer connection not initialized");
        emit connectionFailed(m_lastError);
        return nullptr;
    }

    auto backendChannel = m_peerConnection->createDataChannel(label.toStdString());
    registerDataChannel(backendChannel);
    auto channel = m_dataChannels.value(label);
    return channel;
}

std::shared_ptr<WebRTCDataChannel> WebRTCPeer::getDataChannel(const QString& label)
{
    if (m_dataChannels.contains(label)) {
        return m_dataChannels.value(label);
    }

    return nullptr;
}

QString WebRTCPeer::getPeerId() const
{
    return m_peerId;
}

WebRTCPeer::ConnectionState WebRTCPeer::getConnectionState() const
{
    return m_connectionState;
}

QString WebRTCPeer::getConnectionStateString() const
{
    switch (m_connectionState) {
    case New: return QStringLiteral("new");
    case Connecting: return QStringLiteral("connecting");
    case Connected: return QStringLiteral("connected");
    case Disconnected: return QStringLiteral("disconnected");
    case Failed: return QStringLiteral("failed");
    case Closed: return QStringLiteral("closed");
    default: return QStringLiteral("unknown");
    }
}

bool WebRTCPeer::isInitiator() const
{
    return m_initiator;
}

QString WebRTCPeer::getLastError() const
{
    return m_lastError;
}

rtc::Configuration WebRTCPeer::buildConfiguration()
{
    rtc::Configuration config;
    config.iceServers = buildIceServers();
    config.disableAutoNegotiation = true;
    return config;
}

void WebRTCPeer::initializePeerConnection()
{
    m_peerConnection = std::make_shared<rtc::PeerConnection>(buildConfiguration());
    bindPeerConnectionCallbacks();
}

void WebRTCPeer::bindPeerConnectionCallbacks()
{
    if (!m_peerConnection) {
        return;
    }

    m_peerConnection->onLocalDescription([this](rtc::Description description) {
        const QString sdp = QString::fromStdString(std::string(description));
        if (description.type() == rtc::Description::Type::Offer) {
            emit offerCreated(sdp);
        } else if (description.type() == rtc::Description::Type::Answer) {
            emit answerReceived(sdp);
        }
    });

    m_peerConnection->onLocalCandidate([this](rtc::Candidate candidate) {
        emit iceCandidateGenerated(
            QString::fromStdString(candidate.candidate()),
            candidateMidToLineIndex(candidate));
    });

    m_peerConnection->onDataChannel([this](std::shared_ptr<rtc::DataChannel> backendChannel) {
        registerDataChannel(backendChannel);
    });

    m_peerConnection->onStateChange([this](rtc::PeerConnection::State state) {
        switch (state) {
        case rtc::PeerConnection::State::New:
            updateConnectionState(New);
            break;
        case rtc::PeerConnection::State::Connecting:
            updateConnectionState(Connecting);
            break;
        case rtc::PeerConnection::State::Connected:
            updateConnectionState(Connected);
            emit connectionEstablished();
            break;
        case rtc::PeerConnection::State::Disconnected:
            updateConnectionState(Disconnected);
            break;
        case rtc::PeerConnection::State::Failed:
            updateConnectionState(Failed);
            m_lastError = QStringLiteral("Peer connection failed");
            emit connectionFailed(m_lastError);
            break;
        case rtc::PeerConnection::State::Closed:
            updateConnectionState(Closed);
            emit connectionClosed();
            break;
        }
    });

    m_peerConnection->onIceStateChange([this](rtc::PeerConnection::IceState state) {
        emit iceConnectionStateChanged(enumToString(state));
    });

    m_peerConnection->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
        emit iceGatheringStateChanged(enumToString(state));
    });
}

void WebRTCPeer::registerDataChannel(const std::shared_ptr<rtc::DataChannel>& backendChannel)
{
    if (!backendChannel) {
        return;
    }

    const QString label = QString::fromStdString(backendChannel->label());
    auto channel = m_dataChannels.value(label);
    if (!channel) {
        channel = std::make_shared<WebRTCDataChannel>(label.toStdString());
        m_dataChannels[label] = channel;
    }

    const std::weak_ptr<WebRTCDataChannel> weakChannel = channel;
    channel->setBackendHandlers(
        [backendChannel](const std::vector<uint8_t>& data) {
            return backendChannel->sendBuffer(data);
        },
        [backendChannel](const std::string& text) {
            return backendChannel->send(text);
        },
        [backendChannel]() {
            backendChannel->close();
        });

    backendChannel->onOpen([weakChannel]() {
        if (auto channel = weakChannel.lock()) {
            channel->notifyOpen();
        }
    });

    backendChannel->onClosed([weakChannel]() {
        if (auto channel = weakChannel.lock()) {
            channel->notifyClosed();
        }
    });

    backendChannel->onError([weakChannel](std::string error) {
        if (auto channel = weakChannel.lock()) {
            channel->notifyError(error);
        }
    });

    backendChannel->onBufferedAmountLow([weakChannel]() {
        if (auto channel = weakChannel.lock()) {
            if (channel->onBufferedAmountLow) {
                channel->onBufferedAmountLow();
            }
        }
    });

    backendChannel->onMessage([weakChannel](rtc::message_variant message) {
        auto channel = weakChannel.lock();
        if (!channel) {
            return;
        }

        if (std::holds_alternative<rtc::binary>(message)) {
            const auto& binary = std::get<rtc::binary>(message);
            std::vector<uint8_t> payload;
            payload.reserve(binary.size());
            for (rtc::byte value : binary) {
                payload.push_back(static_cast<uint8_t>(value));
            }
            if (channel->onBinaryMessageReceived) {
                channel->onBinaryMessageReceived(payload);
            }
            return;
        }

        if (std::holds_alternative<rtc::string>(message)) {
            if (channel->onTextMessageReceived) {
                channel->onTextMessageReceived(std::get<rtc::string>(message));
            }
        }
    });

    emit dataChannelOpened(label);
}

void WebRTCPeer::updateConnectionState(ConnectionState state)
{
    if (m_connectionState == state) {
        return;
    }

    m_connectionState = state;
    emit connectionStateChanged(state);
}

void WebRTCPeer::on_connectionStateChanged()
{
    emit connectionStateChanged(m_connectionState);
}

void WebRTCPeer::on_iceConnectionStateChanged()
{
    emit iceConnectionStateChanged(getConnectionStateString());
}

void WebRTCPeer::on_iceGatheringStateChanged()
{
    emit iceGatheringStateChanged(QStringLiteral("gathering"));
}

void WebRTCPeer::on_iceCandidate(const QString& candidate, int sdpMLineIndex)
{
    emit iceCandidateGenerated(candidate, sdpMLineIndex);
}

void WebRTCPeer::on_dataChannelCreated(const QString& label)
{
    emit dataChannelOpened(label);
}
