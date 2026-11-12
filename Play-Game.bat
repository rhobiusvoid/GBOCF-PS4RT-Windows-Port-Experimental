@echo off
rem GBO Code Fairy - PS4RT for Windows launcher.
rem Usage:  Play-Game.bat --game "C:\path\to\CUSA28763"   (first run; the path is remembered)
rem         Play-Game.bat                                   (later runs)
setlocal
cd /d "%~dp0"
set "PATH=%~dp0bin;%PATH%"
set "PS4RT_FSR4_DIR=%~dp0fsr4_shaders"
"%~dp0python\python.exe" "%~dp0scripts\ps4rt_launch.py" %*
if errorlevel 1 (
  echo.
  echo The launcher exited with an error. See docs\README.txt and docs\MUTEX-NOTES.md.
  pause
)
endlocal
