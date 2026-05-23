/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef NETPLAYSESSIONDIALOG_HPP
#define NETPLAYSESSIONDIALOG_HPP

#include <QJsonObject>
#include <QDialog>
#include <QString>

#include <RMG-Core/Cheats.hpp>
#include "Netplay/NetplayCoordinator.hpp"

#include "ui_NetplaySessionDialog.h"

namespace UserInterface
{
namespace Dialog
{
class NetplaySessionDialog : public QDialog, private Ui::NetplaySessionDialog
{
    Q_OBJECT

public:
    NetplaySessionDialog(QWidget *parent, Netplay::NetplayCoordinator* coordinator, QString sessionFile);
    ~NetplaySessionDialog(void);

private:
    QString sessionFile;
    QString romFile;
    int sessionSlot = -1;
    Netplay::NetplayCoordinator* coordinator;

    void syncHostSessionState(void);
    bool getCheats(std::vector<CoreCheat>& cheats, QJsonArray& cheatsArray);
    bool setCheats(const QJsonArray& cheatsArray);
    bool applyCheats(void);
    void updateCheatsTreeWidget(void);

private slots:
    void on_netplay_connected();
    void on_netplay_disconnected();
    void on_coordinator_stateChanged(Netplay::NetplayCoordinator::State state);
    void on_coordinator_playersUpdated(const QStringList& playerNames);
    void on_coordinator_gameStarted(int playerSlot);
    void on_coordinator_cheatsUpdated(const QJsonArray& cheats);
    void on_coordinator_saveSyncReceived(const QJsonArray& saveFiles);
    void on_coordinator_chatMessageReceived(const QString& playerName, const QString& message);
    void on_coordinator_motdReceived(const QString& message);
    void on_chatLineEdit_textChanged(const QString& text);
    void on_sendPushButton_clicked(void);
    
    void on_buttonBox_clicked(QAbstractButton* button);
    void accept(void) Q_DECL_OVERRIDE;
    void reject(void) Q_DECL_OVERRIDE;

protected:
    void showEvent(QShowEvent* event) override;

  signals:
    void OnPlayGame(QString file, QString address, int port, int player);
};
} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAYSESSIONDIALOG_HPP
