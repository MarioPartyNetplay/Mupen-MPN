#!/usr/bin/env bash
#
# ./BundleDependenciesMacOS.sh [Bin directory]
# Example: ./BundleDependenciesMacOS.sh "./Bin/Debug"
#
set -e

script_dir="$(cd "$(dirname "$0")" && pwd)"
toplvl_dir="$(cd "$script_dir/../.." && pwd)"

bin_dir="$(realpath "${1:-$toplvl_dir/Bin/Debug}")"

# Prefer the .app bundle layout; fall back to plain executable installs.
if [[ -f "$bin_dir/Mupen-MPN.app/Contents/MacOS/Mupen-MPN" ]]
then
    macos_dir="$bin_dir/Mupen-MPN.app/Contents/MacOS"
    deploy_target="$bin_dir/Mupen-MPN.app"
    exe="$macos_dir/Mupen-MPN"
elif [[ -f "$bin_dir/Mupen-MPN" ]]
then
    macos_dir="$bin_dir"
    deploy_target=""
    exe="$bin_dir/Mupen-MPN"
else
    echo "BundleDependenciesMacOS.sh: executable not found at $bin_dir/Mupen-MPN.app or $bin_dir/Mupen-MPN"
    exit 1
fi

if [[ -n "$deploy_target" ]]
then
    if command -v macdeployqt6 &>/dev/null
    then
        # Qt 6 macdeployqt only accepts single-dash flags (e.g. -no-strip, not --no-strip).
        deployqt_args=(-no-codesign)
        if [[ "$(basename "$bin_dir")" == Debug* ]]
        then
            deployqt_args+=(-use-debug-libs)
        else
            deployqt_args+=(-no-strip)
        fi
        macdeployqt6 "$deploy_target" "${deployqt_args[@]}"
    elif command -v macdeployqt &>/dev/null
    then
        macdeployqt "$deploy_target" \
            -no-strip \
            -no-translations
    else
        echo "BundleDependenciesMacOS.sh: macdeployqt6 not found, skipping Qt bundling"
    fi
else
    echo "BundleDependenciesMacOS.sh: plain executable build; skipping macdeployqt"
fi

find_molten_vk() {
    local candidate
    for candidate in \
        "/usr/local/opt/molten-vk/lib/libMoltenVK.dylib" \
        "/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib"
    do
        if [[ -f "$candidate" ]]
        then
            echo "$candidate"
            return 0
        fi
    done

    if command -v brew &>/dev/null
    then
        candidate="$(brew --prefix molten-vk 2>/dev/null)/lib/libMoltenVK.dylib"
        if [[ -f "$candidate" ]]
        then
            echo "$candidate"
            return 0
        fi
    fi

    return 1
}

molten_vk_src="$(find_molten_vk || true)"
if [[ -n "$molten_vk_src" ]]
then
    echo "BundleDependenciesMacOS.sh: bundling MoltenVK from $molten_vk_src"
    cp -f "$molten_vk_src" "$macos_dir/libMoltenVK.dylib"
    # Homebrew ships a signed dylib; strip and re-sign after changing the id.
    codesign --remove-signature "$macos_dir/libMoltenVK.dylib" 2>/dev/null || true
    install_name_tool -id "@loader_path/libMoltenVK.dylib" "$macos_dir/libMoltenVK.dylib"
    codesign --force --sign - "$macos_dir/libMoltenVK.dylib"
else
    echo "BundleDependenciesMacOS.sh: MoltenVK not found; install with: brew install molten-vk"
fi

exit 0
