/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
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
#include <QJsonObject>
#include <QWebSocket>
#include <QUdpSocket>
#include <QDialog>
#include <QString>

#include "ui_NetplaySessionBrowserDialog.h"

#include <RMG-Core/Core.hpp>

namespace UserInterface
{
namespace Dialog
{
class NetplaySessionBrowserDialog : public QDialog, private Ui::NetplaySessionBrowserDialog
{
    Q_OBJECT

  public:
    NetplaySessionBrowserDialog(QWidget *parent, QWebSocket* webSocket, QMap<QString, CoreRomSettings> data);
    ~NetplaySessionBrowserDialog(void);

    QJsonObject GetSessionJson(void);
    QString     GetSessionFile(void);

  private:
  	QWebSocket* webSocket;
    QUdpSocket broadcastSocket;
    QJsonObject sessionJson;
    QString sessionFile;
    QMap<QString, CoreRomSettings> romData;

    void showErrorMessage(QString error, QString details);
    QString showROMDialog(QString name, QString md5);

    bool validate(void);
    void validateJoinButton(void);

    void addSessionData(QString name, QString game, QString md5, bool password, int port, 
                        QString cpuEmulator, QString rspPlugin, QString gfxPlugin);

  private slots:
    void on_webSocket_connected(void);
    void on_webSocket_textMessageReceived(QString message);
    void on_broadcastSocket_readyRead(void);
    void on_networkAccessManager_Finished(QNetworkReply* reply);

    void on_serverComboBox_currentIndexChanged(int index);
    void on_tableWidget_currentItemChanged(QTableWidgetItem* current, QTableWidgetItem* previous);

    void on_nickNameLineEdit_textChanged(void);

    void accept(void) Q_DECL_OVERRIDE;

};
} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAYSESSIONBROWSERDIALOG_HPP