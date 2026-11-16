#!/usr/bin/env bash
#
# ./BundleDependencies.sh "./Bin/Release/Mupen-MPN.exe" "./Bin/Release/"
#
set -e

exe="$1"
bin_dir="$2"
if [ -n "${MINGW_PREFIX:-}" ]; then
    path="${MINGW_PREFIX}/bin"
else
    path="/$(printf '%s' "$MSYSTEM" | tr '[:upper:]' '[:lower:]')/bin"
fi

if [ ! -d "$path" ]; then
    echo "MinGW bin directory not found: $path" >&2
    exit 1
fi

function copyForOBJ() {
    local deps
    deps=$(objdump.exe -p "$1" | grep 'DLL Name:' | sed -e "s/\t*DLL Name: //g")
    while read -r line
    do
        [ -n "$line" ] || continue
        findAndCopyDLL "$line"
    done <<< "$deps"
}

function findAndCopyDLL() {
    local file="$path/$1"

    if [ -f "$file" ] && [ ! -f "$bin_dir/$1" ]
    then
        cp "$file" "$bin_dir/"
        copyForOBJ "$file"
    fi
}

function copyRuntimeDlls() {
    shopt -s nullglob
    local file
    local files=("$bin_dir"/*.exe "$bin_dir"/*.dll "$bin_dir/Core"/*.dll "$bin_dir/Plugin"/*/*.dll)
    for file in "${files[@]}"
    do
        [ -f "$file" ] || continue
        echo "=> Copying dependencies for $file"
        copyForOBJ "$file"
    done
}

# UCRT64 ships libcrypto-3.dll. MINGW64 still uses the -x64 suffix.
function copyOptionalDll() {
    local name
    for name in "$@"
    do
        if [ -f "$path/$name" ]
        then
            findAndCopyDLL "$name"
            return 0
        fi
    done
    return 0
}

copyRuntimeDlls

windeployqt6 --exclude-plugins qpdf,qwebp,qgif,qtga,qtuiotouchplugin,qglib,qtiff,qmng,qwbmp \
				--no-translations "$exe"

# windeployqt adds plugins whose dependencies were not visible on the first pass.
copyRuntimeDlls

copyOptionalDll libcrypto-3-x64.dll libcrypto-3.dll
copyOptionalDll libssl-3-x64.dll libssl-3.dll
copyOptionalDll libjpeg-8.dll libjpeg-62.dll
copyOptionalDll libsrtp2.dll

# remove *.a files
find "$bin_dir/" -name '*.a' -delete

exit 0
