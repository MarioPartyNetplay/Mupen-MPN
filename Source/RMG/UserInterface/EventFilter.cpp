/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "EventFilter.hpp"
#include "Utilities/QtKeyToSdl3Key.hpp"

#include <RMG-Core/Settings.hpp>

using namespace UserInterface;

namespace {
bool isFullscreenToggleEvent(const QKeyEvent* event)
{
    const QString binding = QString::fromStdString(
        CoreSettingsGetStringValue(SettingsID::KeyBinding_Fullscreen));
    return Utilities::QtKeyEventMatchesBinding(event, binding);
}
}

EventFilter::EventFilter(QObject *parent) : QObject(parent)
{
}

EventFilter::~EventFilter(void)
{
}

bool EventFilter::eventFilter(QObject *object, QEvent *event)
{
    switch (event->type())
    {
    case QEvent::Type::ShortcutOverride:
        // Keep Alt+Enter (or the configured fullscreen bind) out of the
        // emulator/OSD Alt-hotkey path so it can still toggle fullscreen.
        if (isFullscreenToggleEvent(static_cast<QKeyEvent*>(event))) {
            event->accept();
            return true;
        }
        break;
    case QEvent::Type::KeyPress:
        emit this->on_EventFilter_KeyPressed(static_cast<QKeyEvent*>(event));
        return true;
    case QEvent::Type::Drop:
        emit this->on_EventFilter_FileDropped(static_cast<QDropEvent*>(event));
        return true;
    case QEvent::Type::KeyRelease:
        emit this->on_EventFilter_KeyReleased(static_cast<QKeyEvent*>(event));
        return true;
    default:
        break;
    }

    return QObject::eventFilter(object, event);
}
