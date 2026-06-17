/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#ifndef NETPLAYINDEXCLIENT_HPP
#define NETPLAYINDEXCLIENT_HPP

#include <QObject>
#include <QNetworkAccessManager>

namespace UserInterface::Netplay {

/** HTTP client for publishing/fetching session metadata on the browse index. */
class NetplayIndexClient : public QObject {
    Q_OBJECT

public:
    explicit NetplayIndexClient(QObject* parent = nullptr);

    void publishSession(const QString& hostCode, const QByteArray& data);
    void fetchSession(const QString& hostCode);

signals:
    void published(const QString& key);
    void publishFailed(const QString& reason);
    void fetched(const QString& key, const QByteArray& data);
    void fetchFailed(const QString& reason);

private:
    QNetworkAccessManager m_networkManager;
};

} // namespace UserInterface::Netplay

#endif // NETPLAYINDEXCLIENT_HPP
