/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "TurnCredentialClient.hpp"
#include "../NetplayProtocol.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
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
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>

#include <QDebug>

#include <algorithm>

using namespace UserInterface::Netplay;

namespace {

constexpr int kCredentialRefreshSkewSeconds = 300;
constexpr int kDefaultTurnCredentialCacheTtlSeconds = 86400;
constexpr int kMaxLibjuiceTurnServers = 2;
constexpr int kMaxBrokerAttempts = 3;

struct ParsedIceUrl {
    QString scheme;
    QString host;
    uint16_t port = 0;
    QString transport;
    bool valid = false;
};

struct ParsedIceServers {
    std::vector<rtc::IceServer> stun;
    std::vector<rtc::IceServer> turn;
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
        const QString portPart = hostPort.mid(closeIndex + 1).trimmed();
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

rtc::IceServer makeStunServer(const ParsedIceUrl& iceUrl)
{
    return rtc::IceServer(iceUrl.host.toStdString(), iceUrl.port);
}

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
        const ParsedIceUrl iceUrl = parseIceServerUrl(urlValue.toString());
        if (shouldSkipIceUrl(iceUrl)) {
            continue;
        }

        if (iceUrl.scheme == QLatin1String("stun") || iceUrl.scheme == QLatin1String("stuns")) {
            servers->stun.push_back(makeStunServer(iceUrl));
            continue;
        }

        if (iceUrl.scheme == QLatin1String("turn") || iceUrl.scheme == QLatin1String("turns")) {
            if (username.isEmpty() || credential.isEmpty()) {
                continue;
            }

            const rtc::IceServer::RelayType relayType = relayTypeForParsedUrl(iceUrl);
            if (relayType != rtc::IceServer::RelayType::TurnUdp) {
                continue;
            }

            servers->turn.emplace_back(
                iceUrl.host.toStdString(),
                iceUrl.port,
                username.toStdString(),
                credential.toStdString(),
                relayType);
        }
    }
}

qint64 usernameExpiryUnix(const QString& username)
{
    const int colon = username.indexOf(QLatin1Char(':'));
    if (colon < 10) {
        return 0;
    }

    bool ok = false;
    const qint64 stamp = username.left(colon).toLongLong(&ok);
    if (!ok || stamp < 1000000000LL) {
        return 0;
    }
    return stamp;
}

qint64 earliestUsernameExpiry(const QJsonArray& iceServers)
{
    qint64 earliest = 0;
    for (const QJsonValue& value : iceServers) {
        const qint64 stamp = usernameExpiryUnix(value.toObject().value(QStringLiteral("username")).toString());
        if (stamp > 0 && (earliest == 0 || stamp < earliest)) {
            earliest = stamp;
        }
    }
    return earliest;
}

qint64 brokerExpiryUnix(const QJsonObject& root, const QJsonArray& iceServers)
{
    qint64 expiry = 0;
    const QJsonValue expiresValue = root.value(QStringLiteral("expires"));
    if (expiresValue.isDouble()) {
        expiry = static_cast<qint64>(expiresValue.toDouble());
    } else if (expiresValue.isString()) {
        bool ok = false;
        const qint64 parsed = expiresValue.toString().toLongLong(&ok);
        if (ok) {
            expiry = parsed;
        }
    }

    const qint64 usernameExpiry = earliestUsernameExpiry(iceServers);
    if (usernameExpiry > 0 && (expiry == 0 || usernameExpiry < expiry)) {
        expiry = usernameExpiry;
    }
    return expiry;
}

ParsedIceServers parseIceEntries(const QJsonArray& iceServers)
{
    ParsedIceServers servers;

    for (const QJsonValue& value : iceServers) {
        parseIceServerEntry(value.toObject(), &servers);
    }

    return servers;
}

bool iceServerIdentityLess(const rtc::IceServer& lhs, const rtc::IceServer& rhs)
{
    if (lhs.type != rhs.type) {
        return lhs.type < rhs.type;
    }
    if (lhs.hostname != rhs.hostname) {
        return lhs.hostname < rhs.hostname;
    }
    return lhs.port < rhs.port;
}

void appendUniqueIceServer(std::vector<rtc::IceServer>& servers, const rtc::IceServer& server)
{
    for (const rtc::IceServer& existing : servers) {
        if (existing.type == server.type && existing.hostname == server.hostname &&
            existing.port == server.port) {
            return;
        }
    }
    servers.push_back(server);
}

