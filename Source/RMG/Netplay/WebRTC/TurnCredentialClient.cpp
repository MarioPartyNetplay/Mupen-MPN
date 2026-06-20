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

struct ParsedIceUrl {
    QString scheme;
    QString host;
    uint16_t port = 0;
    QString transport;
    bool valid = false;
};

uint16_t defaultPortForScheme(const QString& scheme)
{
    if (scheme == QLatin1String("turns") || scheme == QLatin1String("stuns")) {
        return 5349;
    }
    return 3478;
}

ParsedIceUrl parseIceServerUrl(const QString& urlString)
{
    ParsedIceUrl result;
    const QString trimmed = urlString.trimmed();
    if (trimmed.isEmpty()) {
        return result;
    }

    // QUrl extracts the scheme for stun:/turn:/turns: URLs, but host:port lives in path().
    const QUrl url(trimmed);
    if (!url.isValid()) {
        return result;
    }

    result.scheme = url.scheme().toLower();
    if (result.scheme.isEmpty()) {
        return result;
    }

    QString hostPort = url.host();
    if (hostPort.isEmpty()) {
        hostPort = url.path();
    }
    if (hostPort.isEmpty()) {
        return result;
    }

    const QString query = url.query();
    if (!query.isEmpty()) {
        result.transport = QUrlQuery(query).queryItemValue(QStringLiteral("transport")).toLower();
    }

    const uint16_t defaultPort = defaultPortForScheme(result.scheme);

    if (hostPort.startsWith(QLatin1Char('['))) {
        const int closeIndex = hostPort.indexOf(QLatin1Char(']'));
        if (closeIndex <= 1) {
            return result;
        }
        result.host = hostPort.mid(1, closeIndex - 1);
        QString portPart = hostPort.mid(closeIndex + 1).trimmed();
        if (portPart.startsWith(QLatin1Char(':'))) {
            bool ok = false;
            const int parsed = portPart.mid(1).toInt(&ok);
            if (!ok || parsed < 1 || parsed > 65535) {
                return result;
            }
            result.port = static_cast<uint16_t>(parsed);
        } else {
            result.port = defaultPort;
        }
    } else {
        const int colonIndex = hostPort.lastIndexOf(QLatin1Char(':'));
        if (colonIndex > 0) {
            bool ok = false;
            const int parsed = hostPort.mid(colonIndex + 1).toInt(&ok);
            if (ok && parsed >= 1 && parsed <= 65535) {
                result.host = hostPort.left(colonIndex);
                result.port = static_cast<uint16_t>(parsed);
            } else {
                result.host = hostPort;
                result.port = defaultPort;
            }
        } else {
            result.host = hostPort;
            result.port = defaultPort;
        }
    }

    if (result.host.isEmpty()) {
        return result;
    }

    result.valid = true;
    return result;
}

rtc::IceServer::RelayType relayTypeForParsedUrl(const ParsedIceUrl& iceUrl)
{
    if (iceUrl.scheme == QLatin1String("turns") || iceUrl.scheme == QLatin1String("stuns")) {
        return rtc::IceServer::RelayType::TurnTls;
    }

    if (iceUrl.transport == QLatin1String("tcp")) {
        return rtc::IceServer::RelayType::TurnTcp;
    }

    return rtc::IceServer::RelayType::TurnUdp;
}

bool shouldSkipIceUrl(const ParsedIceUrl& iceUrl)
{
    if (!iceUrl.valid || iceUrl.host.isEmpty()) {
        return true;
    }

    // Cloudflare documents port 53 as prone to timeouts in some clients.
    if (iceUrl.port == 53) {
        return true;
    }

    return false;
}

struct ParsedIceServers {
    std::vector<rtc::IceServer> stun;
    std::vector<rtc::IceServer> turn;
};

rtc::IceServer makeStunServer(const ParsedIceUrl& iceUrl)
{
    const std::string endpoint = iceUrl.port > 0
        ? QStringLiteral("%1:%2").arg(iceUrl.host).arg(iceUrl.port).toStdString()
        : iceUrl.host.toStdString();
    return rtc::IceServer(endpoint);
}

ParsedIceServers parseIceEntries(const QJsonArray& iceServers)
{
    ParsedIceServers servers;

    for (const QJsonValue& value : iceServers) {
        const QJsonObject entry = value.toObject();
        const QString username = entry.value(QStringLiteral("username")).toString();
        const QString credential = entry.value(QStringLiteral("credential")).toString();
        const QJsonArray urls = entry.value(QStringLiteral("urls")).toArray();

        for (const QJsonValue& urlValue : urls) {
            const ParsedIceUrl iceUrl = parseIceServerUrl(urlValue.toString());
            if (shouldSkipIceUrl(iceUrl)) {
                continue;
            }

            if (iceUrl.scheme == QLatin1String("stun") || iceUrl.scheme == QLatin1String("stuns")) {
                servers.stun.push_back(makeStunServer(iceUrl));
                continue;
            }

            if (iceUrl.scheme == QLatin1String("turn") || iceUrl.scheme == QLatin1String("turns")) {
                if (username.isEmpty() || credential.isEmpty()) {
                    continue;
                }

                servers.turn.emplace_back(
                    iceUrl.host.toStdString(),
                    iceUrl.port,
                    username.toStdString(),
                    credential.toStdString(),
                    relayTypeForParsedUrl(iceUrl));
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
}

bool TurnCredentialClient::connectionReversalEnabled() const
{
    QMutexLocker locker(&m_mutex);
    return m_connectionReversalEnabled;
}

void TurnCredentialClient::prefetch()
{
    if (!turnCredentialsAvailable()) {
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
    if (!turnCredentialsAvailable()) {
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
    return !m_stunServers.empty() && m_expiresAt.isValid() &&
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
    std::vector<rtc::IceServer> fetchedStunServers;
    std::vector<rtc::IceServer> fetchedTurnServers;

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
    return !m_stunServers.empty();
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
    const QJsonArray iceServers = root.value(QStringLiteral("iceServers")).toArray();
    if (iceServers.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("TURN broker response did not include iceServers");
        }
        return false;
    }

    const ParsedIceServers parsedServers = parseIceEntries(iceServers);
    if (parsedServers.stun.empty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("TURN broker response did not include usable STUN URLs");
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
    if (turnCredentialsAvailable()) {
        iceClient.ensureCredentials();
        const std::vector<rtc::IceServer> brokerStunServers = iceClient.stunServers();
        servers.insert(servers.end(), brokerStunServers.begin(), brokerStunServers.end());
    }

    if (servers.empty()) {
        const QString stunHost = stunServerHost().trimmed();
        const quint16 stunPort = stunServerPort();
        if (!stunHost.isEmpty()) {
            const std::string endpoint = stunPort > 0
                ? QStringLiteral("%1:%2").arg(stunHost).arg(stunPort).toStdString()
                : stunHost.toStdString();
            servers.emplace_back(endpoint);
        }
    }

    if (iceClient.isConfigured()) {
        const std::vector<rtc::IceServer> turnServers = iceClient.turnServers();
        servers.insert(servers.end(), turnServers.begin(), turnServers.end());
    }

    return servers;
}

} // namespace UserInterface::Netplay
