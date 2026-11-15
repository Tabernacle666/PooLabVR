@echo off
setlocal
cd /d "%~dp0build\bin\Release"
if not exist "LaboVR.exe" (
  echo LaboVR.exe missing. Run build.bat.
  exit /b 1
)
LaboVR.exe %*
exit /b %errorlevel%
