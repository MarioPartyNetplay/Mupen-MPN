#!/usr/bin/env bash
#
# ./BundleDependencies.sh "./Bin/Release/Mupen-MPN.exe" "./Bin/Release/" "/mingw64/bin"
#

exe="$1"
bin_dir="$2"
path="$3"

function copyForOBJ() {
    local deps=`objdump.exe -p $1 | grep 'DLL Name:' | sed -e "s/\t*DLL Name: //g"`
    while read -r line
	do
        findAndCopyDLL $line
    done <<< "$deps"
}

function findAndCopyDLL() {
    local file="$path/$1"
    
	if [ -f $file ] && [ ! -f "$bin_dir/$1" ]
	then
		cp "$file" "$bin_dir"
        copyForOBJ $file
		return 0
	fi

    return 0
}


for file in "$bin_dir"/*.exe "$bin_dir/Core"/*.dll "$bin_dir/Plugin"/*/*.dll
do
	echo "=> Copying dependencies for $file"
	copyForOBJ "$file"
done

windeployqt-qt6 --no-translations "$exe" --plugindir "$bin_dir/QtPlugins/plugins"
echo -e "[Paths]\nPrefix = QtPlugins/\nPlugins = plugins" >  "$bin_dir/qt.conf"

# needed by Qt at runtime
cp "$path/libcrypto-3-x64.dll" "$bin_dir/"
cp "$path/libssl-3-x64.dll"    "$bin_dir/"
cp "$path/libjpeg-8.dll"       "$bin_dir/"

touch "$bin_dir/portable.txt"

ls
cp -r "../../Data/Games" "$bin_dir/"
cp "../../Data/Downloader.exe" "$bin_dir/Data/"
cp "../../Data/Updater.exe" "$bin_dir/Data/"
cp "../../Data/Updater.ini" "$bin_dir/Data/"

# remove *.a files
find "$bin_dir/" -name '*.a' -delete

exit 0
