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
#include "OnScreenDisplay.hpp"

#include <QEvent>
#include <QGuiApplication>
#include <QMouseEvent>
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
    else if (object == this->widgetContainer)
    {
        switch (event->type())
        {
        case QEvent::MouseButtonPress:
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease:
        {
            QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
            const float scale       = static_cast<float>(this->devicePixelRatio());
            const float x           = static_cast<float>(mouseEvent->position().x()) * scale;
            const float y           = static_cast<float>(mouseEvent->position().y()) * scale;
            const Qt::KeyboardModifiers keyboardModifiers =
                mouseEvent->modifiers() | QGuiApplication::keyboardModifiers();
            const bool configureModifier = (keyboardModifiers & Qt::AltModifier) != 0;
            bool consumed = false;

            switch (event->type())
            {
            case QEvent::MouseButtonPress:
                if (mouseEvent->button() == Qt::LeftButton)
                {
                    consumed = OnScreenDisplayHandleMousePress(x, y, configureModifier);
                }
                break;
            case QEvent::MouseMove:
                consumed = OnScreenDisplayHandleMouseMove(x, y, configureModifier);
                break;
            case QEvent::MouseButtonRelease:
                if (mouseEvent->button() == Qt::LeftButton)
                {
                    consumed = OnScreenDisplayHandleMouseRelease();
                }
                break;
            default:
                break;
            }

            if (consumed || OnScreenDisplayIsDragging())
            {
                return true;
            }
            break;
        }
        default:
            break;
        }
    }

    return QWindow::eventFilter(object, event);
}

void VKWidget::resizeEvent(QResizeEvent *event)
{
    Q_UNUSED(event);
    // Resize is handled via the container event filter; QWindow resize events
    // can report a different size and cause redundant CoreSetVideoSize calls.
}

void VKWidget::queueVideoSizeUpdate(QSize size)
{
    if (!this->isVisible())
    {
        return;
    }

    const int newWidth  = static_cast<int>(size.width()  * this->devicePixelRatio()) & ~0x1;
    const int newHeight = static_cast<int>(size.height() * this->devicePixelRatio()) & ~0x1;

    if (newWidth == this->appliedWidth && newHeight == this->appliedHeight)
    {
        return;
    }

    if (newWidth == this->width && newHeight == this->height)
    {
        return;
    }

    this->width  = newWidth;
    this->height = newHeight;

    if (this->timerId != 0)
    {
        this->killTimer(this->timerId);
        this->timerId = 0;
    }

    this->timerId = this->startTimer(100);

    // Keep OSD anchored while user is actively resizing.
    OnScreenDisplaySetDisplaySize(this->width, this->height);
}

void VKWidget::timerEvent(QTimerEvent *event)
{
    Q_UNUSED(event);

    if (!this->isVisible())
    {
        this->killTimer(this->timerId);
        this->timerId = 0;
        return;
    }

    if (this->width == this->appliedWidth && this->height == this->appliedHeight)
    {
        this->killTimer(this->timerId);
        this->timerId = 0;
        return;
    }

    if (CoreSetVideoSize(this->width, this->height))
    {
        this->appliedWidth  = this->width;
        this->appliedHeight = this->height;
        this->killTimer(this->timerId);
        this->timerId = 0;
    }
}
