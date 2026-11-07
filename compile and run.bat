@echo off
setlocal
cd /d "%~dp0"
call build.bat -Target all
if errorlevel 1 exit /b %errorlevel%
call run.bat %*
exit /b %errorlevel%
