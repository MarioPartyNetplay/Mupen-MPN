/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "EmulationThread.hpp"
#include <RMG-Core/Core.hpp>

using namespace Thread;

EmulationThread::EmulationThread(QObject *parent) : QThread(parent)
{
}

EmulationThread::~EmulationThread(void)
{
}

void EmulationThread::SetRomFile(QString file)
{
    this->rom = file;
}

void EmulationThread::SetDiskFile(QString file)
{
    this->disk = file;
}

void EmulationThread::SetNetplay(QString ip, int port, int player)
{
    this->netplay_ip = ip;
    this->netplay_port = port;
    this->netplay_player = player;
}

void EmulationThread::run(void)
{
    emit this->on_Emulation_Started();

    bool ret = CoreStartEmulation(this->rom.toStdU32String(), this->disk.toStdU32String(), this->netplay_ip.toStdString(), this->netplay_port, this->netplay_player);
    if (!ret)
    {
        this->errorMessage = QString::fromStdString(CoreGetError());
    }

    emit this->on_Emulation_Finished(ret);
}

QString EmulationThread::GetLastError(void)
{
    return this->errorMessage;
}
