@echo off

REM Initialize the iso_file variable
set "iso_file="

REM Get the first ISO file in the same directory as the script
for %%F in ("%~dp0*.z64") do (
    set "iso_file=%%~F"
    goto :found_iso
)

:not_found
echo.
cls
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
echo No Z64 files found in the same directory as the script.
echo Place the Mario Party 3 (USA) Z64 in the script's directory and run it again.
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
echo.
pause
exit /b 0

:found_iso
echo.
echo Given "%iso_file%"
mkdir tmp
cls
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
echo Patching Shiver Blossom Shore over Chilly Waters
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
"tools/partyplanner64-cli-win.exe" overwrite --rom-file "%iso_file%" --target-board-index 0 --board-file "./store/shiverblossomshore-2.0.json" --output-file "./tmp/tmp.z64" > NUL

cls
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
echo Patching Seized Seasides over Deep Bloober Sea
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
"tools/partyplanner64-cli-win.exe" overwrite --rom-file "tmp/tmp.z64" --target-board-index 1 --board-file "./store/seizedseasides-1.0.json" --output-file "tmp/tmp.z64" > NUL

cls
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
echo Patching Parched Sands over Spiny Desert
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
"tools/partyplanner64-cli-win.exe" overwrite --rom-file "tmp/tmp.z64" --target-board-index 2 --board-file "./store/parchedsands-2.0.json" --output-file "tmp/tmp.z64" > NUL

cls
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
echo Patching Seasonal Woodlands over Woody Woods
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
"tools/partyplanner64-cli-win.exe" overwrite --rom-file "tmp/tmp.z64" --target-board-index 3 --board-file "./store/seasonalwoodlands-1.5.json" --output-file "tmp/tmp.z64" > NUL

cls
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
echo Patching Ghastly Graveyard over Creepy Cavern
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
"tools/partyplanner64-cli-win.exe" overwrite --rom-file "tmp/tmp.z64" --target-board-index 4 --board-file "./store/ghastlygraveyard-0.99.2.json" --output-file "tmp/tmp.z64" > NUL

cls
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
echo Patching Star Summit over Waluigi's Island
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
"tools/partyplanner64-cli-win.exe" overwrite --rom-file "tmp/tmp.z64" --target-board-index 5 --board-file "./store/starsummit-1.2.json" --output-file "Mario Party 3 (USA) [Board Pack A - V1].z64" > NUL

cls
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
echo Success! Your game is located at "Mario Party 3 (USA) [Board Pack A - V1].z64"
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
echo Press Enter to exit.
pause > NUL
rmdir /s /q tmp
exit /b 0