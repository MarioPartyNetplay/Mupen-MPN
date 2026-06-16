#!/usr/bin/env bash
set -ex
script_dir="$(dirname "$0")"
toplvl_dir="$(realpath "$script_dir/../../")"
build_config="${1:-Debug}"
build_dir="$toplvl_dir/Build/$build_config"
threads="${2:-$(nproc)}"
generator="Unix Makefiles"

if [[ "$1" = "--help" ]] ||
    [[ "$1" = "-h" ]]
then
    echo "$0 [Build Config] [Thread Count]"
    exit
fi

if [[ $(uname -s) = *MINGW64* ]]
then
    generator="MSYS Makefiles"
fi

mkdir -p "$build_dir"

cmake -S "$toplvl_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE="$build_config" \
    -DPORTABLE_INSTALL=ON -DUSE_ANGRYLION=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -G "$generator"

cmake --build "$build_dir" --parallel "$threads"

if [[ "$build_config" = "Debug" ]] ||
    [[ "$build_config" = "RelWithDebInfo" ]]
then
    cmake --install "$build_dir" --prefix="$toplvl_dir"
else
    cmake --install "$build_dir" --strip --prefix="$toplvl_dir"
fi

if [[ $(uname -s) = *MINGW64* ]]
then
    target_bin_dir="$toplvl_dir/Bin/$build_config"
    mkdir -p "$target_bin_dir"

    libdatachannel_dll=$(find "$build_dir" -name 'libdatachannel.dll' -o -name 'libdatachannel*.dll' | head -n 1 || true)
    
    if [[ -n "$libdatachannel_dll" && -f "$libdatachannel_dll" ]]
    then
        cp "$libdatachannel_dll" "$target_bin_dir/"
        
        echo "==> Resolving and copying deep dependencies for libdatachannel..."
        ldd "$libdatachannel_dll" | grep -i 'mingw' | awk '{print $3}' | while read -r dll_path; do
            if [[ -f "$dll_path" ]]; then
                cp -n "$dll_path" "$target_bin_dir/"
            fi
        done
    fi

    cmake --build "$build_dir" --target=bundle_dependencies
fi