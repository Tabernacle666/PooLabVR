@echo off
setlocal
cd /d "%~dp0"
rem Local XR diagnostic: reuse current build, two JPEGs, metrics, then exit.
rem Does NOT fetch, pull, commit or push. Headset may remain on the table.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\DiagnosticCycle.ps1" %*
exit /b %errorlevel%
