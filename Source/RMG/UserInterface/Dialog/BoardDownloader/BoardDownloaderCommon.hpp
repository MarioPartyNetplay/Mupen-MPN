/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef BOARDDOWNLOADERCOMMON_HPP
#define BOARDDOWNLOADERCOMMON_HPP

#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QWidget>

#include <RMG-Core/RomHeader.hpp>
#include <RMG-Core/RomSettings.hpp>

#include <optional>

namespace UserInterface
{
namespace Dialog
{

enum class MarioPartyTarget
{
    Unknown = 0,
    MarioParty1,
    MarioParty2,
    MarioParty3,
};

struct MarioPartyRomMatch
{
    QString path;
    QString goodName;
    int qualityScore = 0;
};

struct PartyPlannerCliInfo
{
    QString path;
    bool usesWine = false;
};

QString boardDownloaderApiBaseUrl(void);
QUrl projectIconUrl(int projectId);

constexpr inline int boardDownloaderDefaultPageSize(void)
{
    return 25;
}

std::optional<PartyPlannerCliInfo> resolvePartyPlannerCli(void);

bool isMarioParty1(const CoreRomHeader& header, const CoreRomSettings& settings);
bool isMarioParty2(const CoreRomHeader& header, const CoreRomSettings& settings);
bool isMarioParty3(const CoreRomHeader& header, const CoreRomSettings& settings);

MarioPartyTarget marioPartyTargetForRom(const CoreRomHeader& header, const CoreRomSettings& settings);
MarioPartyTarget marioPartyTargetFromGameId(int gameId);
int gameIdFromJson(const QJsonObject& object);

int goodNameQualityScore(const QString& goodName);
QString marioPartyTargetLabel(MarioPartyTarget target);

std::optional<MarioPartyRomMatch> findBestMarioPartyRom(MarioPartyTarget target);
QString formatBoardDate(const QString& date);

} // namespace Dialog
} // namespace UserInterface

#endif // BOARDDOWNLOADERCOMMON_HPP
