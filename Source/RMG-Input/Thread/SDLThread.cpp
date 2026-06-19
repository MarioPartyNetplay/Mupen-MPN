/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "SDLThread.hpp"

using namespace Thread;

SDLThread::SDLThread(QObject *parent) : QThread(parent)
{

}

SDLThread::~SDLThread()
{
    if (this->isRunning())
    {
        this->StopLoop();
    }
}

void SDLThread::StopLoop(void)
{
    this->keepLoopRunning = false;
    while (this->isRunning())
    {
        // wait until we're not running anymore
    }
}

SDLThreadAction SDLThread::GetCurrentAction(void)
{
    return this->currentAction;
}

void SDLThread::SetAction(SDLThreadAction action)
{
    this->currentAction = action;
}

void SDLThread::run(void)
{
    // SDL event pumping for the config UI runs on the Qt main thread.
    // During gameplay, gamepad state is refreshed from GetKeys().
    while (this->keepLoopRunning)
    {
        QThread::msleep(10);
    }
}
