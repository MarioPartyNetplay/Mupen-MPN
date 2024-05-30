#ifndef ROMBROWSERMODELDATA_HPP
#define ROMBROWSERMODELDATA_HPP
#include <QString>
#include <QMetaType>

#include <RMG-Core/Core.hpp> // Assuming CoreRomType, CoreRomHeader, CoreRomSettings are defined here
struct RomBrowserModelData
{
    QString         file;
    CoreRomType     type;
    CoreRomHeader   header;
    CoreRomSettings settings;
    QString         coverFile;
    RomBrowserModelData(QString file, CoreRomType type, CoreRomHeader header, CoreRomSettings settings)
    {
        this->file = file;
        this->type = type;
        this->header = header;
        this->settings = settings;
    }

    RomBrowserModelData() = default;

};

Q_DECLARE_METATYPE(RomBrowserModelData)

#endif // ROMBROWSERMODELDATA_HPP