void appendFallbackStun(std::vector<rtc::IceServer>& servers)
{
    const QString stunHost = stunServerHost().trimmed();
    const quint16 stunPort = stunServerPort();
    if (stunHost.isEmpty() || stunPort == 0) {
        return;
    }

    appendUniqueIceServer(servers, rtc::IceServer(stunHost.toStdString(), static_cast<uint16_t>(stunPort)));
}

std::vector<rtc::IceServer> finalizeIceServers(std::vector<rtc::IceServer> servers)
{
    std::vector<rtc::IceServer> stunServers;
    std::vector<rtc::IceServer> turnServers;

    for (const rtc::IceServer& server : servers) {
        if (server.type == rtc::IceServer::Type::Stun) {
            appendUniqueIceServer(stunServers, server);
        } else if (server.relayType == rtc::IceServer::RelayType::TurnUdp) {
            appendUniqueIceServer(turnServers, server);
        }
    }

    appendFallbackStun(stunServers);

    std::sort(turnServers.begin(), turnServers.end(), [](const rtc::IceServer& lhs, const rtc::IceServer& rhs) {
        const bool lhsPreferred = lhs.hostname.find("turnv2.realtime.cloudflare.com") != std::string::npos;
        const bool rhsPreferred = rhs.hostname.find("turnv2.realtime.cloudflare.com") != std::string::npos;
        if (lhsPreferred != rhsPreferred) {
            return lhsPreferred;
        }
        return iceServerIdentityLess(lhs, rhs);
    });

    if (static_cast<int>(turnServers.size()) > kMaxLibjuiceTurnServers) {
        turnServers.erase(turnServers.begin() + kMaxLibjuiceTurnServers, turnServers.end());
    }

    std::vector<rtc::IceServer> finalized;
    finalized.reserve(stunServers.size() + turnServers.size());
    finalized.insert(finalized.end(), stunServers.begin(), stunServers.end());
    finalized.insert(finalized.end(), turnServers.begin(), turnServers.end());
    return finalized;
}

} // namespace

TurnCredentialClient& TurnCredentialClient::instance()
{
    static TurnCredentialClient client;
    return client;
}

TurnCredentialClient::~TurnCredentialClient()
{
    if (QCoreApplication::instance() == nullptr) {
        m_reply = nullptr;
        m_network = nullptr;
        return;
    }

    if (m_reply != nullptr) {
        QObject::disconnect(m_reply, nullptr, nullptr, nullptr);
        m_reply->abort();
        m_reply = nullptr;
    }
    delete m_network;
    m_network = nullptr;
}

bool TurnCredentialClient::isConfigured() const
{
    return turnCredentialsAvailable() || !qgetenv("RMG_ICE_CONFIG_FILE").isEmpty();
}

void TurnCredentialClient::prefetch()
{
    if (!turnCredentialsAvailable() && qgetenv("RMG_ICE_CONFIG_FILE").isEmpty()) {
        qInfo() << "TurnCredentialClient: ICE broker prefetch skipped (broker disabled or not configured)";
        return;
    }

    if (loadIceConfigFile()) {
        return;
    }

    {
        QMutexLocker locker(&m_mutex);
        if (credentialsAreFreshLocked()) {
            qInfo() << "TurnCredentialClient: ICE credentials already cached; skipping prefetch";
            return;
        }
    }

    if (m_fetchInProgress) {
        qInfo() << "TurnCredentialClient: ICE credential fetch already in progress; skipping prefetch";
        return;
    }

    qInfo() << "TurnCredentialClient: Prefetching ICE credentials from" << netplayTurnIceServersUrl().toString();
    startBrokerRequest();
}

bool TurnCredentialClient::hasFreshCredentials() const
{
    QMutexLocker locker(&m_mutex);
    return credentialsAreFreshLocked();
}

bool TurnCredentialClient::isFetchInProgress() const
{
    return m_fetchInProgress;
}

