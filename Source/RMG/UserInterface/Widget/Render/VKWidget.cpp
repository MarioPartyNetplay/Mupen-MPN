/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "VKWidget.hpp"

#include <QEvent>
#include <QGuiApplication>
#include <QResizeEvent>

#include <RMG-Core/Video.hpp>

using namespace UserInterface::Widget;

VKWidget::VKWidget(QWidget *parent)
{
    this->widgetContainer = QWidget::createWindowContainer(this, parent);
    this->widgetContainer->installEventFilter(this);
    this->setSurfaceType(QWindow::VulkanSurface);
}

VKWidget::~VKWidget(void)
{
}

void VKWidget::SetHideCursor(bool hide)
{
    this->setCursor(hide ? Qt::BlankCursor : Qt::ArrowCursor);
}

QWidget* VKWidget::GetWidget(void)
{
    return this->widgetContainer;
}

bool VKWidget::eventFilter(QObject *object, QEvent *event)
{
    if (object == this->widgetContainer && event->type() == QEvent::Resize)
    {
        this->queueVideoSizeUpdate(static_cast<QResizeEvent *>(event)->size());
    }

    return QWindow::eventFilter(object, event);
}

void VKWidget::resizeEvent(QResizeEvent *event)
{
    this->queueVideoSizeUpdate(event->size());
}

void VKWidget::queueVideoSizeUpdate(QSize size)
{
    if (!this->isVisible())
    {
        return;
    }

    if (this->timerId != 0)
    {
        this->killTimer(this->timerId);
        this->timerId = 0;
    }

    this->timerId = this->startTimer(100);

    // account for HiDPI scaling
    // see https://github.com/Rosalie241/RMG/issues/2
    this->width  = size.width() * this->devicePixelRatio();
    this->height = size.height() * this->devicePixelRatio();

    this->width  &= ~0x1;
    this->height &= ~0x1;
}

void VKWidget::timerEvent(QTimerEvent *event)
{
    if (!this->isVisible())
    {
        this->killTimer(this->timerId);
        this->timerId = 0;
        return;
    }

    // only remove current timer
    // when setting the video size succeeds
    if (CoreSetVideoSize(this->width, this->height))
    {
        this->killTimer(this->timerId);
        this->timerId = 0;
    }
}
