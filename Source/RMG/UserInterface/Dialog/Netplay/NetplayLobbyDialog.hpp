/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef NETPLAYLOBBYDIALOG_HPP
#define NETPLAYLOBBYDIALOG_HPP

#include <QDialog>
#include <QShowEvent>
#include <QString>

#include "ui_NetplayLobbyDialog.h"

#include <RMG-Core/RomSettings.hpp>

namespace UserInterface::Netplay {
class NetplayCoordinator;
}

namespace UserInterface
{
namespace Dialog
{
class CreateNetplaySessionDialog;
class NetplaySessionBrowserDialog;

class NetplayLobbyDialog : public QDialog, private Ui::NetplayLobbyDialog
{
    Q_OBJECT

  public:
    enum class InitialTab {
        Host = 0,
        Join = 1,
    };

    NetplayLobbyDialog(QWidget* parent, Netplay::NetplayCoordinator* coordinator,
                       const QMap<QString, CoreRomSettings>& romData,
                       InitialTab initialTab = InitialTab::Host);
    ~NetplayLobbyDialog(void);

    QString GetSessionFile(void);

  private:
    Netplay::NetplayCoordinator* coordinator;
    CreateNetplaySessionDialog* createPage = nullptr;
    NetplaySessionBrowserDialog* joinPage = nullptr;
    bool joinPageRefreshed = false;

    void syncNicknameToPages(void);
    void updateActionButton(void);

  protected:
    void showEvent(QShowEvent* event) Q_DECL_OVERRIDE;
  private slots:
    void on_nickNameLineEdit_textChanged(void);
    void on_tabWidget_currentChanged(int index);
    void onCreatePageCanSubmitChanged(bool canSubmit);
    void onJoinPageCanSubmitChanged(bool canSubmit);
    void onPageSessionAccepted(void);

    void accept(void) Q_DECL_OVERRIDE;

};
} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAYLOBBYDIALOG_HPP
