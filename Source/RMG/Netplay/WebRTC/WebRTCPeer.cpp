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
#include <QDebug>
#include <QMap>

using namespace UserInterface::Netplay;

WebRTCPeer::WebRTCPeer(const QString& peerId, bool initiator, QObject* parent)
    : QObject(parent)
    , m_peerId(peerId)
    , m_initiator(initiator)
    , m_connectionState(New)
{
    qDebug() << "WebRTCPeer created:" << peerId << "initiator=" << initiator;

    // TODO: Initialize libdatachannel PeerConnection
    // Configuration:
    // - ICE servers (STUN/TURN)
    // - Media constraints
    // - Connection callbacks
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

    qDebug() << "WebRTCPeer: Creating offer for" << m_peerId;

    // TODO: Create SDP offer using libdatachannel
    // m_peerConnection->createOffer(...);
}

void WebRTCPeer::setRemoteDescription(const QString& sdpAnswer)
{
    qDebug() << "WebRTCPeer: Setting remote description for" << m_peerId;

    // TODO: Set remote SDP using libdatachannel
    // m_peerConnection->setRemoteDescription(...);
}

void WebRTCPeer::addICECandidate(const QString& candidate, int sdpMLineIndex)
{
    qDebug() << "WebRTCPeer: Adding ICE candidate for" << m_peerId;

    // TODO: Add ICE candidate using libdatachannel
    // m_peerConnection->addIceCandidate(...);
}

void WebRTCPeer::close()
{
    qDebug() << "WebRTCPeer: Closing connection with" << m_peerId;

    // TODO: Close peer connection using libdatachannel
    // m_peerConnection->close();

    m_connectionState = Closed;
    emit connectionClosed();
}

std::shared_ptr<WebRTCDataChannel> WebRTCPeer::createDataChannel(const QString& label)
{
    qDebug() << "WebRTCPeer: Creating data channel" << label << "for" << m_peerId;

    // TODO: Create data channel using libdatachannel
    auto channel = std::make_shared<WebRTCDataChannel>(label.toStdString());
    channel->open();
    m_dataChannels[label] = channel;
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
    case New: return "new";
    case Connecting: return "connecting";
    case Connected: return "connected";
    case Disconnected: return "disconnected";
    case Failed: return "failed";
    case Closed: return "closed";
    default: return "unknown";
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

//
// Private Slots
//

void WebRTCPeer::on_connectionStateChanged()
{
    // TODO: Handle connection state changes
    emit connectionStateChanged(m_connectionState);
}

void WebRTCPeer::on_iceConnectionStateChanged()
{
    emit iceConnectionStateChanged(getConnectionStateString());
}

void WebRTCPeer::on_iceGatheringStateChanged()
{
    emit iceGatheringStateChanged("gathering");
}

void WebRTCPeer::on_iceCandidate(const QString& candidate, int sdpMLineIndex)
{
    emit iceCandidateGenerated(candidate, sdpMLineIndex);
}

void WebRTCPeer::on_dataChannelCreated(const QString& label)
{
    emit dataChannelOpened(label);
}
