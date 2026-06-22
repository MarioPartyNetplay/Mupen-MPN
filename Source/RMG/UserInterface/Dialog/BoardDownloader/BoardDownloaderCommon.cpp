/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "BoardDownloaderCommon.hpp"

#include <RMG-Core/CachedRomHeaderAndSettings.hpp>
#include <RMG-Core/Directories.hpp>
#include <RMG-Core/Settings.hpp>

#include <QDate>
#include <QDirIterator>

namespace UserInterface
{
namespace Dialog
{

namespace
{

bool containsTitle(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

bool containsTitle(const QString& haystack, const char* needle)
{
    return haystack.contains(QString::fromUtf8(needle), Qt::CaseInsensitive);
}

} // namespace

QString boardDownloaderApiBaseUrl(void)
{
    return QStringLiteral("https://ppapi.tabs.gay");
}

QString boardDownloaderConfigDirectory(void)
{
    std::filesystem::path directory = CoreGetUserConfigDirectory();
    directory /= "BoardDownloaderMPN";
    return QString::fromStdString(directory.string());
}

bool isMarioParty3(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    return header.GameID == "NMVE" ||
           containsTitle(settings.GoodName, "MarioParty3") ||
           containsTitle(settings.GoodName, "Mario Party 3") ||
           containsTitle(settings.InternalName, "MARIO PARTY 3");
}

bool isMarioParty2(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    return containsTitle(settings.GoodName, "MarioParty2") ||
           containsTitle(settings.GoodName, "Mario Party 2") ||
           containsTitle(settings.InternalName, "MARIO PARTY 2") ||
           header.GameID == "NMWE";
}

bool isMarioParty1(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    if (isMarioParty3(header, settings) || isMarioParty2(header, settings))
    {
        return false;
    }

    return containsTitle(settings.GoodName, "MarioParty") ||
           containsTitle(settings.GoodName, "Mario Party") ||
           containsTitle(settings.InternalName, "MARIO PARTY") ||
           header.GameID == "CLBE";
}

MarioPartyTarget marioPartyTargetForRom(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    if (isMarioParty3(header, settings))
    {
        return MarioPartyTarget::MarioParty3;
    }

    if (isMarioParty2(header, settings))
    {
        return MarioPartyTarget::MarioParty2;
    }

    if (isMarioParty1(header, settings))
    {
        return MarioPartyTarget::MarioParty1;
    }

    return MarioPartyTarget::Unknown;
}

MarioPartyTarget marioPartyTargetFromGameId(int gameId)
{
    switch (gameId)
    {
    case 1:
        return MarioPartyTarget::MarioParty1;
    case 2:
        return MarioPartyTarget::MarioParty2;
    case 3:
        return MarioPartyTarget::MarioParty3;
    default:
        return MarioPartyTarget::Unknown;
    }
}

MarioPartyTarget detectMarioPartyTargetFromText(const QString& text)
{
    if (containsTitle(text, "Mario Party 3") || containsTitle(text, "MarioParty3") || containsTitle(text, "MP3"))
    {
        return MarioPartyTarget::MarioParty3;
    }

    if (containsTitle(text, "Mario Party 2") || containsTitle(text, "MarioParty2") || containsTitle(text, "MP2"))
    {
        return MarioPartyTarget::MarioParty2;
    }

    if (containsTitle(text, "Mario Party") || containsTitle(text, "MarioParty") || containsTitle(text, "MP1"))
    {
        return MarioPartyTarget::MarioParty1;
    }

    return MarioPartyTarget::Unknown;
}

int goodNameQualityScore(const QString& goodName)
{
    int score = 0;

    if (goodName.contains(QStringLiteral("[!]")))
    {
        score += 100;
    }

    if (goodName.contains(QStringLiteral("(U)")))
    {
        score += 10;
    }
    else if (goodName.contains(QStringLiteral("(E)")))
    {
        score += 8;
    }
    else if (goodName.contains(QStringLiteral("(J)")))
    {
        score += 6;
    }

    if (goodName.contains(QStringLiteral("[b")))
    {
        score -= 50;
    }

    if (goodName.contains(QStringLiteral("[f")))
    {
        score -= 30;
    }

    if (goodName.contains(QStringLiteral("[T")))
    {
        score -= 20;
    }

    if (goodName.contains(QStringLiteral("(unknown rom)")))
    {
        score -= 100;
    }

    return score;
}

QString marioPartyTargetLabel(MarioPartyTarget target)
{
    switch (target)
    {
    case MarioPartyTarget::MarioParty1:
        return QStringLiteral("Mario Party");
    case MarioPartyTarget::MarioParty2:
        return QStringLiteral("Mario Party 2");
    case MarioPartyTarget::MarioParty3:
        return QStringLiteral("Mario Party 3");
    default:
        return QStringLiteral("Unknown");
    }
}

std::optional<MarioPartyRomMatch> findBestMarioPartyRom(MarioPartyTarget target)
{
    if (target == MarioPartyTarget::Unknown)
    {
        return std::nullopt;
    }

    const QString directory = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::RomBrowser_Directory));
    if (directory.isEmpty())
    {
        return std::nullopt;
    }

    const QStringList filters = {
        QStringLiteral("*.z64"),
        QStringLiteral("*.Z64"),
        QStringLiteral("*.n64"),
        QStringLiteral("*.N64"),
        QStringLiteral("*.v64"),
        QStringLiteral("*.V64"),
    };

    QDirIterator iterator(directory, filters, QDir::Files, QDirIterator::Subdirectories);

    std::optional<MarioPartyRomMatch> bestMatch;

    while (iterator.hasNext())
    {
        const QString filePath = iterator.next();
        CoreRomType type;
        CoreRomHeader header;
        CoreRomSettings settings;

        if (!CoreGetCachedRomHeaderAndSettings(filePath.toStdU32String(), &type, &header, nullptr, &settings))
        {
            continue;
        }

        if (marioPartyTargetForRom(header, settings) != target)
        {
            continue;
        }

        MarioPartyRomMatch match;
        match.path = filePath;
        match.goodName = QString::fromStdString(settings.GoodName);
        match.qualityScore = goodNameQualityScore(match.goodName);

        if (!bestMatch.has_value() || match.qualityScore > bestMatch->qualityScore)
        {
            bestMatch = match;
        }
    }

    return bestMatch;
}

QString formatBoardDate(const QString& date)
{
    const QDate parsed = QDate::fromString(date, QStringLiteral("yyyy-MM-dd"));
    if (!parsed.isValid())
    {
        return date;
    }

    return parsed.toString(QStringLiteral("MMMM d, yyyy"));
}

} // namespace Dialog
} // namespace UserInterface
