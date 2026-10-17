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

class QNetworkAccessManager;
class QNetworkReply;

namespace UserInterface::Netplay {

/**
 * Fetches and caches short-lived STUN/TURN credentials from the netplay server's
 * /turn/ice-servers broker. The Cloudflare API token stays on the server.
 */
class TurnCredentialClient {
public:
    static TurnCredentialClient& instance();

    bool isConfigured() const;
    void prefetch();
    bool ensureCredentials(int timeoutMs = 45000);
    bool hasFreshCredentials() const;
    bool isFetchInProgress() const;
    std::vector<rtc::IceServer> stunServers() const;
    std::vector<rtc::IceServer> turnServers() const;

private:
    TurnCredentialClient() = default;
    ~TurnCredentialClient();

    bool credentialsAreFreshLocked() const;
    bool loadIceConfigFile();
    void startBrokerRequest();
    void finishBrokerRequest();
    bool waitForBroker(int timeoutMs);
    bool parseIceServersResponse(const QByteArray& payload, std::vector<rtc::IceServer>* stunOut,
                                 std::vector<rtc::IceServer>* turnOut, QString* errorOut, qint64* expiresUnixOut);

    mutable QMutex m_mutex;
    std::vector<rtc::IceServer> m_stunServers;
    std::vector<rtc::IceServer> m_turnServers;
    QDateTime m_expiresAt;
    QNetworkAccessManager* m_network = nullptr;
    QNetworkReply* m_reply = nullptr;
    bool m_fetchInProgress = false;
};

std::vector<rtc::IceServer> buildIceServers();

inline void applyNetplayConnectionSettings(NetplayConnectionMode mode, bool useUpnp)
{
    Q_UNUSED(mode);
    Q_UNUSED(useUpnp);
    TurnCredentialClient::instance().prefetch();
}

} // namespace UserInterface::Netplay

#endif // TURNCREDENTIALCLIENT_HPP
