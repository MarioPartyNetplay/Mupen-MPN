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

#include <QCheckBox>
#include <QJsonObject>
#include <QTimerEvent>
#include <QSpinBox>
#include <QUdpSocket>
#include <QWidget>
#include <QString>

#include "ui_CreateNetplaySessionDialog.h"
#include "Netplay/NetplayProtocol.hpp"

#include <RMG-Core/RomSettings.hpp>

// Forward declaration
namespace UserInterface::Netplay {
class NetplayCoordinator;
}

namespace UserInterface
{
namespace Dialog
{
class CreateNetplaySessionDialog : public QWidget, private Ui::CreateNetplaySessionDialog
{
    Q_OBJECT

  public:
    CreateNetplaySessionDialog(QWidget *parent, UserInterface::Netplay::NetplayCoordinator* coordinator, const QMap<QString, CoreRomSettings>& data);
    ~CreateNetplaySessionDialog(void);

    QJsonObject GetSessionJson(void);
    QString     GetSessionFile(void);

    void setEmbeddedMode(bool embedded);
    void setNickname(const QString& nickname);
    bool canSubmit(void) const;
    void submit(void);

  signals:
    void sessionAccepted(void);
    void canSubmitChanged(bool canSubmit);

  private:
  	UserInterface::Netplay::NetplayCoordinator* coordinator;
    bool embeddedMode = false;
    QUdpSocket broadcastSocket;

    int pingTimerId = -1;
    int hostingPort = Netplay::kDefaultNetplayHostingPort;

    QCheckBox* showInBrowserCheckBox = nullptr;
    QCheckBox* useUpnpCheckBox = nullptr;
    QSpinBox* hostingPortSpinBox = nullptr;

  	QJsonObject sessionJson;
    QString sessionFile;
    QString sessionMD5;
    QString sessionGoodName;

    QString dispatcherUrl;

    QString getGameName(QString goodName, QString file);

    bool validate(void) const;
    void validateCreateButton(void);

    void createSession(void);
    void finalizeSession(void);
    void updateConnectionModeUi(void);

    void toggleUI(bool enable, bool enableCreateButton);

  protected:
    void timerEvent(QTimerEvent *event) Q_DECL_OVERRIDE;

  private slots:
    void on_broadcastSocket_readyRead(void);

    void on_nickNameLineEdit_textChanged(void);

    void on_romListWidget_OnRomChanged(bool valid);
    void on_connectionModeComboBox_currentIndexChanged(int index);
};
} // namespace Dialog
} // namespace UserInterface

#endif // CREATENETPLAYSESSIONDIALOG_HPP
