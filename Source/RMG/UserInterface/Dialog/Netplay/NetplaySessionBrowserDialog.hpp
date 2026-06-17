/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef NETPLAYSESSIONBROWSERDIALOG_HPP
#define NETPLAYSESSIONBROWSERDIALOG_HPP

#include <QJsonObject>
#include <QWidget>
#include <QString>
#include <memory>

#include "ui_NetplaySessionBrowserDialog.h"
#include "Netplay/NatTraversal/NatTraversalProtocol.hpp"
#include "Netplay/NatTraversal/NatTraversalClient.hpp"
#include "Netplay/NatTraversal/NatTraversalIndexClient.hpp"
#include "Netplay/NetplayCoordinator.hpp"

#include <RMG-Core/RomSettings.hpp>

class QNetworkAccessManager;
class QNetworkReply;

namespace UserInterface
{
namespace Dialog
{
class NetplaySessionBrowserDialog : public QWidget, private Ui::NetplaySessionBrowserDialog
{
    Q_OBJECT

  public:
    NetplaySessionBrowserDialog(QWidget *parent, Netplay::NetplayCoordinator* coordinator, const QMap<QString, CoreRomSettings>& data);
    ~NetplaySessionBrowserDialog(void);

    QJsonObject GetSessionJson(void);
    QString     GetSessionFile(void);

    void setEmbeddedMode(bool embedded);
    void setNickname(const QString& nickname);
    bool canSubmit(void) const;
    void submit(void);
    void refreshRoomList(void);

  signals:
    void sessionAccepted(void);
    void canSubmitChanged(bool canSubmit);

  private:
  	Netplay::NetplayCoordinator* coordinator;
    bool embeddedMode = false;
    QJsonObject sessionJson;
    QString sessionFile;
    QMap<QString, CoreRomSettings> romData;
    bool isWaitingForConnection;  // Track if we're waiting for server connection
    bool isResolvingHostCode = false;
    QString targetAddress;
    int targetPort = Netplay::kDefaultNetplayHostingPort;
    std::unique_ptr<Netplay::NatTraversalClient> natTraversalClient;
    std::unique_ptr<Netplay::NatTraversalIndexClient> natIndexClient;
    QNetworkAccessManager* networkManager = nullptr;

    QString pendingHostCode;
    QJsonObject pendingIndexSession;
    bool pendingIndexReady = false;
    bool pendingLookupReady = false;
    bool pendingLookupFailed = false;
    QString pendingLookupAddress;
    int pendingLookupPort = Netplay::kDefaultNetplayHostingPort;

    QString showROMDialog(QString name, QString md5);

    bool validate(void) const;
    void validateJoinButton(void);
    void connectToResolvedHost(const QString& address, int port);
    void beginHostCodeJoin(const QString& hostCode);
    void tryCompleteHostCodeJoin();

  private slots:
    void on_nickNameLineEdit_textChanged(void);
    void on_refreshPushButton_clicked(void);
    void onRoomsReplyFinished(QNetworkReply* reply);
    void on_sessionBrowserWidget_OnSessionChanged(bool valid);
    void onCoordinatorConnected(void);
    void onCoordinatorConnectionError(const QString& error);
    void onCoordinatorRoomJoined(const QString& roomId, int slot);

};
} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAYSESSIONBROWSERDIALOG_HPP
