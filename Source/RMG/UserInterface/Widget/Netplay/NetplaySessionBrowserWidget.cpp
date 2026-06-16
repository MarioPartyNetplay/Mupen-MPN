/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "NetplaySessionBrowserWidget.hpp"
#include "UserInterface/NoFocusDelegate.hpp"

#include <QHeaderView>

Q_DECLARE_METATYPE(NetplaySessionData);

using namespace UserInterface::Widget;

//
// Exported Functions
//

NetplaySessionBrowserWidget::NetplaySessionBrowserWidget(QWidget* parent) : QStackedWidget(parent)
{
    qRegisterMetaType<NetplaySessionData>("NetplaySessionData");

    this->emptyWidget = new Widget::NetplaySessionBrowserEmptyWidget(this);
    this->addWidget(this->emptyWidget);

    this->loadingWidget = new Widget::NetplaySessionBrowserLoadingWidget(this, "Loading sessions");
    connect(this, &QStackedWidget::currentChanged, this->loadingWidget, &NetplaySessionBrowserLoadingWidget::on_NetplaySessionBrowserWidget_currentChanged);
    this->loadingWidget->SetWidgetIndex(this->addWidget(this->loadingWidget));

    this->tableWidget = new QTableWidget(this);
    this->tableWidget->setFrameStyle(QFrame::NoFrame);
    this->tableWidget->setItemDelegate(new NoFocusDelegate(this));
    this->tableWidget->setWordWrap(false);
    this->tableWidget->setShowGrid(false);
    this->tableWidget->setSortingEnabled(true);
    this->tableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->tableWidget->setSelectionBehavior(QTableView::SelectRows);
    this->tableWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    this->tableWidget->setVerticalScrollMode(QAbstractItemView::ScrollMode::ScrollPerPixel);
    this->tableWidget->verticalHeader()->hide();
    this->tableWidget->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    this->tableWidget->horizontalHeader()->setSectionsMovable(true);
    this->tableWidget->horizontalHeader()->setFirstSectionMovable(true);
    this->tableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    this->tableWidget->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft);
    this->tableWidget->horizontalHeader()->setSortIndicatorShown(false);
    this->tableWidget->horizontalHeader()->setHighlightSections(false);
    this->tableWidget->horizontalHeader()->setStretchLastSection(true);
    this->addWidget(this->tableWidget);
    connect(this->tableWidget, &QTableWidget::currentItemChanged, this, &NetplaySessionBrowserWidget::on_tableWidget_currentItemChanged);

    QStringList labels;
    labels << "Host";
    labels << "Game";
    labels << "Players";
    this->tableWidget->setColumnCount(labels.size());
    this->tableWidget->setHorizontalHeaderLabels(labels);

    this->setCurrentWidget(this->loadingWidget);
}

NetplaySessionBrowserWidget::~NetplaySessionBrowserWidget()
{
}

void NetplaySessionBrowserWidget::Reset(void)
{
    this->setCurrentWidget(this->emptyWidget);
    this->tableWidget->model()->removeRows(0, this->tableWidget->rowCount());
}

void NetplaySessionBrowserWidget::StartRefresh(void)
{
    this->refreshTimer.start();
    this->setCurrentWidget(this->loadingWidget);
    this->tableWidget->model()->removeRows(0, this->tableWidget->rowCount());
}

void NetplaySessionBrowserWidget::AddSessionData(const QString& hostName, const QString& gameName,
                                                 const QString& hostCode, const QString& lobbySize,
                                                 int port, const QString& address)
{
    const NetplaySessionData sessionData =
    {
        hostName,
        gameName,
        hostCode,
        lobbySize,
        address,
        port,
    };

    int row = this->tableWidget->rowCount();
    this->tableWidget->insertRow(row);

    QTableWidgetItem* hostItem = new QTableWidgetItem(hostName);
    hostItem->setData(Qt::UserRole, QVariant::fromValue<NetplaySessionData>(sessionData));
    this->tableWidget->setItem(row, 0, hostItem);

    QTableWidgetItem* gameItem = new QTableWidgetItem(gameName);
    this->tableWidget->setItem(row, 1, gameItem);

    QTableWidgetItem* playersItem = new QTableWidgetItem(lobbySize);
    this->tableWidget->setItem(row, 2, playersItem);
}

void NetplaySessionBrowserWidget::RefreshDone(void)
{
    if (this->tableWidget->rowCount() == 0)
    {
        this->currentViewWidget = this->emptyWidget;
    }
    else
    {
        this->currentViewWidget = this->tableWidget;
    }

    qint64 elapsedTime = this->refreshTimer.elapsed();
    if (elapsedTime < 300)
    {
        this->showViewWidgetTimerId = this->startTimer(300 - elapsedTime);
        return;
    }

    this->setCurrentWidget(this->currentViewWidget);
    emit this->OnRefreshDone();
}

bool NetplaySessionBrowserWidget::IsCurrentSessionValid()
{
    return this->currentWidget() == this->tableWidget &&
            this->tableWidget->currentItem() != nullptr;
}

bool NetplaySessionBrowserWidget::GetCurrentSession(NetplaySessionData& data)
{
    if (!this->IsCurrentSessionValid())
    {
        return false;
    }

    QTableWidgetItem* item = this->tableWidget->item(this->tableWidget->currentRow(), 0);
    if (item == nullptr)
    {
        return false;
    }

    data = item->data(Qt::UserRole).value<NetplaySessionData>();
    return true;
}

void NetplaySessionBrowserWidget::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == this->showViewWidgetTimerId)
    {
        this->killTimer(this->showViewWidgetTimerId);
        this->showViewWidgetTimerId = -1;

        this->setCurrentWidget(this->currentViewWidget);
        emit this->OnRefreshDone();
    }
}

void NetplaySessionBrowserWidget::on_tableWidget_currentItemChanged(QTableWidgetItem* current, QTableWidgetItem* previous)
{
    Q_UNUSED(previous);

    if (this->currentWidget() == this->tableWidget)
    {
        emit this->OnSessionChanged(current != nullptr);
    }
}
