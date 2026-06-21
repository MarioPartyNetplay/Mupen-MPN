/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "TurnCredentialClient.hpp"
#include "../NetplayProtocol.hpp"

#include <QFile>
#include <QMutexLocker>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <QDebug>

#include <stdexcept>

using namespace UserInterface::Netplay;

namespace {

constexpr int kCredentialRefreshSkewSeconds = 300;
constexpr int kDefaultTurnCredentialCacheTtlSeconds = 86400;

struct ParsedIceServers {
    std::vector<rtc::IceServer> stun;
    std::vector<rtc::IceServer> turn;
};

QJsonArray iceUrlsFromJsonValue(const QJsonValue& value)
{
    if (value.isArray()) {
        return value.toArray();
    }

    QJsonArray urls;
    if (value.isString()) {
        const QString url = value.toString().trimmed();
        if (!url.isEmpty()) {
            urls.append(url);
        }
    }
    return urls;
}

bool shouldSkipIceServerPort(uint16_t port)
{
    // Cloudflare documents port 53 as prone to timeouts in some clients.
    return port == 53;
}

bool appendIceServerUrl(
    const QString& urlString,
    const QString& username,
    const QString& credential,
    ParsedIceServers* servers)
{
    if (!servers) {
        return false;
    }

    const QString trimmed = urlString.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }

    try {
        rtc::IceServer server(trimmed.toStdString());
        if (shouldSkipIceServerPort(server.port)) {
            return false;
        }

        if (server.type == rtc::IceServer::Type::Turn) {
            if (username.isEmpty() || credential.isEmpty()) {
                return false;
            }

            server.username = username.toStdString();
            server.password = credential.toStdString();
            servers->turn.push_back(std::move(server));
            return true;
        }

        servers->stun.push_back(std::move(server));
        return true;
    } catch (const std::exception& exception) {
        qWarning() << "TurnCredentialClient: Skipping invalid ICE URL"
                   << trimmed << "-" << exception.what();
        return false;
    }
}

void parseIceServerEntry(const QJsonObject& entry, ParsedIceServers* servers)
{
    if (!servers || entry.isEmpty()) {
        return;
    }

    const QString username = entry.value(QStringLiteral("username")).toString();
    const QString credential =
        entry.value(QStringLiteral("credential")).toString().isEmpty()
            ? entry.value(QStringLiteral("password")).toString()
            : entry.value(QStringLiteral("credential")).toString();

    for (const QJsonValue& urlValue : iceUrlsFromJsonValue(entry.value(QStringLiteral("urls")))) {
        appendIceServerUrl(urlValue.toString(), username, credential, servers);
    }
}

ParsedIceServers parseIceEntries(const QJsonArray& iceServers)
{
    ParsedIceServers servers;

    for (const QJsonValue& value : iceServers) {
        parseIceServerEntry(value.toObject(), &servers);
    }

    return servers;
}

} // namespace

TurnCredentialClient& TurnCredentialClient::instance()
{
    static TurnCredentialClient client;
    return client;
}

bool TurnCredentialClient::isConfigured() const
{
    if (!turnCredentialsAvailable()) {
        return false;
    }

    QMutexLocker locker(&m_mutex);
    return m_connectionReversalEnabled;
}

void TurnCredentialClient::setConnectionReversalEnabled(bool enabled)
{
    QMutexLocker locker(&m_mutex);
    m_connectionReversalEnabled = enabled;
}

bool TurnCredentialClient::connectionReversalEnabled() const
{
    QMutexLocker locker(&m_mutex);
    return m_connectionReversalEnabled;
}

void TurnCredentialClient::prefetch()
{
    if (!turnCredentialsAvailable() && qgetenv("RMG_ICE_CONFIG_FILE").isEmpty()) {
        return;
    }

    QMutexLocker locker(&m_mutex);
    if (credentialsAreFreshLocked() || m_fetchInProgress) {
        return;
    }
    m_fetchInProgress = true;
    locker.unlock();

    fetchCredentialsBlocking(10000);
}

