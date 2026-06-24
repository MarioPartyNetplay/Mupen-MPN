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
#include "TurnCredentialClient.hpp"
#include <RMG-Core/Netplay/WebRTC/WebRTCDataChannel.hpp>
#include "../NetplayProtocol.hpp"

#include <rtc/rtc.hpp>

#include <QDebug>
#include <QMap>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <sstream>

using namespace UserInterface::Netplay;

namespace {

constexpr int kDisconnectedGraceMs = 30000;

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
    , m_callbacksEnabled(std::make_shared<std::atomic<bool>>(true))
{
    m_disconnectedGraceTimer = new QTimer(this);
    m_disconnectedGraceTimer->setSingleShot(true);
    connect(m_disconnectedGraceTimer, &QTimer::timeout, this, [this]() {
        if (m_connectionState != Disconnected) {
            return;
        }

        qWarning() << "WebRTCPeer: ICE disconnected grace expired for" << m_peerId;
        if (m_initiator) {
            attemptRecovery();
            return;
        }

        m_lastError = QStringLiteral("Peer connection disconnected");
        emit connectionFailed(m_lastError);
    });

    qDebug() << "WebRTCPeer created:" << peerId << "initiator=" << initiator;
    initializePeerConnection();
}

WebRTCPeer::~WebRTCPeer()
{
    close();
}

void WebRTCPeer::attemptRecovery()
{
    if (!m_initiator || m_connectionState == Closed) {
        return;
    }

    if (m_connectionState == Failed || !m_peerConnection) {
        qInfo() << "WebRTCPeer: Rebuilding failed peer connection for" << m_peerId;
        rebuildPeerConnection();
        createDataChannel(QStringLiteral("RMG-Input"));
        createOffer();
        return;
    }

    qInfo() << "WebRTCPeer: Attempting ICE recovery for" << m_peerId;

    try {
        m_peerConnection->setLocalDescription(rtc::Description::Type::Offer);
        m_localDescriptionSent = true;
    } catch (const std::exception& exception) {
        m_lastError = QString::fromStdString(exception.what());
        emit connectionFailed(m_lastError);
    }
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

    if (m_callbacksEnabled) {
        m_callbacksEnabled->store(false);
    }

    if (m_disconnectedGraceTimer) {
        m_disconnectedGraceTimer->stop();
    }

    for (auto it = m_dataChannels.begin(); it != m_dataChannels.end(); ++it) {
        if (it.value()) {
            it.value()->onBinaryMessageReceived = nullptr;
            it.value()->onClosed = nullptr;
            it.value()->onError = nullptr;
            it.value()->onTextMessageReceived = nullptr;
            it.value()->onStateChanged = nullptr;
            it.value()->onBufferedAmountLow = nullptr;
            it.value()->detachBackendHandlers();
        }
    }

    if (m_peerConnection) {
        m_peerConnection->close();
        m_peerConnection.reset();
    }

    m_dataChannels.clear();

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

void WebRTCPeer::rebuildPeerConnection()
{
    if (m_disconnectedGraceTimer) {
        m_disconnectedGraceTimer->stop();
    }

    if (m_callbacksEnabled) {
        m_callbacksEnabled->store(false);
    }

    for (auto it = m_dataChannels.begin(); it != m_dataChannels.end(); ++it) {
        if (it.value()) {
            it.value()->onBinaryMessageReceived = nullptr;
            it.value()->onClosed = nullptr;
            it.value()->onError = nullptr;
            it.value()->onTextMessageReceived = nullptr;
            it.value()->onStateChanged = nullptr;
            it.value()->onBufferedAmountLow = nullptr;
            it.value()->detachBackendHandlers();
        }
    }
    m_dataChannels.clear();

    if (m_peerConnection) {
        m_peerConnection->close();
        m_peerConnection.reset();
    }

    m_callbacksEnabled = std::make_shared<std::atomic<bool>>(true);
    m_localDescriptionSent = false;
    updateConnectionState(New);
    initializePeerConnection();
}

void WebRTCPeer::dispatchToPeerThread(std::function<void()> action)
{
    if (QThread::currentThread() == thread()) {
        action();
        return;
    }

    QMetaObject::invokeMethod(
        this,
        [action = std::move(action)]() mutable { action(); },
        Qt::QueuedConnection);
}

void WebRTCPeer::handlePeerConnectionState(int state)
{
    switch (static_cast<rtc::PeerConnection::State>(state)) {
    case rtc::PeerConnection::State::New:
        updateConnectionState(New);
        break;
    case rtc::PeerConnection::State::Connecting:
        updateConnectionState(Connecting);
        break;
    case rtc::PeerConnection::State::Connected:
        if (m_disconnectedGraceTimer) {
            m_disconnectedGraceTimer->stop();
        }
        updateConnectionState(Connected);
        emit connectionEstablished();
        break;
    case rtc::PeerConnection::State::Disconnected:
        updateConnectionState(Disconnected);
        if (m_disconnectedGraceTimer && !m_disconnectedGraceTimer->isActive()) {
            qWarning() << "WebRTCPeer: ICE disconnected for" << m_peerId
                       << "- waiting" << kDisconnectedGraceMs << "ms before recovery";
            m_disconnectedGraceTimer->start(kDisconnectedGraceMs);
        }
        break;
    case rtc::PeerConnection::State::Failed:
        if (m_disconnectedGraceTimer) {
            m_disconnectedGraceTimer->stop();
        }
        updateConnectionState(Failed);
        m_lastError = QStringLiteral("Peer connection failed");
        emit connectionFailed(m_lastError);
        break;
    case rtc::PeerConnection::State::Closed:
        updateConnectionState(Closed);
        emit connectionClosed();
        break;
    }
}

void WebRTCPeer::bindPeerConnectionCallbacks()
{
    if (!m_peerConnection) {
        return;
    }

    const std::shared_ptr<std::atomic<bool>> callbacksEnabled = m_callbacksEnabled;

    m_peerConnection->onLocalDescription([this, callbacksEnabled](rtc::Description description) {
        const QString sdp = QString::fromStdString(std::string(description));
        const auto descriptionType = description.type();
        dispatchToPeerThread([this, callbacksEnabled, sdp, descriptionType]() {
            if (!callbacksEnabled || !callbacksEnabled->load()) {
                return;
            }
            if (descriptionType == rtc::Description::Type::Offer) {
                emit offerCreated(sdp);
            } else if (descriptionType == rtc::Description::Type::Answer) {
                emit answerReceived(sdp);
            }
        });
    });

    m_peerConnection->onLocalCandidate([this, callbacksEnabled](rtc::Candidate candidate) {
        const QString candidateSdp = QString::fromStdString(candidate.candidate());
        const int lineIndex = candidateMidToLineIndex(candidate);
        dispatchToPeerThread([this, callbacksEnabled, candidateSdp, lineIndex]() {
            if (!callbacksEnabled || !callbacksEnabled->load()) {
                return;
            }
            emit iceCandidateGenerated(candidateSdp, lineIndex);
        });
    });

    m_peerConnection->onDataChannel([this, callbacksEnabled](std::shared_ptr<rtc::DataChannel> backendChannel) {
        dispatchToPeerThread([this, callbacksEnabled, backendChannel]() {
            if (!callbacksEnabled || !callbacksEnabled->load()) {
                return;
            }
            registerDataChannel(backendChannel);
        });
    });

    m_peerConnection->onStateChange([this, callbacksEnabled](rtc::PeerConnection::State state) {
        const int stateValue = static_cast<int>(state);
        dispatchToPeerThread([this, callbacksEnabled, stateValue]() {
            if (!callbacksEnabled || !callbacksEnabled->load()) {
                return;
            }
            handlePeerConnectionState(stateValue);
        });
    });

    m_peerConnection->onIceStateChange([this, callbacksEnabled](rtc::PeerConnection::IceState state) {
        const QString stateString = enumToString(state);
        dispatchToPeerThread([this, callbacksEnabled, stateString]() {
            if (!callbacksEnabled || !callbacksEnabled->load()) {
                return;
            }
            emit iceConnectionStateChanged(stateString);
        });
    });

    m_peerConnection->onGatheringStateChange([this, callbacksEnabled](rtc::PeerConnection::GatheringState state) {
        const QString stateString = enumToString(state);
        dispatchToPeerThread([this, callbacksEnabled, stateString]() {
            if (!callbacksEnabled || !callbacksEnabled->load()) {
                return;
            }
            emit iceGatheringStateChanged(stateString);
        });
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
    } else {
        channel->detachBackendHandlers();
    }

    const std::weak_ptr<WebRTCDataChannel> weakChannel = channel;
    const std::shared_ptr<std::atomic<bool>> callbacksEnabled = m_callbacksEnabled;
    channel->setBackendHandlers(
        [backendChannel, callbacksEnabled](const std::vector<uint8_t>& data) {
            if (!callbacksEnabled || !callbacksEnabled->load()) {
                return false;
            }
            return backendChannel->sendBuffer(data);
        },
        [backendChannel, callbacksEnabled](const std::string& text) {
            if (!callbacksEnabled || !callbacksEnabled->load()) {
                return false;
            }
            return backendChannel->send(text);
        },
        [backendChannel, callbacksEnabled]() {
            if (!callbacksEnabled || !callbacksEnabled->load()) {
                return;
            }
            backendChannel->close();
        });

    backendChannel->onOpen([this, weakChannel, callbacksEnabled]() {
        dispatchToPeerThread([weakChannel, callbacksEnabled]() {
            if (!callbacksEnabled || !callbacksEnabled->load()) {
                return;
            }
            if (auto channel = weakChannel.lock()) {
                channel->notifyOpen();
            }
        });
    });

    backendChannel->onClosed([this, weakChannel, callbacksEnabled]() {
        dispatchToPeerThread([weakChannel, callbacksEnabled]() {
            if (!callbacksEnabled || !callbacksEnabled->load()) {
                return;
            }
            if (auto channel = weakChannel.lock()) {
                channel->notifyClosed();
            }
        });
    });

    backendChannel->onError([this, weakChannel, callbacksEnabled](std::string error) {
        dispatchToPeerThread([weakChannel, callbacksEnabled, error = std::move(error)]() mutable {
            if (!callbacksEnabled || !callbacksEnabled->load()) {
                return;
            }
            if (auto channel = weakChannel.lock()) {
                channel->notifyError(error);
            }
        });
    });

    backendChannel->onBufferedAmountLow([this, weakChannel, callbacksEnabled]() {
        dispatchToPeerThread([weakChannel, callbacksEnabled]() {
            if (!callbacksEnabled || !callbacksEnabled->load()) {
                return;
            }
            if (auto channel = weakChannel.lock()) {
                if (channel->onBufferedAmountLow) {
                    channel->onBufferedAmountLow();
                }
            }
        });
    });

    backendChannel->onMessage([this, weakChannel, callbacksEnabled](rtc::message_variant message) {
        if (!callbacksEnabled || !callbacksEnabled->load()) {
            return;
        }

        if (std::holds_alternative<rtc::binary>(message)) {
            const auto& binary = std::get<rtc::binary>(message);
            std::vector<uint8_t> payload;
            payload.reserve(binary.size());
            for (rtc::byte value : binary) {
                payload.push_back(static_cast<uint8_t>(value));
            }

            dispatchToPeerThread([weakChannel, callbacksEnabled, payload = std::move(payload)]() {
                if (!callbacksEnabled || !callbacksEnabled->load()) {
                    return;
                }
                if (auto channel = weakChannel.lock()) {
                    if (channel->onBinaryMessageReceived) {
                        channel->onBinaryMessageReceived(payload);
                    }
                }
            });
            return;
        }

        if (std::holds_alternative<rtc::string>(message)) {
            const std::string text = std::get<rtc::string>(message);
            dispatchToPeerThread([weakChannel, callbacksEnabled, text]() {
                if (!callbacksEnabled || !callbacksEnabled->load()) {
                    return;
                }
                if (auto channel = weakChannel.lock()) {
                    if (channel->onTextMessageReceived) {
                        channel->onTextMessageReceived(text);
                    }
                }
            });
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
