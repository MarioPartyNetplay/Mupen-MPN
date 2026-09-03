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
#include "Utilities/QtMessageBox.hpp"

#include <RMG-Core/CachedRomHeaderAndSettings.hpp>
#include <RMG-Core/Directories.hpp>
#include <RMG-Core/Settings.hpp>

#include <QDate>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

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

QUrl projectIconUrl(int projectId)
{
    return QUrl(boardDownloaderApiBaseUrl() + QStringLiteral("/project/%1/icon").arg(projectId));
}

std::optional<PartyPlannerCliInfo> resolvePartyPlannerCli(void)
{
    const QDir extrasDirectory(
        QDir(QString::fromStdString(CoreGetSharedDataDirectory().string())).filePath(QStringLiteral("pp64-cli")));

#if defined(Q_OS_WIN)
    const QStringList candidates = {QStringLiteral("partyplanner64-cli-win.exe")};
#elif defined(Q_OS_MACOS)
    const QStringList candidates = {
        QStringLiteral("partyplanner64-cli-macos"),
        QStringLiteral("partyplanner64-cli-linux"),
    };
#else
    const QStringList candidates = {QStringLiteral("partyplanner64-cli-linux")};
#endif

    for (const QString& candidate : candidates)
    {
        const QFileInfo cliInfo(extrasDirectory.filePath(candidate));
        if (!cliInfo.exists() || !cliInfo.isFile())
        {
            continue;
        }

        PartyPlannerCliInfo info;
        info.path = QDir::toNativeSeparators(cliInfo.absoluteFilePath());
#if defined(Q_OS_WIN)
        info.usesWine = false;
#else
        info.usesWine = candidate.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive);
#endif
        return info;
    }

    return std::nullopt;
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

int gameIdFromJson(const QJsonObject& object)
{
    const int gameId = object.value(QStringLiteral("gameId")).toInt();
    if (gameId > 0)
    {
        return gameId;
    }

    const int gameIdSnake = object.value(QStringLiteral("game_id")).toInt();
    if (gameIdSnake > 0)
    {
        return gameIdSnake;
    }

    return object.value(QStringLiteral("game")).toInt();
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

namespace
{

QString absoluteNativePath(const QString& path)
{
    return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
}

bool ensureParentDirectoryExists(const QString& filePath, QString& error)
{
    const QFileInfo fileInfo(filePath);
    const QString directoryPath = fileInfo.absolutePath();
    if (directoryPath.isEmpty())
    {
        error = QStringLiteral("Invalid output path.");
        return false;
    }

    QDir directory(directoryPath);
    if (directory.exists())
    {
        return true;
    }

    if (!directory.mkpath(QStringLiteral(".")))
    {
        error = QStringLiteral("Could not create directory: %1").arg(directoryPath);
        return false;
    }

    return true;
}

bool replaceExistingFile(const QString& filePath, QString& error)
{
    if (!QFile::exists(filePath))
    {
        return true;
    }

    QFile existingFile(filePath);
    existingFile.setPermissions(existingFile.permissions() | QFile::WriteOwner | QFile::WriteUser | QFile::WriteGroup |
                                QFile::WriteOther);
    if (QFile::remove(filePath))
    {
        return true;
    }

    error = QStringLiteral("Could not overwrite existing file: %1").arg(filePath);
    return false;
}

struct PartyPlannerPatchResult
{
    bool success = false;
    bool canForce = false;
    QString output;
};

PartyPlannerPatchResult runPartyPlannerPatch(const PartyPlannerCliInfo& cli,
                                             const QString& boardFilePath,
                                             const QString& romFilePath,
                                             const QString& outputFilePath,
                                             bool force)
{
    PartyPlannerPatchResult result;

    QProcess process;
    QStringList arguments;
    arguments << QStringLiteral("overwrite")
              << QStringLiteral("--rom-file") << romFilePath
              << QStringLiteral("--target-board-index") << QStringLiteral("0")
              << QStringLiteral("--board-file") << boardFilePath
              << QStringLiteral("--output-file") << outputFilePath;
    if (force)
    {
        arguments << QStringLiteral("--force");
    }

    if (cli.usesWine)
    {
        process.setProgram(QStringLiteral("wine"));
        QStringList wineArguments;
        wineArguments << cli.path;
        wineArguments.append(arguments);
        process.setArguments(wineArguments);
    }
    else
    {
        process.setProgram(cli.path);
        process.setArguments(arguments);
        process.setWorkingDirectory(QFileInfo(cli.path).absolutePath());
    }

    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForStarted())
    {
        result.output = process.errorString();
        return result;
    }

    if (!process.waitForFinished(-1))
    {
        result.output = QStringLiteral("PartyPlanner64 CLI timed out.");
        return result;
    }

    result.output = QString::fromUtf8(process.readAll()).trimmed();
    const QFileInfo outputInfo(outputFilePath);
    const bool wroteOutput =
        outputInfo.exists() && outputInfo.isFile() && outputInfo.size() > 0;
    const bool exitedCleanly =
        process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;

    result.canForce = result.output.contains(QStringLiteral("Use --force to overwrite anyway."));
    result.success = exitedCleanly && wroteOutput;
    return result;
}

QString partyPlannerPatchWarnings(const QString& cliOutput)
{
    QStringList warnings;
    for (const QString& line : cliOutput.split(QRegularExpression(QStringLiteral("[\\r\\n]+"))))
    {
        if (line.startsWith(QStringLiteral("Warning:")))
        {
            warnings.push_back(line.mid(QStringLiteral("Warning:").size()).trimmed());
        }
    }
    return warnings.join(QStringLiteral("\n"));
}

} // namespace

