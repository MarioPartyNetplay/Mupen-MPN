#!/usr/bin/env bash
#
# Build ANGLE with the Metal backend for macOS.
# Usage: BuildANGLE.sh <angle-source-dir> <output-dir>
#
set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <angle-source-dir> <output-dir>"
    exit 1
fi

angle_src="$(cd "$1" && pwd)"
angle_out="$(mkdir -p "$2" && cd "$2" && pwd)"
script_dir="$(cd "$(dirname "$0")" && pwd)"
toplvl_dir="$(cd "$script_dir/../.." && pwd)"
depot_tools_dir="${DEPOT_TOOLS_DIR:-$toplvl_dir/Build/depot_tools}"

if [[ ! -d "$angle_src" ]]; then
    echo "BuildANGLE.sh: ANGLE source directory not found: $angle_src"
    exit 1
fi

if [[ ! -d "$depot_tools_dir" ]]; then
    echo "BuildANGLE.sh: fetching depot_tools into $depot_tools_dir"
    git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git "$depot_tools_dir"
fi

export PATH="$depot_tools_dir:$PATH"

if ! command -v gn >/dev/null 2>&1; then
    echo "BuildANGLE.sh: gn not found in depot_tools PATH"
    exit 1
fi

if ! command -v ninja >/dev/null 2>&1; then
    echo "BuildANGLE.sh: ninja not found in depot_tools PATH"
    exit 1
fi

cd "$angle_src"

if [[ ! -f .gclient ]]; then
    echo "BuildANGLE.sh: bootstrapping ANGLE dependencies (first run may take several minutes)"
    python3 scripts/bootstrap.py
    gclient sync -D --no-history --shallow --nohooks
fi

target_cpu="$(uname -m)"
if [[ "$target_cpu" == "x86_64" ]]; then
    target_cpu="x64"
fi

cat > "$angle_out/args.gn" <<EOF
target_cpu = "${target_cpu}"
is_component_build = false
is_debug = false
angle_enable_d3d9 = false
angle_enable_d3d11 = false
angle_enable_gl = false
angle_enable_metal = true
angle_enable_null = false
angle_enable_vulkan = false
angle_enable_essl = true
angle_enable_glsl = true
angle_enable_swiftshader = false
EOF

echo "BuildANGLE.sh: generating ninja files in $angle_out"
gn gen "$angle_out"

echo "BuildANGLE.sh: building libEGL and libGLESv2"
ninja -C "$angle_out" libEGL libGLESv2

for lib in libEGL.dylib libGLESv2.dylib; do
    if [[ ! -f "$angle_out/$lib" ]]; then
        echo "BuildANGLE.sh: expected output not found: $angle_out/$lib"
        exit 1
    fi
done

echo "BuildANGLE.sh: ANGLE Metal build complete"
