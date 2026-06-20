#!/usr/bin/env bash
#
# Fetch only the third_party sources required for a Metal-only ANGLE CMake build.
# Avoids full gclient sync (which pulls Vulkan/NASM/lunarg tools and often stalls).
#
# Usage: SyncANGLEDeps.sh <angle-source-dir>
#
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <angle-source-dir>"
    exit 1
fi

angle_src="$(cd "$1" && pwd)"

clone_dep() {
    local dest="$1"
    local url="$2"
    local commit="$3"
    local check_file="$4"

    if [[ -f "$dest/$check_file" ]]; then
        return 0
    fi

    echo "SyncANGLEDeps.sh: fetching $(basename "$dest")"
    rm -rf "$dest"
    mkdir -p "$(dirname "$dest")"
    git clone --filter=blob:none --no-checkout "$url" "$dest"
    git -C "$dest" checkout "$commit"
}

metal_deps_ready() {
    [[ -f "$angle_src/third_party/zlib/adler32.c" ]] &&
    [[ -f "$angle_src/third_party/glslang/src/glslang/Public/ShaderLang.h" ]] &&
    [[ -f "$angle_src/third_party/spirv-headers/src/include/spirv/unified1/spirv.h" ]] &&
    [[ -f "$angle_src/third_party/spirv-tools/src/source/CMakeLists.txt" ]] &&
    [[ -f "$angle_src/third_party/spirv-cross/src/spirv_cross.hpp" ]]
}

if metal_deps_ready; then
    exit 0
fi

clone_dep "$angle_src/third_party/zlib" \
    "https://chromium.googlesource.com/chromium/src/third_party/zlib" \
    "3246f1b60849cc505e231c5d19d0cbf358093555" \
    "adler32.c"

clone_dep "$angle_src/third_party/glslang/src" \
    "https://github.com/KhronosGroup/glslang.git" \
    "20960a4872f681e4213312b06b48cc4ddae3c73d" \
    "glslang/Public/ShaderLang.h"

clone_dep "$angle_src/third_party/spirv-headers/src" \
    "https://github.com/KhronosGroup/SPIRV-Headers.git" \
    "c63848ecf2200425511319fd8bf2c17b751e501e" \
    "include/spirv/unified1/spirv.h"

clone_dep "$angle_src/third_party/spirv-tools/src" \
    "https://github.com/KhronosGroup/SPIRV-Tools.git" \
    "113784c9cf103b775fdadb1ef194f6b963e24a7e" \
    "source/CMakeLists.txt"

clone_dep "$angle_src/third_party/spirv-cross/src" \
    "https://github.com/KhronosGroup/SPIRV-Cross.git" \
    "b8fcf307f1f347089e3c46eb4451d27f32ebc8d3" \
    "spirv_cross.hpp"

if ! metal_deps_ready; then
    echo "SyncANGLEDeps.sh: one or more Metal ANGLE dependencies are still missing"
    exit 1
fi

echo "SyncANGLEDeps.sh: Metal ANGLE dependencies ready"