QString sanitizeBoardFileName(const QString& fileName)
{
    QString sanitized = fileName;
    sanitized.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));
    return sanitized.trimmed();
}

bool patchMarioPartyBoardRom(QWidget* parent,
                             const QString& boardFilePath,
                             const QString& romFilePath,
                             const QString& outputFilePath)
{
    const std::optional<PartyPlannerCliInfo> cli = resolvePartyPlannerCli();
    if (!cli.has_value())
    {
        Utilities::QtMessageBox::Error(parent,
                                       QStringLiteral("PartyPlanner64 CLI not found"),
                                       QStringLiteral("Expected a bundled CLI in Data/pp64-cli."));
        return false;
    }

    const QString nativeBoardPath = absoluteNativePath(boardFilePath);
    const QString nativeRomPath = absoluteNativePath(romFilePath);
    const QString nativeOutputPath = absoluteNativePath(outputFilePath);

    QString directoryError;
    if (!ensureParentDirectoryExists(nativeOutputPath, directoryError))
    {
        Utilities::QtMessageBox::Error(parent, QStringLiteral("Failed to save patched ROM"), directoryError);
        return false;
    }

    QString replaceError;
    if (!replaceExistingFile(nativeOutputPath, replaceError))
    {
        Utilities::QtMessageBox::Error(parent, QStringLiteral("Failed to save patched ROM"), replaceError);
        return false;
    }

    PartyPlannerPatchResult patchResult =
        runPartyPlannerPatch(*cli, nativeBoardPath, nativeRomPath, nativeOutputPath, false);
    if (!patchResult.success && patchResult.canForce)
    {
        bool unusedCheckBox = false;
        const bool forcePatch = Utilities::QtMessageBox::Question(
            parent,
            QStringLiteral("PartyPlanner64 reported issues with this board.\n\nOverwrite anyway?"),
            QString(),
            unusedCheckBox);
        if (forcePatch)
        {
            patchResult =
                runPartyPlannerPatch(*cli, nativeBoardPath, nativeRomPath, nativeOutputPath, true);
        }
    }

    if (!patchResult.success)
    {
        Utilities::QtMessageBox::Error(parent,
                                       QStringLiteral("Failed to patch ROM"),
                                       patchResult.output.isEmpty()
                                           ? QStringLiteral("PartyPlanner64 CLI did not produce a patched ROM.")
                                           : patchResult.output);
        return false;
    }

    const QString warnings = partyPlannerPatchWarnings(patchResult.output);
    if (!warnings.isEmpty())
    {
        Utilities::QtMessageBox::Info(parent,
                                      QStringLiteral("Patched ROM saved with warnings"),
                                      warnings);
    }

    return true;
}

} // namespace Dialog
} // namespace UserInterface