bool TurnCredentialClient::ensureCredentials(int timeoutMs)
{
    if (!turnCredentialsAvailable() && qgetenv("RMG_ICE_CONFIG_FILE").isEmpty()) {
        return true;
    }

    if (loadIceConfigFile()) {
        return true;
    }

    if (timeoutMs <= 0) {
        timeoutMs = 45000;
    }

    QElapsedTimer timer;
    timer.start();
    int attempts = 0;

    while (timer.elapsed() < timeoutMs) {
        {
            QMutexLocker locker(&m_mutex);
            if (credentialsAreFreshLocked()) {
                return true;
            }
        }

        if (!m_fetchInProgress) {
            if (attempts >= kMaxBrokerAttempts) {
                break;
            }
            if (attempts > 0) {
                QEventLoop pause;
                QTimer::singleShot(2000, &pause, &QEventLoop::quit);
                pause.exec();
                if (timer.elapsed() >= timeoutMs) {
                    break;
                }
            }
            ++attempts;
            startBrokerRequest();
            if (!m_fetchInProgress) {
                break;
            }
        }

        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) {
            break;
        }

        const bool finished = waitForBroker(remaining);
        {
            QMutexLocker locker(&m_mutex);
            if (credentialsAreFreshLocked()) {
                return true;
            }
        }

        if (!finished && m_fetchInProgress) {
            qInfo() << "TurnCredentialClient: TURN credentials are still loading";
            return false;
        }
    }

    QMutexLocker locker(&m_mutex);
    const bool ready = credentialsAreFreshLocked();
    if (!ready) {
        qWarning() << "TurnCredentialClient: TURN credentials were not ready";
    }
    return ready;
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
    if (!m_expiresAt.isValid() ||
        m_expiresAt <= QDateTime::currentDateTimeUtc().addSecs(kCredentialRefreshSkewSeconds)) {
        return false;
    }

    // Broker-backed NAT traversal requires relay credentials, not just STUN discovery.
    if (turnCredentialsAvailable() || !qgetenv("RMG_ICE_CONFIG_FILE").isEmpty()) {
        return !m_turnServers.empty();
    }

    return !m_stunServers.empty();
}

bool TurnCredentialClient::loadIceConfigFile()
{
    const QByteArray configFilePath = qgetenv("RMG_ICE_CONFIG_FILE");
    if (configFilePath.isEmpty()) {
        return false;
    }

    QFile configFile(QString::fromUtf8(configFilePath));
    if (!configFile.open(QIODevice::ReadOnly)) {
        return false;
    }

    QString errorMessage;
    std::vector<rtc::IceServer> fetchedStunServers;
    std::vector<rtc::IceServer> fetchedTurnServers;
    if (!parseIceServersResponse(configFile.readAll(), &fetchedStunServers, &fetchedTurnServers, &errorMessage,
                                 nullptr)) {
        if (!errorMessage.isEmpty()) {
            qWarning() << "TurnCredentialClient:" << errorMessage;
        }
        return false;
    }

    QMutexLocker locker(&m_mutex);
    m_stunServers = std::move(fetchedStunServers);
    m_turnServers = std::move(fetchedTurnServers);
    m_expiresAt = QDateTime::currentDateTimeUtc().addSecs(kDefaultTurnCredentialCacheTtlSeconds);
    qInfo() << "TurnCredentialClient: Loaded" << m_stunServers.size() << "STUN and" << m_turnServers.size()
            << "TURN ICE servers from" << configFile.fileName();
    return true;
}

void TurnCredentialClient::startBrokerRequest()
{
    if (m_fetchInProgress || m_reply != nullptr) {
        return;
    }

    const QUrl brokerUrl = netplayTurnIceServersUrl();
    if (brokerUrl.isEmpty()) {
        qInfo() << "TurnCredentialClient: No ICE broker URL configured";
        return;
    }

    if (m_network == nullptr) {
        m_network = new QNetworkAccessManager();
    }

    QNetworkRequest request(brokerUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("RMG-Netplay/1.0"));
    request.setTransferTimeout(45000);

    qInfo() << "TurnCredentialClient: Fetching ICE credentials from" << brokerUrl.toString();
    m_fetchInProgress = true;
    m_reply = m_network->get(request);
    QObject::connect(m_reply, &QNetworkReply::finished, m_reply, [this]() {
        finishBrokerRequest();
    });
    if (m_reply != nullptr && m_reply->isFinished()) {
        finishBrokerRequest();
    }
}

