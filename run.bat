@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\RunSession.ps1" -Mode "%~1"
exit /b %errorlevel%
