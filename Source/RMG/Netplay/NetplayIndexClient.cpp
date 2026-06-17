/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "NetplayIndexClient.hpp"
#include "NetplayProtocol.hpp"

#include <QNetworkReply>
#include <QNetworkRequest>

namespace UserInterface::Netplay {

NetplayIndexClient::NetplayIndexClient(QObject* parent)
    : QObject(parent)
{
}

void NetplayIndexClient::publishSession(const QString& hostCode, const QByteArray& data)
{
    const QString key = sessionIndexKey(hostCode);
    const QUrl url = netplaySessionIndexPutUrl(key);
    if (url.isEmpty()) {
        emit publishFailed(QStringLiteral("Invalid session index key"));
        return;
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(10000);

    QNetworkReply* reply = m_networkManager.put(request, data);
    connect(reply, &QNetworkReply::finished, this, [this, reply, key]() {
        if (reply->error() != QNetworkReply::NoError) {
            emit publishFailed(reply->errorString());
        } else {
            emit published(key);
        }
        reply->deleteLater();
    });
}

void NetplayIndexClient::fetchSession(const QString& hostCode)
{
    const QString key = sessionIndexKey(hostCode);
    const QUrl url = netplaySessionIndexUrl(hostCode);
    if (url.isEmpty()) {
        emit fetchFailed(QStringLiteral("Invalid session index key"));
        return;
    }

    QNetworkRequest request(url);
    request.setTransferTimeout(5000);

    QNetworkReply* reply = m_networkManager.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, key]() {
        if (reply->error() != QNetworkReply::NoError) {
            emit fetchFailed(reply->errorString());
        } else {
            emit fetched(key, reply->readAll());
        }
        reply->deleteLater();
    });
}

} // namespace UserInterface::Netplay
