/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#ifndef TURNCREDENTIALCLIENT_HPP
#define TURNCREDENTIALCLIENT_HPP

#include "../NetplayProtocol.hpp"

#include <rtc/configuration.hpp>

#include <QDateTime>
#include <QMutex>
#include <QString>
#include <QUrl>
#include <vector>

namespace UserInterface::Netplay {

/** Fetches and caches short-lived TURN credentials from the netplay index server. */
class TurnCredentialClient {
public:
    static TurnCredentialClient& instance();

    bool isConfigured() const;
    void setConnectionReversalEnabled(bool enabled);
    bool connectionReversalEnabled() const;
    void prefetch();
    bool ensureCredentials(int timeoutMs = 10000);
    std::vector<rtc::IceServer> turnServers() const;

private:
    TurnCredentialClient() = default;

    bool credentialsAreFreshLocked() const;
    bool fetchCredentialsBlocking(int timeoutMs);
    bool fetchFromBroker(const QUrl& brokerUrl, int timeoutMs, std::vector<rtc::IceServer>* serversOut, QString* errorOut);
    bool parseIceServersResponse(const QByteArray& payload, std::vector<rtc::IceServer>* serversOut, QString* errorOut);

    mutable QMutex m_mutex;
    std::vector<rtc::IceServer> m_turnServers;
    QDateTime m_expiresAt;
    bool m_fetchInProgress = false;
    bool m_connectionReversalEnabled = false;
};

std::vector<rtc::IceServer> buildIceServers();

inline void applyNetplayConnectionSettings(NetplayConnectionMode mode, bool useUpnp)
{
    Q_UNUSED(useUpnp);
    TurnCredentialClient& client = TurnCredentialClient::instance();
    const bool useTraversalServer = mode == NetplayConnectionMode::TraversalServer;
    client.setConnectionReversalEnabled(useTraversalServer);
    if (useTraversalServer) {
        client.prefetch();
    }
}

} // namespace UserInterface::Netplay

#endif // TURNCREDENTIALCLIENT_HPP