void TurnCredentialClient::finishBrokerRequest()
{
    QNetworkReply* reply = m_reply;
    m_reply = nullptr;
    if (reply == nullptr) {
        m_fetchInProgress = false;
        return;
    }

    QString errorMessage;
    std::vector<rtc::IceServer> fetchedStunServers;
    std::vector<rtc::IceServer> fetchedTurnServers;
    qint64 expiresUnix = 0;
    bool success = false;

    if (reply->error() != QNetworkReply::NoError) {
        errorMessage = reply->errorString();
    } else {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode >= 200 && statusCode < 300) {
            success = parseIceServersResponse(reply->readAll(), &fetchedStunServers, &fetchedTurnServers,
                                              &errorMessage, &expiresUnix);
        } else {
            errorMessage = QStringLiteral("TURN broker returned HTTP %1").arg(statusCode);
        }
    }
    reply->deleteLater();

    QDateTime expiresAt;
    if (success) {
        if (expiresUnix > 0) {
            expiresAt = QDateTime::fromSecsSinceEpoch(expiresUnix, QTimeZone::UTC);
        } else {
            qWarning() << "TurnCredentialClient: broker did not report credential expiry; assuming"
                       << kDefaultTurnCredentialCacheTtlSeconds << "seconds";
            expiresAt = QDateTime::currentDateTimeUtc().addSecs(kDefaultTurnCredentialCacheTtlSeconds);
        }
        if (!expiresAt.isValid() ||
            expiresAt <= QDateTime::currentDateTimeUtc().addSecs(kCredentialRefreshSkewSeconds)) {
            success = false;
            errorMessage = QStringLiteral("broker credentials expire too soon");
        }
    }

    QMutexLocker locker(&m_mutex);
    m_fetchInProgress = false;
    if (!success) {
        if (!errorMessage.isEmpty()) {
            qWarning() << "TurnCredentialClient:" << errorMessage;
        }
        return;
    }

    m_stunServers = std::move(fetchedStunServers);
    m_turnServers = std::move(fetchedTurnServers);
    m_expiresAt = expiresAt;
    qInfo() << "TurnCredentialClient: Loaded" << m_stunServers.size() << "STUN and" << m_turnServers.size()
            << "TURN ICE servers from broker, expires" << m_expiresAt.toString(Qt::ISODate);
}

bool TurnCredentialClient::waitForBroker(int timeoutMs)
{
    if (m_reply == nullptr || m_reply->isFinished()) {
        return true;
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(m_reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    if (timeoutMs > 0) {
        timer.start(timeoutMs);
    }
    loop.exec();
    return m_reply == nullptr || m_reply->isFinished();
}

bool TurnCredentialClient::parseIceServersResponse(
    const QByteArray& payload,
    std::vector<rtc::IceServer>* stunOut,
    std::vector<rtc::IceServer>* turnOut,
    QString* errorOut,
    qint64* expiresUnixOut)
{
    if (!stunOut || !turnOut) {
        return false;
    }
    if (expiresUnixOut) {
        *expiresUnixOut = 0;
    }

    const QJsonDocument document = QJsonDocument::fromJson(payload);
    if (!document.isObject()) {
        if (errorOut) {
            *errorOut = QStringLiteral("TURN broker response was not valid JSON");
        }
        return false;
    }

    const QJsonObject root = document.object();
    const QJsonValue iceValue = root.value(QStringLiteral("iceServers"));
    QJsonArray iceServers;
    if (iceValue.isArray()) {
        iceServers = iceValue.toArray();
    } else if (iceValue.isObject()) {
        iceServers.append(iceValue);
    }

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
    if (expiresUnixOut) {
        *expiresUnixOut = brokerExpiryUnix(root, iceServers);
    }
    return true;
}

namespace UserInterface::Netplay {

std::vector<rtc::IceServer> buildIceServers()
{
    std::vector<rtc::IceServer> servers;

    TurnCredentialClient& iceClient = TurnCredentialClient::instance();
    if (turnCredentialsAvailable() || !qgetenv("RMG_ICE_CONFIG_FILE").isEmpty()) {
        if (!iceClient.hasFreshCredentials()) {
            iceClient.prefetch();
            qWarning() << "TurnCredentialClient: ICE credentials are still loading; STUN fallback only";
        }
        const std::vector<rtc::IceServer> brokerStunServers = iceClient.stunServers();
        servers.insert(servers.end(), brokerStunServers.begin(), brokerStunServers.end());
        const std::vector<rtc::IceServer> turnServers = iceClient.turnServers();
        servers.insert(servers.end(), turnServers.begin(), turnServers.end());
    }

    servers = finalizeIceServers(std::move(servers));

    for (const rtc::IceServer& server : servers) {
        if (server.type == rtc::IceServer::Type::Stun) {
            qInfo() << "ICE STUN server:" << QString::fromStdString(server.hostname) << server.port;
        } else {
            qInfo() << "ICE TURN server:" << QString::fromStdString(server.hostname) << server.port
                    << "(relay UDP, user" << !server.username.empty() << ")";
        }
    }

    if (servers.empty()) {
        qWarning() << "TurnCredentialClient: No ICE servers configured";
    }

    return servers;
}

} // namespace UserInterface::Netplay
