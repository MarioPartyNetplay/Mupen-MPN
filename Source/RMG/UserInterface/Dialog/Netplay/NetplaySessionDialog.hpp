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
#include <QPair>
#include <memory>

#include <QCloseEvent>
#include <QEvent>

#include <QNetworkAccessManager>

#include <RMG-Core/Cheats.hpp>
#include "Netplay/NetplayCoordinator.hpp"
#include "Netplay/NetplayHostRegistry.hpp"
#include "Netplay/NetplayIndexClient.hpp"

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

    void shutdownSession();

private:
    QString sessionFile;
    QJsonObject sessionJson;
    QString romFile;
    int sessionSlot = -1;
    Netplay::NetplayCoordinator* coordinator;
    std::unique_ptr<Netplay::NetplayHostRegistry> hostRegistry;
    std::unique_ptr<Netplay::NetplayIndexClient> indexClient;
    QNetworkAccessManager networkManager;
    QString publicIpAddress;

    bool m_sessionShutdown = false;
    bool m_pendingGameStart = false;
    bool m_emulationBeginReceived = false;
    bool m_sessionSavesApplied = false;
    bool m_sessionCoreSettingsApplied = false;
    int m_pendingPlayerSlot = 0;
    int m_lastDisplayedBufferDelay = -1;
    int m_clientSessionPrepWatchdogTimerId = -1;
    int m_clientSessionPrepRetries = 0;
    int m_hostSessionSyncRetryTimerId = -1;
    int m_hostSessionSyncRetries = 0;

    QList<Netplay::SocketIOClient::PlayerInfo> m_cachedPlayers;
    QStringList m_publishedSessionPlayerIds;
    bool m_updatingPlayerList = false;

    void syncHostSessionState(void);
    void beginHostBrowserRegistration(uint16_t hostingPort, bool listInBrowser);
    void updateConnectInfoDisplay(void);
    void fetchPublicIpAddress(void);
    void publishHostSessionIndex(bool started);
    void refreshPlayersListWidget(void);
    void tryStartPendingGame(void);
    void tryCompletePendingGameStart(void);
    void requestSynchronizedEmulationStart(void);
    bool sessionPrepReadyForStart(void) const;
    void scheduleHostSessionSyncRetries(void);
    bool getCheats(std::vector<CoreCheat>& cheats, QJsonArray& cheatsArray);
    bool setCheats(const QJsonArray& cheatsArray);
    bool applyCheats(void);
    void updateCheatsTreeWidget(void);
    bool isLocalSessionHost(void) const;
    void applyHostOnlyControlsVisibility(void);
    void updateLobbyActionButtons(void);
    void setLayoutWidgetsVisible(QLayout* layout, bool visible);
    void openInputConfiguration(void);
    void openAssignPortsDialog(void);
    bool selectGameFromRomList(QString* romPathOut, QString* gameNameOut, QString* md5Out);
    QString promptMatchingRom(const QString& gameName, const QString& md5);
    void applyLocalSessionGame(const QString& romPath, const QString& gameName, const QString& md5);
    void updateChangeGameButton(const QString& gameName);
    void appendGameChangedSystemMessage(const QString& gameName);
    QString expectedSessionMd5(void) const;
    bool isHostClientId(const QString& clientId) const;

private slots:
    void on_netplay_connected();
    void on_netplay_disconnected();
    void on_coordinator_stateChanged(Netplay::NetplayCoordinator::State state);
    void on_coordinator_playersUpdated(const QList<Netplay::SocketIOClient::PlayerInfo>& players);
    void on_coordinator_gameStarted(int playerSlot);
    void on_coordinator_cheatsUpdated(const QJsonArray& cheats);
    void on_coordinator_saveSyncReceived(const QJsonArray& saveFiles);
    void on_coordinator_coreSettingsSyncReceived(const QJsonObject& coreSettings);
    void on_coordinator_chatMessageReceived(const QString& playerName, const QString& message);
    void on_coordinator_motdReceived(const QString& message);
    void on_chatLineEdit_textChanged(const QString& text);
    void on_sendPushButton_clicked(void);
    void on_changeGamePushButton_clicked(void);
    void on_kickPlayerPushButton_clicked(void);
    void on_assignPortsPushButton_clicked(void);
    void on_copyRoomIdPushButton_clicked(void);
    void on_startPushButton_clicked(void);
    void on_quitPushButton_clicked(void);
    void on_cheatsPushButton_clicked(void);
    void on_coordinator_playerKicked(const QString& reason);
    void on_coordinator_sessionGameChanged(const QString& gameName, const QString& md5);
    void accept(void) Q_DECL_OVERRIDE;
    void reject(void) Q_DECL_OVERRIDE;

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void timerEvent(QTimerEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    bool isKeyboardCaptureWidget(QWidget* widget) const;

  signals:
    void OnPlayGame(QString file, QString address, int port, int player);
};
} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAYSESSIONDIALOG_HPP