bool TurnCredentialClient::ensureCredentials(int timeoutMs)
{
    if (!turnCredentialsAvailable() && qgetenv("RMG_ICE_CONFIG_FILE").isEmpty()) {
        return true;
    }

    {
        QMutexLocker locker(&m_mutex);
        if (credentialsAreFreshLocked()) {
            return true;
        }
    }

    return fetchCredentialsBlocking(timeoutMs);
}

std::vector<rtc::IceServer> TurnCredentialClient::stunServers() const
{
    QMutexLocker locker(&m_mutex);
    return m_stunServers;
}

std::vector<rtc::IceServer> TurnCredentialClient::turnServers() const
{
    QMutexLocker locker(&m_mutex);
    return m_turnServers;
}

bool TurnCredentialClient::credentialsAreFreshLocked() const
{
    return m_expiresAt.isValid() &&
           m_expiresAt > QDateTime::currentDateTimeUtc().addSecs(kCredentialRefreshSkewSeconds) &&
           (!m_stunServers.empty() || !m_turnServers.empty());
}

bool TurnCredentialClient::fetchCredentialsBlocking(int timeoutMs)
{
    QString errorMessage;
    std::vector<rtc::IceServer> fetchedStunServers;
    std::vector<rtc::IceServer> fetchedTurnServers;

    const QByteArray configFilePath = qgetenv("RMG_ICE_CONFIG_FILE");
    if (!configFilePath.isEmpty()) {
        QFile configFile(QString::fromUtf8(configFilePath));
        if (configFile.open(QIODevice::ReadOnly) &&
            parseIceServersResponse(configFile.readAll(), &fetchedStunServers, &fetchedTurnServers,
                                    &errorMessage)) {
            QMutexLocker locker(&m_mutex);
            m_fetchInProgress = false;
            m_stunServers = std::move(fetchedStunServers);
            m_turnServers = std::move(fetchedTurnServers);
            m_expiresAt = QDateTime::currentDateTimeUtc().addSecs(kDefaultTurnCredentialCacheTtlSeconds);
            qInfo() << "TurnCredentialClient: Loaded" << m_stunServers.size() << "STUN and"
                    << m_turnServers.size() << "TURN ICE servers from" << configFile.fileName();
            return true;
        }

        if (!errorMessage.isEmpty()) {
            qWarning() << "TurnCredentialClient:" << errorMessage;
        }
    }

    const QUrl brokerUrl = netplayTurnIceServersUrl();
    if (brokerUrl.isEmpty()) {
        QMutexLocker locker(&m_mutex);
        m_fetchInProgress = false;
        return false;
    }

    if (fetchFromBroker(brokerUrl, timeoutMs, &fetchedStunServers, &fetchedTurnServers, &errorMessage)) {
        QMutexLocker locker(&m_mutex);
        m_fetchInProgress = false;
        m_stunServers = std::move(fetchedStunServers);
        m_turnServers = std::move(fetchedTurnServers);
        m_expiresAt = QDateTime::currentDateTimeUtc().addSecs(kDefaultTurnCredentialCacheTtlSeconds);
        qInfo() << "TurnCredentialClient: Loaded" << m_stunServers.size() << "STUN and"
                << m_turnServers.size() << "TURN ICE servers from broker";
        return true;
    }

    if (!errorMessage.isEmpty()) {
        qWarning() << "TurnCredentialClient:" << errorMessage;
    }

    QMutexLocker locker(&m_mutex);
    m_fetchInProgress = false;
    return !m_stunServers.empty() || !m_turnServers.empty();
}

