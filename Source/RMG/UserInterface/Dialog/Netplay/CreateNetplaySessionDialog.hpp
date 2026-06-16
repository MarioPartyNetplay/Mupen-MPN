/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CREATENETPLAYSESSIONDIALOG_HPP
#define CREATENETPLAYSESSIONDIALOG_HPP

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QCheckBox>
#include <QJsonObject>
#include <QTimerEvent>
#include <QWebSocket>
#include <QSpinBox>
#include <QUdpSocket>
#include <QDialog>
#include <QString>
#include <memory>

#include "ui_CreateNetplaySessionDialog.h"
#include "Netplay/NatTraversal/NatTraversalClient.hpp"
#include "Netplay/NatTraversal/NatTraversalIndexClient.hpp"

#include <RMG-Core/RomSettings.hpp>

// Forward declaration
namespace UserInterface::Netplay {
class NetplayCoordinator;
}

namespace UserInterface
{
namespace Dialog
{
class CreateNetplaySessionDialog : public QDialog, private Ui::CreateNetplaySessionDialog
{
    Q_OBJECT

  public:
    CreateNetplaySessionDialog(QWidget *parent, UserInterface::Netplay::NetplayCoordinator* coordinator, QMap<QString, CoreRomSettings> data);
    ~CreateNetplaySessionDialog(void);

    QJsonObject GetSessionJson(void);
    QString     GetSessionFile(void);

  private:
  	UserInterface::Netplay::NetplayCoordinator* coordinator;
    QUdpSocket broadcastSocket;
    QNetworkAccessManager networkManager;
    QNetworkReply* publicIpReply = nullptr;

    int pingTimerId = -1;
    int hostingPort = Netplay::kDefaultNetplayHostingPort;
    int publicIpTimeoutTimerId = -1;
    bool directConnection = false;
    
    QString publicIpAddress;  // Store the public IP
    QString pendingNatHostCode;  // Finish NAT publish/finalize after public IP lookup
    QCheckBox* directConnectionCheckBox = nullptr;
    QSpinBox* hostingPortSpinBox = nullptr;
    std::unique_ptr<UserInterface::Netplay::NatTraversalClient> natTraversalClient;
    std::unique_ptr<UserInterface::Netplay::NatTraversalIndexClient> natIndexClient;

  	QJsonObject sessionJson;
    QString sessionFile;
    QString sessionMD5;
    QString sessionGoodName;

    QString dispatcherUrl;

    QString getGameName(QString goodName, QString file);

    bool validate(void);
    void validateCreateButton(void);

    void createSession(void);
    void registerNatTraversalHost(void);
    void publishSessionIndex(const QString& hostCode);
    void fetchPublicIpAddress(void);
    void finalizeSession(void);

    void toggleUI(bool enable, bool enableCreateButton);

  protected:
    void timerEvent(QTimerEvent *event) Q_DECL_OVERRIDE;

  private slots:
  	void on_webSocket_textMessageReceived(QString message);
    void on_webSocket_pong(quint64 elapsedTime, const QByteArray&);
    void on_webSocket_connected(void);
    void on_broadcastSocket_readyRead(void);
    void on_publicIpFetch_Finished(QNetworkReply* reply);

    void on_nickNameLineEdit_textChanged(void);

    void on_romListWidget_OnRomChanged(bool valid);

  	void accept(void) Q_DECL_OVERRIDE;
};
} // namespace Dialog
} // namespace UserInterface

#endif // CREATENETPLAYSESSIONDIALOG_HPP
