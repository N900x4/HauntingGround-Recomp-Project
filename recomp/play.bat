@echo off
rem Haunting Ground (Windows build): window, keyboard or gamepad, sound, paced to real time.
rem Build hgboot.exe first (README). The memory card is ..\memcards\port0; the run log goes to play.log.
rem Every session is recorded to ..\recordings\YYYYMMDD-HHMMSS (input and a copy of the memory card at the start),
rem so it can be replayed exactly with replay.bat. set HG_PADREC=off before starting to turn that off.
rem Options are environment variables, set before starting, e.g.:
rem   set HG_SCALE=3                     window scale (default 2)
rem   set HG_WAV=D:\ps2-work\session.wav  record the game's sound
rem   set HG_MOVIES=skip                 skip the movies
rem   set HG_GSVK=1                      draw with the GPU (Vulkan; new, same picture as the software renderer)
cd /d "%~dp0"
set "PATH=%~dp0;C:\msys64\ucrt64\bin;%PATH%"
if not defined HG_PADPLAY if not defined HG_PADREC set HG_PADREC=auto
if /i "%HG_PADREC%"=="off" set HG_PADREC=
hgboot.exe -q %* > play.log 2>&1
