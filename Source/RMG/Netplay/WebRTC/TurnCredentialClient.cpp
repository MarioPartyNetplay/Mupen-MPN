/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "TurnCredentialClient.hpp"
#include "../NetplayProtocol.hpp"

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
#include <QUrlQuery>

#include <QDebug>

using namespace UserInterface::Netplay;

namespace {

constexpr int kCredentialRefreshSkewSeconds = 300;
constexpr int kDefaultTurnCredentialCacheTtlSeconds = 86400;

rtc::IceServer::RelayType relayTypeForUrl(const QUrl& url)
{
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("turns")) {
        return rtc::IceServer::RelayType::TurnTls;
    }

    const QString transport = QUrlQuery(url).queryItemValue(QStringLiteral("transport")).toLower();
    if (transport == QLatin1String("tcp")) {
        return rtc::IceServer::RelayType::TurnTcp;
    }

    return rtc::IceServer::RelayType::TurnUdp;
}

uint16_t defaultPortForScheme(const QString& scheme)
{
    if (scheme == QLatin1String("turns")) {
        return 5349;
    }
    return 3478;
}

bool shouldSkipIceUrl(const QUrl& url)
{
    if (!url.isValid() || url.host().isEmpty()) {
        return true;
    }

    // Cloudflare documents port 53 as prone to timeouts in some clients.
    if (url.port(defaultPortForScheme(url.scheme())) == 53) {
        return true;
    }

    return false;
}

std::vector<rtc::IceServer> parseTurnEntries(const QJsonArray& iceServers)
{
    std::vector<rtc::IceServer> servers;

    for (const QJsonValue& value : iceServers) {
        const QJsonObject entry = value.toObject();
        const QString username = entry.value(QStringLiteral("username")).toString();
        const QString credential = entry.value(QStringLiteral("credential")).toString();
        const QJsonArray urls = entry.value(QStringLiteral("urls")).toArray();

        for (const QJsonValue& urlValue : urls) {
            const QUrl url(urlValue.toString());
            if (shouldSkipIceUrl(url)) {
                continue;
            }

            const QString scheme = url.scheme().toLower();
            const std::string host = url.host().toStdString();
            const uint16_t port = static_cast<uint16_t>(url.port(defaultPortForScheme(scheme)));

            if (scheme == QLatin1String("stun") || scheme == QLatin1String("stuns")) {
                continue;
            }

            if (scheme == QLatin1String("turn") || scheme == QLatin1String("turns")) {
                if (username.isEmpty() || credential.isEmpty()) {
                    continue;
                }

                servers.emplace_back(
                    host,
                    port,
                    username.toStdString(),
                    credential.toStdString(),
                    relayTypeForUrl(url));
            }
        }
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
    if (!enabled) {
        m_turnServers.clear();
        m_expiresAt = QDateTime();
    }
}

bool TurnCredentialClient::connectionReversalEnabled() const
{
    QMutexLocker locker(&m_mutex);
    return m_connectionReversalEnabled;
}

void TurnCredentialClient::prefetch()
{
    if (!isConfigured()) {
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
    if (!isConfigured()) {
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

std::vector<rtc::IceServer> TurnCredentialClient::turnServers() const
{
    QMutexLocker locker(&m_mutex);
    return m_turnServers;
}

bool TurnCredentialClient::credentialsAreFreshLocked() const
{
    return !m_turnServers.empty() && m_expiresAt.isValid() &&
           m_expiresAt > QDateTime::currentDateTimeUtc().addSecs(kCredentialRefreshSkewSeconds);
}

bool TurnCredentialClient::fetchCredentialsBlocking(int timeoutMs)
{
    const QUrl brokerUrl = netplayTurnIceServersUrl();
    if (brokerUrl.isEmpty()) {
        QMutexLocker locker(&m_mutex);
        m_fetchInProgress = false;
        return false;
    }

    QString errorMessage;
    std::vector<rtc::IceServer> fetchedServers;

    if (fetchFromBroker(brokerUrl, timeoutMs, &fetchedServers, &errorMessage)) {
        QMutexLocker locker(&m_mutex);
        m_fetchInProgress = false;
        m_turnServers = std::move(fetchedServers);
        m_expiresAt = QDateTime::currentDateTimeUtc().addSecs(kDefaultTurnCredentialCacheTtlSeconds);
        qInfo() << "TurnCredentialClient: Loaded" << m_turnServers.size()
                << "TURN ICE servers from broker";
        return true;
    }

    if (!errorMessage.isEmpty()) {
        qWarning() << "TurnCredentialClient:" << errorMessage;
    }

    QMutexLocker locker(&m_mutex);
    m_fetchInProgress = false;
    return !m_turnServers.empty();
}

bool TurnCredentialClient::fetchFromBroker(
    const QUrl& brokerUrl,
    int timeoutMs,
    std::vector<rtc::IceServer>* serversOut,
    QString* errorOut)
{
    if (!serversOut) {
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
            success = parseIceServersResponse(reply->readAll(), serversOut, &localError);
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
    std::vector<rtc::IceServer>* serversOut,
    QString* errorOut)
{
    if (!serversOut) {
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
    const QJsonArray iceServers = root.value(QStringLiteral("iceServers")).toArray();
    if (iceServers.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("TURN broker response did not include iceServers");
        }
        return false;
    }

    const std::vector<rtc::IceServer> parsedServers = parseTurnEntries(iceServers);
    if (parsedServers.empty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("TURN broker response did not include usable TURN URLs");
        }
        return false;
    }

    *serversOut = parsedServers;
    return true;
}

namespace UserInterface::Netplay {

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

    TurnCredentialClient& turnClient = TurnCredentialClient::instance();
    if (turnClient.isConfigured()) {
        turnClient.ensureCredentials();
        const std::vector<rtc::IceServer> turnServers = turnClient.turnServers();
        servers.insert(servers.end(), turnServers.begin(), turnServers.end());
    }

    return servers;
}

} // namespace UserInterface::Netplay
