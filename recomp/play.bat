@echo off
rem Haunting Ground (Windows build): window, keyboard or gamepad, sound, paced to real time.
rem Build hgboot.exe first (README). The memory card is ..\memcards\port0; the run log goes to play.log.
rem Options are environment variables, set before starting, e.g.:
rem   set HG_SCALE=3                     window scale (default 2)
rem   set HG_WAV=D:\ps2-work\session.wav  record the game's sound
rem   set HG_MOVIES=skip                 skip the movies
cd /d "%~dp0"
set "PATH=%~dp0;C:\msys64\ucrt64\bin;%PATH%"
hgboot.exe -q %* > play.log 2>&1
