@echo off
rem Replay a recorded session in a window: replay.bat ..\recordings\YYYYMMDD-HHMMSS
rem The memory card starts again from the copy taken when the session began (in the recording's replay_card folder);
rem your own card is not touched. Live input is ignored. For a headless replay at full speed, see docs/handoff.md.
if "%~1"=="" (echo usage: replay.bat RECORDING_FOLDER & exit /b 1)
set "REC=%~f1"
cd /d "%~dp0"
set "PATH=%~dp0;C:\msys64\ucrt64\bin;%PATH%"
set "HG_PADPLAY=%REC%"
set HG_PADREC=
hgboot.exe -q > replay.log 2>&1
