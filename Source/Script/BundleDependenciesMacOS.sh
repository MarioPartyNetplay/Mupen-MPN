#!/usr/bin/env bash
#
# ./BundleDependenciesMacOS.sh [Bin directory] [--rewrite-only]
# Example: ./BundleDependenciesMacOS.sh "./Bin/Debug"
#
set -e

script_dir="$(cd "$(dirname "$0")" && pwd)"
toplvl_dir="$(cd "$script_dir/../.." && pwd)"

rewrite_only=0
bin_dir=""
for arg in "$@"
do
    if [[ "$arg" == "--rewrite-only" ]]
    then
        rewrite_only=1
    elif [[ -z "$bin_dir" ]]
    then
        bin_dir="$(realpath "$arg")"
    fi
done
bin_dir="${bin_dir:-$(realpath "$toplvl_dir/Bin/Debug")}"

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

build_config="$(basename "$bin_dir")"
build_exe="$toplvl_dir/Build/$build_config/Source/RMG/Mupen-MPN.app/Contents/MacOS/Mupen-MPN"
if [[ -f "$build_exe" && "$build_exe" -nt "$exe" ]]
then
    echo "BundleDependenciesMacOS.sh: warning: $exe is older than $build_exe"
    echo "BundleDependenciesMacOS.sh: run 'cmake --install $toplvl_dir/Build/$build_config --prefix=$toplvl_dir' first"
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

    local name
    name="$(basename "$plugin")"
    # macdeployqt walks every dylib; keep the emulator core and plugins it
    # cannot rewrite out of that pass. Homebrew deps are rewritten later.
    [[ "$name" == "mupen64plus-input-raphnetraw.dylib" ||
       "$name" == "libmupen64plus.dylib" ||
       "$plugin" == *"/Core/"* ]]
}

skip_homebrew_rewrite() {
    local plugin="$1"
    if is_mach_o_bundle "$plugin"
    then
        return 0
    fi

    [[ "$(basename "$plugin")" == "mupen64plus-input-raphnetraw.dylib" ]]
}

bundle_external_dylib() {
    local binary="$1"
    local dep_path="$2"
    local frameworks_dir="$deploy_target/Contents/Frameworks"

    if [[ ! -f "$binary" || -z "$dep_path" ]]
    then
        return 1
    fi

    local dep_name dest new_path dest_existed=0
    dep_name="$(basename "$dep_path")"
    dest="$frameworks_dir/$dep_name"
    new_path="@executable_path/../Frameworks/$dep_name"
    mkdir -p "$frameworks_dir"
    [[ -f "$dest" ]] && dest_existed=1

    # Copy when the original still exists (fresh from MacPorts/Homebrew).
    # Nested dylibs such as libminizip -> libz are often already staged by
    # macdeployqt, but still record the absolute prefix path.
    if [[ -f "$dep_path" ]]
    then
        cp -f "$dep_path" "$dest"
        install_name_tool -id "$new_path" "$dest" 2>/dev/null || true
        codesign --force --sign - "$dest" 2>/dev/null || true
    elif [[ ! -f "$dest" && "$dep_name" == libz.* ]]
    then
        # macOS always provides zlib; MacPorts minizip/libpng may still
        # record /opt/local/lib/libz.1.dylib when that file is absent.
        if install_name_tool -change "$dep_path" "/usr/lib/libz.1.dylib" "$binary" 2>/dev/null
        then
            codesign --force --sign - "$binary" 2>/dev/null || true
            return 0
        fi
        echo "BundleDependenciesMacOS.sh: warning: could not rewrite $dep_path in $binary"
        return 1
    elif [[ ! -f "$dest" ]]
    then
        return 1
    fi

    if install_name_tool -change "$dep_path" "$new_path" "$binary" 2>/dev/null
    then
        codesign --force --sign - "$binary" 2>/dev/null || true
    else
        echo "BundleDependenciesMacOS.sh: warning: could not rewrite $dep_path in $binary"
    fi

    [[ "$dest_existed" -eq 0 && -f "$dest" ]]
}

bundle_homebrew_dependencies() {
    local frameworks_dir="$deploy_target/Contents/Frameworks"
    [[ -n "$deploy_target" ]] || return 0
    mkdir -p "$frameworks_dir"

    # Walk MacOS binaries and already-bundled Frameworks dylibs so nested
    # deps (libminizip -> libz, libpng -> libz) get rewritten too.
    local pass copied binary dep_path
    for pass in 1 2 3 4 5 6 7 8
    do
        copied=0
        while IFS= read -r binary
        do
            if skip_homebrew_rewrite "$binary"
            then
                continue
            fi

            otool -hv "$binary" 2>/dev/null | grep -q "MH_" || continue

            while IFS= read -r dep_path
            do
                if [[ "$dep_path" == *Qt*.framework* ]]
                then
                    continue
                fi
                if bundle_external_dylib "$binary" "$dep_path"
                then
                    copied=1
                fi
            done < <(otool -L "$binary" 2>/dev/null | awk '/\/opt\/homebrew\/|\/usr\/local\/opt\/|\/usr\/local\/Cellar\/|\/opt\/local\// { print $1 }')
        done < <(find "$deploy_target/Contents/MacOS" "$frameworks_dir" -type f 2>/dev/null)

        [[ "$copied" -eq 0 ]] && break
    done
}

