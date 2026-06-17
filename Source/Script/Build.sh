#!/usr/bin/env bash
set -ex
script_dir="$(dirname "$0")"
toplvl_dir="$(realpath "$script_dir/../../")"
build_config="${1:-Debug}"
build_dir="$toplvl_dir/Build/$build_config"
generator="Unix Makefiles"
cmake_extra_args=()

if [[ "$1" = "--help" ]] ||
    [[ "$1" = "-h" ]]
then
    echo "$0 [Build Config] [Thread Count]"
    echo ""
    echo "On Apple Silicon, this script re-executes under Rosetta (arch -x86_64)"
    echo "and expects x86_64 Homebrew dependencies in /usr/local."
    echo "Install with: arch -x86_64 /bin/bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\""
    echo "Then: arch -x86_64 brew install cmake qt sdl3 nasm speexdsp libsamplerate minizip pkg-config openssl nlohmann-json libsrtp libusb"
    exit
fi

if [[ $(uname -s) = Darwin ]]; then
  if [[ $(uname -m) = arm64 && "${MPN_SKIP_ROSETTA_REEXEC:-}" != "1" && -x /usr/local/bin/brew ]]; then
    exec arch -x86_64 env MPN_SKIP_ROSETTA_REEXEC=1 PATH="/usr/local/bin:/usr/local/sbin:${PATH}" "$0" "$@"
  fi

  if command -v sysctl >/dev/null 2>&1; then
    threads="${2:-$(sysctl -n hw.ncpu)}"
  else
    threads="${2:-4}"
  fi

  if [[ -x /usr/local/bin/brew ]]; then
    cmake_extra_args+=(-DCMAKE_PREFIX_PATH="/usr/local" -DCMAKE_OSX_ARCHITECTURES=x86_64)
  else
    cmake_extra_args+=(-DCMAKE_PREFIX_PATH="$(brew --prefix)" -DCMAKE_OSX_ARCHITECTURES=arm64)
  fi
elif [[ $(uname -s) = *MINGW64* ]]; then
    generator="MSYS Makefiles"
    threads="${2:-$(nproc)}"
else
    threads="${2:-$(nproc)}"
fi

mkdir -p "$build_dir"

cmake -S "$toplvl_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE="$build_config" \
    -DPORTABLE_INSTALL=ON -DUSE_ANGRYLION=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -G "$generator" \
    "${cmake_extra_args[@]}"

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
