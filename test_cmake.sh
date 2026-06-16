#!/usr/bin/env bash
set -e
cd /home/Tabitha/RMG
rm -rf Build/Debug/CMakeCache.txt Build/Debug/CMakeFiles
cmake -S . -B Build/Debug \
    -DCMAKE_BUILD_TYPE=Debug \
    -DPORTABLE_INSTALL=ON \
    -DUSE_ANGRYLION=ON \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -G 'MSYS Makefiles' 2>&1 | tail -15
