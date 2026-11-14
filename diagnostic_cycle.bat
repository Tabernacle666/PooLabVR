@echo off
setlocal
cd /d "%~dp0"
rem Empty VR shell: one diagnostic screenshot plus runtime metrics.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\DiagnosticCycle.ps1" %*
exit /b %errorlevel%
