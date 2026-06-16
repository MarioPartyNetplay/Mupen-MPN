/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef NETPLAYSESSIONBROWSERWIDGET_HPP
#define NETPLAYSESSIONBROWSERWIDGET_HPP

#include "NetplaySessionBrowserEmptyWidget.hpp"
#include "NetplaySessionBrowserLoadingWidget.hpp"

#include <QTableWidgetItem>
#include <QStackedWidget>
#include <QTableWidget>

struct NetplaySessionData
{
    QString HostName;
    QString GameName;
    QString HostCode;
    QString LobbySize;
    QString Address;
    int Port = 0;
};

namespace UserInterface
{
namespace Widget
{
class NetplaySessionBrowserWidget : public QStackedWidget
{
    Q_OBJECT

  public:
    NetplaySessionBrowserWidget(QWidget *);
    ~NetplaySessionBrowserWidget(void);

    void Reset(void);

    void StartRefresh(void);

    void AddSessionData(const QString& hostName, const QString& gameName, const QString& hostCode,
                        const QString& lobbySize, int port, const QString& address);

    void RefreshDone(void);

    bool IsCurrentSessionValid(void);
    bool GetCurrentSession(NetplaySessionData& data);

  private:
    Widget::NetplaySessionBrowserEmptyWidget*   emptyWidget   = nullptr;
    Widget::NetplaySessionBrowserLoadingWidget* loadingWidget = nullptr;

    QTableWidget* tableWidget       = nullptr;
    QWidget*      currentViewWidget = nullptr;

    int showViewWidgetTimerId = -1;
    QElapsedTimer refreshTimer;

  protected:
    void timerEvent(QTimerEvent *event) Q_DECL_OVERRIDE;

  private slots:
    void on_tableWidget_currentItemChanged(QTableWidgetItem* current, QTableWidgetItem* previous);

  signals:
    void OnRefreshDone(void);
    void OnSessionChanged(bool valid);

};
} // namespace Widget
} // namespace UserInterface

#endif // NETPLAYSESSIONBROWSERWIDGET_HPP
