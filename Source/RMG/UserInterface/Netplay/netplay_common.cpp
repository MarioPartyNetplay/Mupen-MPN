#include "netplay_common.h"

#include <RMG-Core/m64p/Api.hpp>

#include <QProcess>
#include <QFile>

m64p_error loadROM(QString filename)
{
    char *ROM_buffer = NULL;
    size_t romlength = 0;

    if (filename.endsWith(".7z") || filename.endsWith(".zip") || filename.endsWith(".ZIP"))
    {
        QProcess process;
        process.setProgram("7za");
        QStringList arguments = { "e", "-so", filename, "*64"  };
        process.setArguments(arguments);
        process.start();
        process.waitForFinished(-1);
        QByteArray data = process.readAllStandardOutput();
        romlength = data.size();
        if (romlength == 0)
        {
            printf("couldn't open file '%s' for reading.", filename.toUtf8().constData());
            return M64ERR_INVALID_STATE;
        }
        ROM_buffer = (char *) malloc(romlength);
        memcpy(ROM_buffer, data.constData(), romlength);
    }
    else
    {
        /* load ROM image */
        QFile file(filename);
        if (!file.open(QIODevice::ReadOnly))
        {
            printf("couldn't open ROM file '%s' for reading.", filename.toUtf8().constData());
            return M64ERR_INVALID_STATE;
        }

        romlength = file.size();
        QDataStream in(&file);
        ROM_buffer = (char *) malloc(romlength);
        if (in.readRawData(ROM_buffer, romlength) == -1)
        {
            printf("couldn't read %li bytes from ROM image file '%s'.", romlength, filename.toUtf8().constData());
            free(ROM_buffer);
            file.close();
            return M64ERR_INVALID_STATE;
        }
        file.close();
    }

    /* Try to load the ROM image into the core */
    if (m64p::Core.DoCommand(M64CMD_ROM_OPEN, (int) romlength, ROM_buffer) != M64ERR_SUCCESS)
    {
        printf("core failed to open ROM image file '%s'.", filename.toUtf8().constData());
        free(ROM_buffer);
        return M64ERR_INVALID_STATE;
    }
    free(ROM_buffer); /* the core copies the ROM image, so we can release this buffer immediately */

    return M64ERR_SUCCESS;
}
