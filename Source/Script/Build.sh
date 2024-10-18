#!/usr/bin/env bash
# Check if the system is macOS
if [[ "$(uname)" == "Darwin" ]]; then
    alias nproc="sysctl -n hw.logicalcpu"
    export CXXFLAGS='-stdlib=libc++'
    export LDFLAGS='-mmacosx-version-min=14.0'
    export CMAKE_SYSROOT="/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/"
    export PATH="/usr/local/opt/qt@5/bin:$PATH"
    export PATH="/usr/local/opt/libiconv/bin:$PATH"
    export PATH="/usr/local/opt/sdl2/bin:$PATH"
    export LIBRARY_PATH="/usr/local/opt/minizip/lib:$LIBRARY_PATH"
    export LIBRARY_PATH="/usr/local/opt/sdl2/lib:$LIBRARY_PATH"
    export LIBRARY_PATH="/usr/local/opt/sdl2_net/lib:$LIBRARY_PATH"
    export LIBRARY_PATH="/usr/local/opt/speexdsp/lib:$LIBRARY_PATH"
    export LIBRARY_PATH="/usr/local/opt/libsamplerate/lib:$LIBRARY_PATH"
fi

set -ex
script_dir="$(dirname "$0")"
toplvl_dir="$(realpath "$script_dir/../../")"
build_config="${1:-Release}"
build_dir="$toplvl_dir/Build/$build_config"
threads="${2:-$(nproc)}"

if [ "$1" = "--help" ] ||
    [ "$1" = "-h" ]
then
    echo "$0 [Build Config] [Thread Count]"
    exit
fi

mkdir -p "$build_dir"

cmake -S "$toplvl_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE="$build_config" -DPORTABLE_INSTALL=ON -DUSE_ANGRYLION=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -G "Unix Makefiles"

cmake --build "$build_dir" --parallel "$threads"

if [[ "$(uname)" == "Linux" ]]; then
    if [[ "$build_config" == "Debug" ]] || 
       [[ "$build_config" == "RelWithDebInfo" ]]; then
        cmake --install "$build_dir" --prefix="$toplvl_dir"
    else
        cmake --install "$build_dir" --strip --prefix="$toplvl_dir"
    fi
fi

if [[ "$(uname -s)" == *MINGW64* ]]; then
    cmake --build "$build_dir" --target=bundle_dependencies -j11
fi