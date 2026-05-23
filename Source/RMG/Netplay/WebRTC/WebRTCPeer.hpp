/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef WEBRTCPEER_HPP
#define WEBRTCPEER_HPP

#include <QString>
#include <QObject>
#include <QByteArray>
#include <QMap>
#include <memory>
#include <functional>

namespace UserInterface::Netplay {

class WebRTCDataChannel;

/**
 * @class WebRTCPeer
 * @brief Manages a single WebRTC peer connection
 * 
 * Handles:
 * - WebRTC peer connection lifecycle
 * - SDP offer/answer negotiation
 * - ICE candidate gathering
 * - Data channel creation
 * - Connection state monitoring
 */
class WebRTCPeer : public QObject {
    Q_OBJECT

public:
    enum ConnectionState {
        New,
        Connecting,
        Connected,
        Disconnected,
        Failed,
        Closed
    };

    // Constructor/Destructor
    explicit WebRTCPeer(const QString& peerId, bool initiator = false, QObject* parent = nullptr);
    ~WebRTCPeer();

    // Connection Management
    void createOffer();
    void setRemoteDescription(const QString& sdpAnswer);
    void addICECandidate(const QString& candidate, int sdpMLineIndex = 0);

    void close();

    // Data Channel
    std::shared_ptr<WebRTCDataChannel> createDataChannel(const QString& label);
    std::shared_ptr<WebRTCDataChannel> getDataChannel(const QString& label);

    // Getters
    QString getPeerId() const;
    ConnectionState getConnectionState() const;
    QString getConnectionStateString() const;
    bool isInitiator() const;

    // Error handling
    QString getLastError() const;

signals:
    // Connection signals
    void connectionStateChanged(ConnectionState state);
    void connectionEstablished();
    void connectionFailed(const QString& reason);
    void connectionClosed();

    // SDP Signaling signals
    void offerCreated(const QString& sdpOffer);
    void answerReceived(const QString& sdpAnswer);

    // ICE Signaling signals
    void iceCandidateGenerated(const QString& candidate, int sdpMLineIndex);
    void iceConnectionStateChanged(const QString& state);
    void iceGatheringStateChanged(const QString& state);

    // Data Channel signals
    void dataChannelOpened(const QString& label);
    void dataChannelClosed(const QString& label);
    void dataChannelError(const QString& label, const QString& error);

private slots:
    void on_connectionStateChanged();
    void on_iceConnectionStateChanged();
    void on_iceGatheringStateChanged();
    void on_iceCandidate(const QString& candidate, int sdpMLineIndex);
    void on_dataChannelCreated(const QString& label);

private:
    // WebRTC implementation would go here
    // Using libdatachannel as the backend

    QString m_peerId;
    bool m_initiator;
    ConnectionState m_connectionState;
    QString m_lastError;

    QMap<QString, std::shared_ptr<WebRTCDataChannel>> m_dataChannels;

    // TODO: Actual WebRTC peer connection object from libdatachannel
    // std::shared_ptr<rtc::PeerConnection> m_peerConnection;
    // std::map<QString, std::shared_ptr<WebRTCDataChannel>> m_dataChannels;
};

} // namespace UserInterface::Netplay

#endif // WEBRTCPEER_HPP
