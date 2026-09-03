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
    echo "macOS prefers MacPorts (/opt/local). CI builds Intel x86_64 even on Apple Silicon"
    echo "by setting build_arch x86_64 in /opt/local/etc/macports/macports.conf"
    echo "and/or MPN_OSX_ARCH=x86_64."
    echo "Install: https://www.macports.org/install.php"
    echo "Then: sudo port install cmake ccache nasm pkgconfig qt6-qtbase qt6-qttools qt6-qtsvg qt6-qtimageformats SDL3 speexDSP libsamplerate minizip openssl3 nlohmann-json libsrtp libusb libpng MoltenVK vulkan-loader vulkan-headers hidapi libenet"
    echo ""
    echo "Fallback: Intel Homebrew in /usr/local (Rosetta on Apple Silicon), or native Homebrew."
    exit
fi

if [[ $(uname -s) = Darwin ]]; then
  if command -v sysctl >/dev/null 2>&1; then
    threads="${2:-$(sysctl -n hw.ncpu)}"
  else
    threads="${2:-4}"
  fi

  host_arch="$(uname -m)"
  macos_arch="${MPN_OSX_ARCH:-$host_arch}"
  macports_prefix="/opt/local"
  macports_qt="${macports_prefix}/libexec/qt6"

  if [[ -z "${MPN_OSX_ARCH:-}" && -x "${macports_prefix}/bin/port" ]]; then
    mp_arch="$(awk '/^[[:space:]]*build_arch[[:space:]]+/ { print $2; exit }' "${macports_prefix}/etc/macports/macports.conf" 2>/dev/null || true)"
    macos_arch="${mp_arch:-$macos_arch}"
  fi

  # mupen64plus unix Makefiles use uname -m for HOST_CPU. When targeting
  # Intel from an arm64 process, run the rest of the build under Rosetta.
  if [[ "$macos_arch" = x86_64 && "$host_arch" = arm64 && "${MPN_SKIP_ROSETTA_REEXEC:-}" != "1" ]]; then
    exec arch -x86_64 env MPN_SKIP_ROSETTA_REEXEC=1 MPN_OSX_ARCH=x86_64 "$0" "$@"
  fi

  if [[ -x "${macports_prefix}/bin/port" ]]; then
    export PATH="${macports_qt}/bin:${macports_prefix}/bin:${macports_prefix}/sbin:${PATH}"
    export PKG_CONFIG_PATH="${macports_prefix}/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    cmake_extra_args+=(-DCMAKE_PREFIX_PATH="${macports_qt};${macports_prefix}" -DCMAKE_IGNORE_PREFIX_PATH="/usr/local;/opt/homebrew" -DCMAKE_OSX_ARCHITECTURES="${macos_arch}" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3)
  elif [[ -x /usr/local/bin/brew ]]; then
    if [[ "$host_arch" = arm64 && "${MPN_SKIP_ROSETTA_REEXEC:-}" != "1" ]]; then
      exec arch -x86_64 env MPN_SKIP_ROSETTA_REEXEC=1 PATH="/usr/local/bin:/usr/local/sbin:${PATH}" "$0" "$@"
    fi
    cmake_extra_args+=(-DCMAKE_PREFIX_PATH="/usr/local" -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3)
  else
    cmake_extra_args+=(-DCMAKE_PREFIX_PATH="$(brew --prefix)" -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3)
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

if [[ $(uname -s) = Darwin ]]
then
    cmake --build "$build_dir" --target=bundle_dependencies
fi