rewrite_bundled_qt_frameworks() {
    [[ -n "$deploy_target" && -f "$exe" ]] || return 0

    local frameworks_dir="$deploy_target/Contents/Frameworks"
    [[ -d "$frameworks_dir" ]] || return 0

    local binary qt_path framework_name fw_binary new_path
    while IFS= read -r binary
    do
        otool -hv "$binary" 2>/dev/null | grep -q "MH_" || continue

        while IFS= read -r qt_path
        do
            [[ -n "$qt_path" ]] || continue
            framework_name="$(sed -n 's#.*/\([^/]*\.framework\)/.*#\1#p' <<< "$qt_path")"
            [[ "$framework_name" == *.framework ]] || continue

            fw_binary="${framework_name%.framework}"
            new_path="@executable_path/../Frameworks/$framework_name/Versions/A/$fw_binary"
            if [[ ! -f "$frameworks_dir/$framework_name/Versions/A/$fw_binary" ]]
            then
                continue
            fi

            install_name_tool -change "$qt_path" "$new_path" "$binary" 2>/dev/null || \
                echo "BundleDependenciesMacOS.sh: warning: could not rewrite $qt_path in $binary"
        done < <(otool -L "$binary" 2>/dev/null | awk '/Qt.*\.framework/ && /\/(opt\/|Cellar\/)/ { print $1 }')
    done < <(find "$deploy_target/Contents/MacOS" -type f 2>/dev/null)

    # The main executable is not always discovered by the MacOS directory scan on some bundle layouts.
    rewrite_bundled_qt_frameworks_for_binary "$exe"
}

rewrite_bundled_qt_frameworks_for_binary() {
    local binary="$1"
    [[ -f "$binary" ]] || return 0

    local frameworks_dir="$deploy_target/Contents/Frameworks"
    [[ -d "$frameworks_dir" ]] || return 0

    local qt_path framework_name fw_binary new_path
    otool -hv "$binary" 2>/dev/null | grep -q "MH_" || return 0

    while IFS= read -r qt_path
    do
        [[ -n "$qt_path" ]] || continue
        framework_name="$(sed -n 's#.*/\([^/]*\.framework\)/.*#\1#p' <<< "$qt_path")"
        [[ "$framework_name" == *.framework ]] || continue

        fw_binary="${framework_name%.framework}"
        new_path="@executable_path/../Frameworks/$framework_name/Versions/A/$fw_binary"
        if [[ ! -f "$frameworks_dir/$framework_name/Versions/A/$fw_binary" ]]
        then
            continue
        fi

        install_name_tool -change "$qt_path" "$new_path" "$binary" 2>/dev/null || \
            echo "BundleDependenciesMacOS.sh: warning: could not rewrite $qt_path in $binary"
    done < <(otool -L "$binary" 2>/dev/null | awk '/Qt.*\.framework/ && /\/(opt\/|Cellar\/)/ { print $1 }')
}

ensure_framework_rpath() {
    [[ -f "$exe" ]] || return 0

    if ! otool -l "$exe" 2>/dev/null | awk '/cmd LC_RPATH/{getline; print $2}' | grep -qx "@executable_path/../Frameworks"
    then
        install_name_tool -add_rpath "@executable_path/../Frameworks" "$exe" 2>/dev/null || \
            echo "BundleDependenciesMacOS.sh: warning: could not add Frameworks rpath to $exe"
    fi
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

if [[ "$rewrite_only" -eq 1 ]]
then
    rewrite_bundled_qt_frameworks
    ensure_framework_rpath
    bundle_homebrew_dependencies
    exit 0
fi

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
    rewrite_bundled_qt_frameworks
    ensure_framework_rpath
    bundle_homebrew_dependencies
else
    echo "BundleDependenciesMacOS.sh: plain executable build; skipping macdeployqt"
fi

find_molten_vk() {
    local candidate
    for candidate in \
        "/opt/local/lib/libMoltenVK.dylib" \
        "/usr/local/opt/molten-vk/lib/libMoltenVK.dylib" \
        "/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib"
    do
        if [[ -f "$candidate" ]]
        then
            echo "$candidate"
            return 0
        fi
    done

    if command -v port &>/dev/null
    then
        candidate="$(port contents MoltenVK 2>/dev/null | awk '/libMoltenVK\.dylib$/ { print $1; exit }')"
        if [[ -f "$candidate" ]]
        then
            echo "$candidate"
            return 0
        fi
    fi

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
    # Do not strip the vendor signature first: that can leave __LINKEDIT
    # inconsistent so install_name_tool refuses the file entirely.
    if otool -hv "$macos_dir/libMoltenVK.dylib" 2>/dev/null | grep -q "MH_"
    then
        if ! install_name_tool -id "@loader_path/libMoltenVK.dylib" "$macos_dir/libMoltenVK.dylib" 2>/dev/null
        then
            echo "BundleDependenciesMacOS.sh: warning: could not rewrite MoltenVK install name; using unmodified copy"
            cp -f "$molten_vk_src" "$macos_dir/libMoltenVK.dylib"
        fi
        codesign --force --sign - "$macos_dir/libMoltenVK.dylib" 2>/dev/null || \
            echo "BundleDependenciesMacOS.sh: warning: could not ad-hoc sign MoltenVK"
    else
        echo "BundleDependenciesMacOS.sh: warning: MoltenVK is not a Mach-O dylib; leaving as copied"
    fi
else
    echo "BundleDependenciesMacOS.sh: MoltenVK not found; install with: sudo port install MoltenVK"
fi

exit 0
