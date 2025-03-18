#!/usr/bin/env bash

set -e

contents=RMGMacOS/Mupen-MPN.app/Contents

mkdir -p ${contents}
mkdir -p ${contents}/MacOS/
mkdir -p ${contents}/Frameworks/
mkdir -p ${contents}/Frameworks/Plugin/Audio
mkdir -p ${contents}/Frameworks/Plugin/GFX
mkdir -p ${contents}/Frameworks/Plugin/Input
mkdir -p ${contents}/Frameworks/Plugin/RSP
mkdir -p ${contents}/MacOS/Core
mkdir -p ${contents}/Frameworks/Data
mkdir -p ${contents}/Frameworks/Cheats

# Get files in correct directories
cp Bin/Release/Mupen-MPN "${contents}/MacOS/Mupen-MPN"
cp Bin/Release/libRMG-Core.dylib "${contents}/MacOS"
cp Bin/Release/Plugin/Audio/RMG-Audio.dylib "${contents}/Frameworks/Plugin/Audio"
cp Bin/Release/Plugin/GFX/mupen64plus-video-GLideN64.dylib "${contents}/Frameworks/Plugin/GFX"
cp Bin/Release/Plugin/Input/RMG-Input.dylib "${contents}/Frameworks/Plugin/Input"
cp Bin/Release/Plugin/RSP/mupen64plus-rsp-hle.dylib "${contents}/Frameworks/Plugin/RSP"
cp Bin/Release/Core/libmupen64plus.dylib "${contents}/MacOS/Core"
cp Bin/Release/Data/font.ttf "${contents}/Frameworks/Data"
cp Bin/Release/Data/gamecontrollerdb.txt "${contents}/Frameworks/Data"
cp Bin/Release/Data/mupen64plus.ini "${contents}/Frameworks/Data"
cp Bin/Release/Data/Cheats/MARIOKART64.cht "${contents}/Frameworks/Data/Cheats"
cp Bin/Release/Data/Cheats/MarioParty.cht "${contents}/Frameworks/Data/Cheats"
cp Bin/Release/Data/Cheats/MarioParty2.cht "${contents}/Frameworks/Data/Cheats"
cp Bin/Release/Data/Cheats/MarioParty3.cht "${contents}/Frameworks/Data/Cheats"
cp Bin/Release/Data/Cheats/MarioPartyN64Combo.cht "${contents}/Frameworks/Data/Cheats"
otool -L "${contents}/MacOS/libRMG-Core.dylib"
otool -L "${contents}/Frameworks/Plugin/Audio/RMG-Audio.dylib"
otool -L "${contents}/Frameworks/Plugin/Input/RMG-Input.dylib"
otool -L "${contents}/Frameworks/Plugin/GFX/mupen64plus-video-GLideN64.dylib"
otool -L "${contents}/Frameworks/Plugin/RSP/mupen64plus-rsp-hle.dylib"
otool -L "${contents}/MacOS/Core/libmupen64plus.dylib"

install_name_tool -change '@rpath/libRMG-Core.dylib' @loader_path/libRMG-Core.dylib "${contents}/MacOS/Mupen-MPN"
install_name_tool -change '@rpath/libmupen64plus.dylib' @loader_path/libmupen64plus.dylib "${contents}/MacOS/Mupen-MPN"

macdeployqt "${contents}/.."
cp -R /usr/local/opt/qt/lib/QtDBus.framework "${contents}/Frameworks/"
