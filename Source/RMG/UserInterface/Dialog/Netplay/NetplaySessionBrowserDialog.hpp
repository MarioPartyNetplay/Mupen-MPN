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

#include <QTableWidgetItem>
#include <QNetworkReply>
#include <QTimerEvent>
#include <QJsonObject>
#include <QDialog>
#include <QString>
#include <memory>

#include "ui_NetplaySessionBrowserDialog.h"
#include "Netplay/NatTraversal/NatTraversalProtocol.hpp"
#include "Netplay/NatTraversal/NatTraversalClient.hpp"
#include "Netplay/NatTraversal/NatTraversalIndexClient.hpp"
#include "Netplay/NetplayCoordinator.hpp"

#include <RMG-Core/RomSettings.hpp>

namespace UserInterface
{
namespace Dialog
{
class NetplaySessionBrowserDialog : public QDialog, private Ui::NetplaySessionBrowserDialog
{
    Q_OBJECT

  public:
    NetplaySessionBrowserDialog(QWidget *parent, Netplay::NetplayCoordinator* coordinator, QMap<QString, CoreRomSettings> data);
    ~NetplaySessionBrowserDialog(void);

    QJsonObject GetSessionJson(void);
    QString     GetSessionFile(void);

  private:
  	Netplay::NetplayCoordinator* coordinator;
    QJsonObject sessionJson;
    QString sessionFile;
    QMap<QString, CoreRomSettings> romData;
    bool isWaitingForConnection;  // Track if we're waiting for server connection
    bool isResolvingHostCode = false;
    QString targetAddress;
    int targetPort = Netplay::kDefaultNetplayHostingPort;
    std::unique_ptr<Netplay::NatTraversalClient> natTraversalClient;
    std::unique_ptr<Netplay::NatTraversalIndexClient> natIndexClient;

    QString pendingHostCode;
    QJsonObject pendingIndexSession;
    bool pendingIndexReady = false;
    bool pendingLookupReady = false;
    bool pendingLookupFailed = false;
    QString pendingLookupAddress;
    int pendingLookupPort = Netplay::kDefaultNetplayHostingPort;

    QString showROMDialog(QString name, QString md5);

    bool validate(void);
    void validateJoinButton(void);
    void connectToResolvedHost(const QString& address, int port);
    void beginHostCodeJoin(const QString& hostCode);
    void tryCompleteHostCodeJoin();

  private slots:
    void on_nickNameLineEdit_textChanged(void);
    void onCoordinatorConnected(void);
    void onCoordinatorConnectionError(const QString& error);
    void onCoordinatorRoomJoined(const QString& roomId, int slot);

    void accept(void) Q_DECL_OVERRIDE;

};
} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAYSESSIONBROWSERDIALOG_HPP
