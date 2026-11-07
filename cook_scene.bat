@echo off
setlocal
if "%~2"=="" exit /b 2
"%~dp0build\bin\Release\LaboVRCook.exe" --scene "%~f1" "%~f2"
exit /b %errorlevel%
