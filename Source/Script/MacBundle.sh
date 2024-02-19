#!/usr/bin/env bash

set -e

contents=RMGMacOS/RMG-MPN.app/Contents

mkdir -p ${contents}
mkdir -p ${contents}/MacOS/
mkdir -p ${contents}/MacOS/Plugin/Audio
mkdir -p ${contents}/MacOS/Plugin/GFX
mkdir -p ${contents}/MacOS/Plugin/Input
mkdir -p ${contents}/MacOS/Plugin/RSP
mkdir -p ${contents}/MacOS/Core
mkdir -p ${contents}/MacOS/Data
mkdir -p ${contents}/MacOS/Cheats

# Get files in correct directories
cp Bin/Release/RMG "${contents}/MacOS/RMG-MPN"
cp Bin/Release/libRMG-Core.dylib "${contents}/MacOS"
cp Bin/Release/Plugin/Audio/RMG-Audio.dylib "${contents}/MacOS/Plugin/Audio"
cp Bin/Release/Plugin/GFX/mupen64plus-video-angrylion-plus.dylib "${contents}/MacOS/Plugin/GFX"
cp Bin/Release/Plugin/GFX/mupen64plus-video-parallel.dylib "${contents}/MacOS/Plugin/GFX"
cp Bin/Release/Plugin/Input/RMG-Input.dylib "${contents}/MacOS/Plugin/Input"
cp Bin/Release/Plugin/RSP/mupen64plus-rsp-cxd4.dylib "${contents}/MacOS/Plugin/RSP"
cp Bin/Release/Plugin/RSP/mupen64plus-rsp-parallel.dylib "${contents}/MacOS/Plugin/RSP"
cp Bin/Release/Plugin/RSP/mupen64plus-rsp-hle.dylib "${contents}/MacOS/Plugin/RSP"
cp Bin/Release/Core/libmupen64plus.dylib "${contents}/MacOS/Core"
cp Bin/Release/Data/font.ttf "${contents}/MacOS/Data"
cp Bin/Release/Data/gamecontrollerdb.txt "${contents}/MacOS/Data"
cp Bin/Release/Data/mupen64plus.ini "${contents}/MacOS/Data"
cp Bin/Release/Data/vosk-model-small-en-us-0.15.zip "${contents}/MacOS/Data"
cp Bin/Release/Data/Cheats/7C3829D9-6E8247CE-45.cht "${contents}/MacOS/Data/Cheats"
cp Bin/Release/Data/Cheats/9EA95858-AF72B618-45.cht "${contents}/MacOS/Data/Cheats"
cp Bin/Release/Data/Cheats/930C29EA-939245BF-45.cht "${contents}/MacOS/Data/Cheats"
cp Bin/Release/Data/Cheats/2829657E-A0621877-45.cht "${contents}/MacOS/Data/Cheats"

otool -L "${contents}/MacOS/libRMG-Core.dylib"
otool -L "${contents}/MacOS/Plugin/Audio/RMG-Audio.dylib"
otool -L "${contents}/MacOS/Plugin/Input/RMG-Input.dylib"
otool -L "${contents}/MacOS/Plugin/GFX/mupen64plus-video-angrylion-plus.dylib"
otool -L "${contents}/MacOS/Plugin/GFX/mupen64plus-video-parallel.dylib"
otool -L "${contents}/MacOS/Plugin/RSP/mupen64plus-rsp-cxd4.dylib"
otool -L "${contents}/MacOS/Plugin/RSP/mupen64plus-rsp-parallel.dylib"
otool -L "${contents}/MacOS/Plugin/RSP/mupen64plus-rsp-hle.dylib"
otool -L "${contents}/MacOS/Core/libmupen64plus.dylib"