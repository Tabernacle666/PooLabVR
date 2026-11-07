@echo off
setlocal
cd /d "%~dp0"
if not exist "logs" mkdir "logs"
if not exist "logs" exit /b 1
for /f "usebackq delims=" %%V in ("VERSION.txt") do set "LAB_VERSION=%%V"
set "LAB_STAMP="
for /f %%T in ('powershell.exe -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss_fff"') do set "LAB_STAMP=%%T"
if not defined LAB_STAMP set "LAB_STAMP=session"
set "LAB_LOG=%~dp0logs\LaboVR_%LAB_VERSION%_%LAB_STAMP%_%RANDOM%.log"
>"%LAB_LOG%" echo LaboVR %LAB_VERSION% launcher=run.bat started=%DATE% %TIME%
if errorlevel 1 exit /b 1
echo Journal : "%LAB_LOG%"
if not exist "%~dp0build\bin\Release\LaboVR.exe" (
  >>"%LAB_LOG%" echo ERROR: build\bin\Release\LaboVR.exe missing. Run compile and run.bat.
  echo Executable absent. Utiliser compile and run.bat.
  exit /b 1
)
cd /d "%~dp0build\bin\Release"
LaboVR.exe --log "%LAB_LOG%" %*
set "LAB_RESULT=%ERRORLEVEL%"
>>"%LAB_LOG%" echo launcher.exit=%LAB_RESULT% finished=%DATE% %TIME%
exit /b %LAB_RESULT%
