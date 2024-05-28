#include "EmulationThread.hpp"
#include <RMG-Core/Core.hpp>

#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>

using namespace Thread;

EmulationThread::EmulationThread(QObject *parent) : QThread(parent)
{
#ifndef _WIN32
    // on Linux, musl has a too small stack size,
    // causing a crash in mesa when using paraLLEl,
    // so to fix it, set a stack size of 2MiB for
    // the emulation thread
    // see https://github.com/Rosalie241/RMG/issues/219
    this->setStackSize(0x200000);
#endif
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

void EmulationThread::SetCheats(QJsonObject cheats)
{
    this->cheatsObject = cheats;
    CoreAddCallbackMessage(CoreDebugMessageType::Info, "Cheats set in SetCheats method");
}

void EmulationThread::run(void) {
    emit this->on_Emulation_Started();

    bool ret = CoreStartEmulation(this->rom.toStdU32String(), this->disk.toStdU32String(), this->netplay_ip.toStdString(), this->netplay_port, this->netplay_player);
    if (!ret) {
        this->errorMessage = QString::fromStdString(CoreGetError());
    } else {
        // Apply cheats after starting emulation
        CoreAddCallbackMessage(CoreDebugMessageType::Info, "Applying cheats after emulation start");
        ApplyCheats(this->cheatsObject);
    }

    emit this->on_Emulation_Finished(ret);
}

QString EmulationThread::GetLastError(void)
{
    return this->errorMessage;
}

void EmulationThread::ApplyCheats(QJsonObject cheatsObject) {
    CoreAddCallbackMessage(CoreDebugMessageType::Info, "Starting ApplyCheats");

    QJsonArray cheatsArray = cheatsObject["cheats"].toArray();
    std::vector<CoreCheat> cheats;

    for (const QJsonValue& value : cheatsArray) {
        QJsonObject cheatObject = value.toObject();
        CoreCheat cheat;
        cheat.Name = cheatObject["Name"].toString().toStdString();

        QJsonArray codesArray = cheatObject["Codes"].toArray();
        for (const QJsonValue& codeValue : codesArray) {
            QJsonObject codeObject = codeValue.toObject();
            CoreCheatCode code;

            // Debug messages to check the values being parsed
            QString addressStr = codeObject["Address"].toString();
            QString valueStr = codeObject["Value"].toString();
            CoreAddCallbackMessage(CoreDebugMessageType::Info, ("Parsing Address: " + addressStr).toStdString().c_str());
            CoreAddCallbackMessage(CoreDebugMessageType::Info, ("Parsing Value: " + valueStr).toStdString().c_str());

            bool ok;
            code.Address = addressStr.toUInt(&ok, 16);
            if (!ok) {
                CoreAddCallbackMessage(CoreDebugMessageType::Error, ("Failed to parse Address: " + addressStr).toStdString().c_str());
                continue;
            }

            code.Value = valueStr.toUInt(&ok, 16);
            if (!ok) {
                CoreAddCallbackMessage(CoreDebugMessageType::Error, ("Failed to parse Value: " + valueStr).toStdString().c_str());
                continue;
            }

            cheat.CheatCodes.push_back(code);
        }

        cheats.push_back(cheat);
    }

    CoreAddCallbackMessage(CoreDebugMessageType::Info, "Cheats to be applied:");
    for (const auto& cheat : cheats) {
        CoreAddCallbackMessage(CoreDebugMessageType::Info, ("Cheat: " + cheat.Name).c_str());
        for (const auto& code : cheat.CheatCodes) {
            CoreAddCallbackMessage(CoreDebugMessageType::Info, ("Address: " + std::to_string(code.Address) + " Value: " + std::to_string(code.Value)).c_str());
        }
    }

    CoreAddCallbackMessage(CoreDebugMessageType::Info, "Applying cheats...");
    if (CoreApplyCheatsRuntime(cheats)) {
        CoreAddCallbackMessage(CoreDebugMessageType::Info, "Cheats applied successfully");
    } else {
        CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to apply cheats");
    }
}