bool TurnCredentialClient::fetchFromBroker(
    const QUrl& brokerUrl,
    int timeoutMs,
    std::vector<rtc::IceServer>* stunOut,
    std::vector<rtc::IceServer>* turnOut,
    QString* errorOut)
{
    if (!stunOut || !turnOut) {
        return false;
    }

    QNetworkAccessManager networkManager;
    QNetworkRequest request(brokerUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("RMG-Netplay/1.0"));
    request.setTransferTimeout(timeoutMs);

    QNetworkReply* reply = networkManager.get(request);

    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeoutTimer.start(timeoutMs);
    loop.exec();

    const bool timedOut = !reply->isFinished();
    bool success = false;
    QString localError;

    if (timedOut) {
        localError = QStringLiteral("TURN broker request timed out");
        reply->abort();
    } else if (reply->error() != QNetworkReply::NoError) {
        localError = reply->errorString();
    } else {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode >= 200 && statusCode < 300) {
            success = parseIceServersResponse(reply->readAll(), stunOut, turnOut, &localError);
        } else {
            localError = QStringLiteral("TURN broker returned HTTP %1").arg(statusCode);
        }
    }

    reply->deleteLater();

    if (!success && errorOut) {
        *errorOut = localError;
    }
    return success;
}

bool TurnCredentialClient::parseIceServersResponse(
    const QByteArray& payload,
    std::vector<rtc::IceServer>* stunOut,
    std::vector<rtc::IceServer>* turnOut,
    QString* errorOut)
{
    if (!stunOut || !turnOut) {
        return false;
    }

    const QJsonDocument document = QJsonDocument::fromJson(payload);
    if (!document.isObject()) {
        if (errorOut) {
            *errorOut = QStringLiteral("TURN broker response was not valid JSON");
        }
        return false;
    }

    const QJsonObject root = document.object();
    QJsonArray iceServers = root.value(QStringLiteral("iceServers")).toArray();

    // gopher64-style flat config: { "urls": [...], "username": "...", "credential": "..." }
    if (iceServers.isEmpty() && root.contains(QStringLiteral("urls"))) {
        iceServers.append(root);
    }

    if (iceServers.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("ICE config did not include iceServers or urls");
        }
        return false;
    }

    const ParsedIceServers parsedServers = parseIceEntries(iceServers);
    if (parsedServers.stun.empty() && parsedServers.turn.empty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("ICE config did not include usable STUN/TURN URLs");
        }
        return false;
    }

    *stunOut = parsedServers.stun;
    *turnOut = parsedServers.turn;
    return true;
}

namespace UserInterface::Netplay {

std::vector<rtc::IceServer> buildIceServers()
{
    std::vector<rtc::IceServer> servers;

    TurnCredentialClient& iceClient = TurnCredentialClient::instance();
    if (turnCredentialsAvailable() || !qgetenv("RMG_ICE_CONFIG_FILE").isEmpty()) {
        if (!iceClient.ensureCredentials()) {
            qWarning() << "TurnCredentialClient: Failed to refresh ICE credentials; using cached or fallback STUN";
        }

        const std::vector<rtc::IceServer> brokerStunServers = iceClient.stunServers();
        servers.insert(servers.end(), brokerStunServers.begin(), brokerStunServers.end());

        const std::vector<rtc::IceServer> turnServers = iceClient.turnServers();
        servers.insert(servers.end(), turnServers.begin(), turnServers.end());
    }

    if (servers.empty()) {
        const QString stunHost = stunServerHost().trimmed();
        const quint16 stunPort = stunServerPort();
        if (!stunHost.isEmpty() && stunPort > 0) {
            servers.emplace_back(stunHost.toStdString(), static_cast<uint16_t>(stunPort));
        }
    }

    for (const rtc::IceServer& server : servers) {
        if (server.type == rtc::IceServer::Type::Stun) {
            qInfo() << "ICE STUN server:" << QString::fromStdString(server.hostname) << server.port;
        } else {
            qInfo() << "ICE TURN server:" << QString::fromStdString(server.hostname) << server.port
                    << "(relay"
                    << (server.relayType == rtc::IceServer::RelayType::TurnTls ? "TLS" :
                        server.relayType == rtc::IceServer::RelayType::TurnTcp ? "TCP" : "UDP")
                    << ")";
        }
    }

    if (servers.empty()) {
        qWarning() << "TurnCredentialClient: No ICE servers configured";
    }

    return servers;
}

} // namespace UserInterface::Netplay
