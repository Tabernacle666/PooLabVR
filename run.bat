@echo off
setlocal
cd /d "%~dp0build\bin\Release"
if not exist "OASIS.exe" (
  echo OASIS.exe missing. Run build.bat.
  exit /b 1
)
OASIS.exe %*
exit /b %errorlevel%
