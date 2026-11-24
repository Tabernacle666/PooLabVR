@echo off
setlocal
set "OASIS_MODE=%~1"
if not defined OASIS_MODE set "OASIS_MODE=cosmos"
if not "%~2"=="" set "OASIS_DEPTH_MODE=%~2"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\RunSession.ps1" -Mode "%OASIS_MODE%"
exit /b %errorlevel%
