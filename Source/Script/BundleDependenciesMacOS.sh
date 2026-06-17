#!/usr/bin/env bash
#
# ./BundleDependenciesMacOS.sh "./Bin/Release"
#
set -e

bin_dir="$1"
exe="$bin_dir/Mupen-MPN"

if [[ ! -f "$exe" ]]
then
    echo "BundleDependenciesMacOS.sh: executable not found at $exe"
    exit 1
fi

if command -v macdeployqt6 &>/dev/null
then
    macdeployqt6 "$exe" \
        --no-strip \
        --no-translations \
        --exclude-plugins qpdf,qwebp,qgif,qtga,qtuiotouchplugin,qglib,qtiff,qmng,qwbmp
elif command -v macdeployqt &>/dev/null
then
    macdeployqt "$exe" \
        --no-strip \
        --no-translations
else
    echo "BundleDependenciesMacOS.sh: macdeployqt6 not found, skipping Qt bundling"
fi

exit 0
