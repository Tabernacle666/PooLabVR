@echo off
setlocal
cd /d "%~dp0"
if errorlevel 1 exit /b 30
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\DiagnosticAndCommit.ps1" %*
exit /b %errorlevel%
