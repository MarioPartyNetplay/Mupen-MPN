/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef PLUGINDISPLAYNAME_HPP
#define PLUGINDISPLAYNAME_HPP

#include <QFileInfo>
#include <QString>

namespace Utilities
{

inline QString InputPluginDisplayName(const QString& fileName, const QString& pluginName)
{
    const QString file = QFileInfo(fileName).completeBaseName().toLower();
    const QString name = pluginName.toLower();

    if (file.contains(QStringLiteral("raphnet")) || name.contains(QStringLiteral("raphnet")) ||
        name == QStringLiteral("n64 adapter"))
    {
        return QStringLiteral("N64 Adapter");
    }

    if (file.contains(QStringLiteral("gca")) || file.contains(QStringLiteral("gamecube")) ||
        name.contains(QStringLiteral("gamecube")))
    {
        return QStringLiteral("GameCube Adapter");
    }

    if (file == QStringLiteral("rmg-input") || name.contains(QStringLiteral("emulated")) ||
        name.contains(QStringLiteral("mupen mpn - input plugin")))
    {
        return QStringLiteral("Emulated Controllers");
    }

    if (pluginName.isEmpty())
    {
        return QFileInfo(fileName).completeBaseName();
    }

    return pluginName;
}

inline QString InputPluginDescription(const QString& displayName)
{
    if (displayName == QStringLiteral("Emulated Controllers"))
    {
        return QStringLiteral("Mupen MPN input plugin for a keyboard, mouse, or gamepad.");
    }
    if (displayName == QStringLiteral("GameCube Adapter"))
    {
        return QStringLiteral("GameCube adapter plugin for an official GameCube controller adapter.");
    }
    if (displayName == QStringLiteral("N64 Adapter"))
    {
        return QStringLiteral("Raphnet N64 adapter plugin. Controllers are used directly, so there is no extra configuration.");
    }
    return QString();
}

inline int InputPluginSortRank(const QString& displayName)
{
    if (displayName == QStringLiteral("Emulated Controllers"))
    {
        return 0;
    }
    if (displayName == QStringLiteral("GameCube Adapter"))
    {
        return 1;
    }
    if (displayName == QStringLiteral("N64 Adapter"))
    {
        return 2;
    }
    return 3;
}

} // namespace Utilities

#endif // PLUGINDISPLAYNAME_HPP
