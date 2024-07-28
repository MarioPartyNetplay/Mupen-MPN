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
echo Patching Clockwork Courtyard over Chilly Waters
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
"tools/partyplanner64-cli-win.exe" overwrite --rom-file "%iso_file%" --target-board-index 0 --board-file "./store/clockwork-courtyard-1.0.json" --output-file "./tmp/tmp.z64" > NUL

cls
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
echo Patching Primal Isles over Deep Bloober Sea
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
"tools/partyplanner64-cli-win.exe" overwrite --rom-file "tmp/tmp.z64" --target-board-index 1 --board-file "./store/primal-isles-4.2.json" --output-file "tmp/tmp.z64" > NUL

cls
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
echo Patching Sultan City over Spiny Desert
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
"tools/partyplanner64-cli-win.exe" overwrite --rom-file "tmp/tmp.z64" --target-board-index 2 --board-file "./store/sultan-city-1.1.json" --output-file "tmp/tmp.z64" > NUL

cls
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
echo Patching King Boo Carnival over Woody Woods
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
"tools/partyplanner64-cli-win.exe" overwrite --rom-file "tmp/tmp.z64" --target-board-index 3 --board-file "./store/king-boo-carnival-4.0.json" --output-file "tmp/tmp.z64" > NUL

cls
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
echo Patching Snifit Stock Exchange over Creepy Cavern
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
"tools/partyplanner64-cli-win.exe" overwrite --rom-file "tmp/tmp.z64" --target-board-index 4 --board-file "./store/snifit-stock-exchange-1.json" --output-file "tmp/tmp.z64" > NUL

cls
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
echo Patching Luma's Playground over Waluigi's Island
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
"tools/partyplanner64-cli-win.exe" overwrite --rom-file "tmp/tmp.z64" --target-board-index 5 --board-file "./store/lumas-playground-1.1.json" --output-file "Mario Party 3 (USA) [Board Pack B - V1].z64" > NUL

cls
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
echo Success! Your game is located at "Mario Party 3 (USA) [Board Pack B - V1].z64"
echo = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
echo Press Enter to exit.
pause > NUL
rmdir /s /q tmp
exit /b 0