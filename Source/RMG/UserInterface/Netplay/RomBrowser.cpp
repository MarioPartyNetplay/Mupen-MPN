#include "RomBrowser.hpp"
#include <QVBoxLayout>
#include <QDir>
#include <QListWidgetItem>
#include <RMG-Core/Settings/Settings.hpp>
#include <RMG-Core/m64p/Api.hpp> // Include necessary header for ROM metadata
#include <QRegularExpression>

namespace UserInterface {
namespace Widget {

RomBrowser::RomBrowser(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);

    listWidget = new QListWidget(this);
    layout->addWidget(listWidget);

    connect(listWidget, &QListWidget::itemDoubleClicked, this, &RomBrowser::onItemDoubleClicked);

    setLayout(layout);

    loadRoms();
}

void RomBrowser::loadRoms()
{
    // Example directory containing ROMs
    QString romDirectory = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::RomBrowser_Directory));  

    QDir dir(romDirectory);
    QStringList filters;
    filters << "*.n64" << "*.z64" << "*.v64" << "*.rom" << "*.zip";
    romList = dir.entryList(filters, QDir::Files);

    for (const QString &rom : romList)
    {
        QString romPath = dir.absoluteFilePath(rom);
        QString displayName = getRomGoodName(romPath);

        if (displayName.isEmpty()) {
            // If good name is not available, remove the file extension
            displayName = rom;
            displayName.chop(displayName.length() - displayName.lastIndexOf('.'));
        }

        // Clean the display name by removing text within brackets and parentheses
        displayName = cleanRomName(displayName);

        QListWidgetItem *item = new QListWidgetItem(displayName, listWidget);
        item->setData(Qt::UserRole, romPath);
    }
}

QString RomBrowser::getRomGoodName(const QString &romPath)
{
    m64p_rom_settings romSettings;
    if (m64p::Core.DoCommand(M64CMD_ROM_GET_SETTINGS, sizeof(romSettings), &romSettings) == M64ERR_SUCCESS)
    {
        if (!QString::fromStdString(romSettings.goodname).isEmpty()) {
            return QString::fromStdString(romSettings.goodname);
        }
    }
    return QFileInfo(romPath).fileName();
}

QString RomBrowser::cleanRomName(const QString &name)
{
    // Remove anything in brackets or parentheses
    QRegularExpression re("\\s*\\([^\\)]*\\)|\\s*\\[[^\\]]*\\]");
    QString cleanedName = name;
    cleanedName.remove(re);
    return cleanedName.trimmed();
}

void RomBrowser::onItemDoubleClicked(QListWidgetItem *item)
{
    QString romPath = item->data(Qt::UserRole).toString();
    emit romDoubleClicked(romPath);
}

} // namespace Widget
} // namespace UserInterface