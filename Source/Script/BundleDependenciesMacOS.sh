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

qt_debug_libs_available() {
    local qt_core_path
    qt_core_path="$(otool -L "$exe" 2>/dev/null | awk '/QtCore\.framework/ { print $1; exit }')"
    if [[ -z "$qt_core_path" ]]
    then
        return 1
    fi

    local qt_framework_dir="${qt_core_path%/QtCore*}"
    [[ -f "$qt_framework_dir/QtCore_debug" ]]
}

is_mach_o_bundle() {
    otool -hv "$1" 2>/dev/null | grep -q ' BUNDLE '
}

should_stage_for_macdeployqt() {
    local plugin="$1"
    if is_mach_o_bundle "$plugin"
    then
        return 0
    fi

    # This plugin cannot be processed by install_name_tool on current Xcode toolchains.
    [[ "$(basename "$plugin")" == "mupen64plus-input-raphnetraw.dylib" ]]
}

bundle_external_dylib() {
    local binary="$1"
    local dep_path="$2"
    local frameworks_dir="$deploy_target/Contents/Frameworks"

    if [[ ! -f "$binary" || ! -f "$dep_path" ]]
    then
        return 0
    fi

    local dep_name
    dep_name="$(basename "$dep_path")"
    mkdir -p "$frameworks_dir"
    cp -f "$dep_path" "$frameworks_dir/$dep_name"
    install_name_tool -id "@executable_path/../Frameworks/$dep_name" "$frameworks_dir/$dep_name" || return 0
    install_name_tool -change "$dep_path" "@executable_path/../Frameworks/$dep_name" "$binary" || \
        echo "BundleDependenciesMacOS.sh: warning: could not rewrite $dep_path in $binary"
}

bundle_homebrew_dependencies() {
    local frameworks_dir="$deploy_target/Contents/Frameworks"
    [[ -d "$frameworks_dir" ]] || return 0

    while IFS= read -r binary
    do
        if should_stage_for_macdeployqt "$binary"
        then
            continue
        fi

        while IFS= read -r dep_path
        do
            bundle_external_dylib "$binary" "$dep_path"
        done < <(otool -L "$binary" 2>/dev/null | awk '/\/opt\/homebrew\/|\/usr\/local\/opt\// { print $1 }')
    done < <(find "$deploy_target/Contents/MacOS" -type f 2>/dev/null)
}

staged_plugins_dir=""
staged_plugins_manifest=""
stash_macdeployqt_incompatible_plugins() {
    local plugin
    staged_plugins_dir="$(mktemp -d)"
    staged_plugins_manifest="$staged_plugins_dir/manifest.txt"
    while IFS= read -r plugin
    do
        if should_stage_for_macdeployqt "$plugin"
        then
            echo "BundleDependenciesMacOS.sh: staging bundle plugin for macdeployqt: $(basename "$plugin")"
            printf '%s\n' "$plugin" >> "$staged_plugins_manifest"
            mv "$plugin" "$staged_plugins_dir/$(basename "$plugin")"
        fi
    done < <(find "$macos_dir" -name '*.dylib' 2>/dev/null)
}

restore_staged_plugins() {
    if [[ -z "$staged_plugins_manifest" || ! -f "$staged_plugins_manifest" ]]
    then
        return 0
    fi

    local original_path name
    while IFS= read -r original_path
    do
        [[ -n "$original_path" ]] || continue
        name="$(basename "$original_path")"
        if [[ -f "$staged_plugins_dir/$name" ]]
        then
            mkdir -p "$(dirname "$original_path")"
            mv "$staged_plugins_dir/$name" "$original_path"
        fi
    done < "$staged_plugins_manifest"

    rm -f "$staged_plugins_manifest"
    rmdir "$staged_plugins_dir" 2>/dev/null || true
    staged_plugins_dir=""
    staged_plugins_manifest=""
}

if [[ -n "$deploy_target" ]]
then
    stash_macdeployqt_incompatible_plugins

    if command -v macdeployqt6 &>/dev/null
    then
        # Qt 6 macdeployqt only accepts single-dash flags (e.g. -no-strip, not --no-strip).
        deployqt_args=(-no-codesign)
        if [[ "$(basename "$bin_dir")" == Debug* ]] && qt_debug_libs_available
        then
            # Only when Qt was built with separate debug frameworks (not Homebrew).
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

    restore_staged_plugins
    bundle_homebrew_dependencies
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
