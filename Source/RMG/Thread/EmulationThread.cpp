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

void EmulationThread::run(void) {
    emit this->on_Emulation_Started();

    bool ret = CoreStartEmulation(this->rom.toStdU32String(), this->disk.toStdU32String(), this->netplay_ip.toStdString(), this->netplay_port, this->netplay_player);
    if (!ret) {
        this->errorMessage = QString::fromStdString(CoreGetError());
    } else {
        ApplyCheats(cheatsObject);
    }

    emit this->on_Emulation_Finished(ret);
}

QString EmulationThread::GetLastError(void)
{
    return this->errorMessage;
}

void EmulationThread::ApplyCheats(QJsonObject cheatsObject)
{    
    // Log the entire JSON object to verify its structure
    QJsonDocument cheatsDoc(cheatsObject);
    QString cheatsObjectString = cheatsDoc.toJson(QJsonDocument::Compact);

    if (cheatsObject.contains("custom") && cheatsObject.value("custom").isArray()) {
        QJsonArray customCheatsArray = cheatsObject.value("custom").toArray();

        CoreCheat cheat;
        cheat.Name = "Netplay"; // Set header name

        for (const QJsonValue &value : customCheatsArray) {
            QString cheatString = value.toString();

            // Parse the cheat code and value
            QStringList codeParts = cheatString.split(' '); // Remove '$' and split by space
            if (codeParts.size() == 2) {
                CoreCheatCode code;
                code.Address = codeParts[0].toUInt(nullptr, 16); // Convert address to unsigned int
                code.Value = codeParts[1].toUInt(nullptr, 16); // Convert value to unsigned int
                code.UseOptions = false; // No options for now
                code.OptionIndex = 0;
                code.OptionSize = 0;
                cheat.CheatCodes.push_back(code);
            }
        }
        if (!cheat.CheatCodes.empty()) {
            std::vector<CoreCheat> cheatsToApply;
            cheatsToApply.push_back(cheat); // Add the constructed cheat to the vector

            if (CoreApplyCheatsRuntime(cheatsToApply)) {
                CoreAddCallbackMessage(CoreDebugMessageType::Info, ("Netplay cheat added with " + QString::number(cheat.CheatCodes.size()) + " codes").toStdString().c_str());
            }
        }
    }
}