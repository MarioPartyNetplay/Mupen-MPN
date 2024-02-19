#!/usr/bin/env bash

set -e

contents=RMGMacOS/RMG-MPN.app/Contents

mkdir -p ${contents}
mkdir -p ${contents}/Frameworks/
mkdir -p ${contents}/Frameworks/Plugin/Audio
mkdir -p ${contents}/Frameworks/Plugin/GFX
mkdir -p ${contents}/Frameworks/Plugin/Input
mkdir -p ${contents}/Frameworks/Plugin/RSP
mkdir -p ${contents}/Frameworks/Core
mkdir -p ${contents}/Frameworks/Data
mkdir -p ${contents}/Frameworks/Cheats

# Get files in correct directories
cp Bin/Release/RMG "${contents}/MacOS/RMG-MPN"
cp Bin/Release/libRMG-Core.dylib "${contents}/Frameworks"
cp Bin/Release/Plugin/Audio/RMG-Audio.dylib "${contents}/Frameworks/Plugin/Audio"
cp Bin/Release/Plugin/GFX/mupen64plus-video-angrylion-plus.dylib "${contents}/Frameworks/Plugin/GFX"
cp Bin/Release/Plugin/GFX/mupen64plus-video-parallel.dylib "${contents}/Frameworks/Plugin/GFX"
cp Bin/Release/Plugin/Input/RMG-Input.dylib "${contents}/Frameworks/Plugin/Input"
cp Bin/Release/Plugin/RSP/mupen64plus-rsp-cxd4.dylib "${contents}/Frameworks/Plugin/RSP"
cp Bin/Release/Plugin/RSP/mupen64plus-rsp-parallel.dylib "${contents}/Frameworks/Plugin/RSP"
cp Bin/Release/Plugin/RSP/mupen64plus-rsp-hle.dylib "${contents}/Frameworks/Plugin/RSP"
cp Bin/Release/Core/libmupen64plus.dylib "${contents}/Frameworks/Core"
cp Bin/Release/Data/font.ttf "${contents}/Frameworks/Data"
cp Bin/Release/Data/gamecontrollerdb.txt "${contents}/Frameworks/Data"
cp Bin/Release/Data/mupen64plus.ini "${contents}/Frameworks/Data"
cp Bin/Release/Data/vosk-model-small-en-us-0.15.zip "${contents}/Frameworks/Data"
cp Bin/Release/Data/Cheats/7C3829D9-6E8247CE-45.cht "${contents}/Frameworks/Data/Cheats"
cp Bin/Release/Data/Cheats/9EA95858-AF72B618-45.cht "${contents}/Frameworks/Data/Cheats"
cp Bin/Release/Data/Cheats/930C29EA-939245BF-45.cht "${contents}/Frameworks/Data/Cheats"
cp Bin/Release/Data/Cheats/2829657E-A0621877-45.cht "${contents}/Frameworks/Data/Cheats"

otool -L "${contents}/Frameworks/libRMG-Core.dylib"
otool -L "${contents}/Frameworks/Plugin/Audio/RMG-Audio.dylib"
otool -L "${contents}/Frameworks/Plugin/Input/RMG-Input.dylib"
otool -L "${contents}/Frameworks/Plugin/GFX/mupen64plus-video-angrylion-plus.dylib"
otool -L "${contents}/Frameworks/Plugin/GFX/mupen64plus-video-parallel.dylib"
otool -L "${contents}/Frameworks/Plugin/RSP/mupen64plus-rsp-cxd4.dylib"
otool -L "${contents}/Frameworks/Plugin/RSP/mupen64plus-rsp-parallel.dylib"
otool -L "${contents}/Frameworks/Plugin/RSP/mupen64plus-rsp-hle.dylib"
otool -L "${contents}/Frameworks/Core/libmupen64plus.dylib"