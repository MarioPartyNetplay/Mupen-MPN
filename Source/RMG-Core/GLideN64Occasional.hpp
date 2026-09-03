/*
 * Mupen MPN - occasional GLideN64 setting overrides
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_GLIDEN64_OCCASIONAL_HPP
#define CORE_GLIDEN64_OCCASIONAL_HPP

#include "RomHeader.hpp"
#include "RomSettings.hpp"

void CoreGLideN64OccasionalSetRom(const CoreRomHeader& header, const CoreRomSettings& settings);
void CoreGLideN64OccasionalClearRom(void);
void CoreGLideN64OccasionalUpdateFrame(void);

#endif // CORE_GLIDEN64_OCCASIONAL_HPP
