#include "RomBrowser.hpp"
#include <QVBoxLayout>
#include <QDir>
#include <QListWidgetItem>
#include <RMG-Core/Settings/Settings.hpp>
#include <RMG-Core/m64p/Api.hpp> // Include necessary header for ROM metadata
#include <QRegularExpression>
#include <RMG-Core/Core.hpp> // For CoreAddCallbackMessage

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
    QString romDirectory = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::RomBrowser_Directory));
    CoreAddCallbackMessage(CoreDebugMessageType::Info, ("ROM Directory Path: " + romDirectory.toStdString()).c_str());

    QDir dir(romDirectory);
    if (!dir.exists()) {
        CoreAddCallbackMessage(CoreDebugMessageType::Info, ("Directory does not exist: " + romDirectory.toStdString()).c_str());
        return;
    }

    QStringList filters;
    filters << "*.n64" << "*.z64" << "*.v64" << "*.rom" << "*.zip";
    QStringList romList = dir.entryList(filters, QDir::Files);
    CoreAddCallbackMessage(CoreDebugMessageType::Info, ("ROM List: " + romList.join(", ").toStdString()).c_str());

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

        CoreAddCallbackMessage(CoreDebugMessageType::Info, ("Adding ROM: " + displayName.toStdString() + " with path: " + romPath.toStdString()).c_str());

        QListWidgetItem *item = new QListWidgetItem(displayName, listWidget);
        item->setData(Qt::UserRole, romPath);
    }
}

QString RomBrowser::getRomGoodName(const QString &romPath)
{
    m64p_rom_settings romSettings;
    QString fallbackName = QFileInfo(romPath).fileName();
    QString cleanedFallbackName = cleanRomName(fallbackName);
    return cleanedFallbackName;
}

QString RomBrowser::cleanRomName(const QString &name)
{
    // Remove the file extension
    QString nameWithoutExtension = name.left(name.lastIndexOf('.'));

    // Remove anything in brackets or parentheses
    QRegularExpression re("\\s*\\([^\\)]*\\)|\\s*\\[[^\\]]*\\]");
    QString cleanedName = nameWithoutExtension;
    cleanedName.remove(re);
    return cleanedName.trimmed();
}

void RomBrowser::onItemDoubleClicked(QListWidgetItem *item)
{
    QString romPath = item->data(Qt::UserRole).toString();
    CoreAddCallbackMessage(CoreDebugMessageType::Info, ("ROM double-clicked: " + romPath.toStdString()).c_str());
    emit romDoubleClicked(romPath);
}

} // namespace Widget
} // namespace UserInterface