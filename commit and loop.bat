@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 exit /b 1
echo Mise a jour du depot...
git pull --ff-only
if errorlevel 1 goto pull_failed

call "%~dp0build.bat" -Target all
if errorlevel 1 goto failed

set "OASIS_SOURCE_REVISION="
for /f "delims=" %%H in ('git rev-parse HEAD 2^>nul') do set "OASIS_SOURCE_REVISION=%%H"
set "OASIS_SOURCE_DIRTY=0"
git diff --quiet
if errorlevel 1 set "OASIS_SOURCE_DIRTY=1"
git diff --cached --quiet
if errorlevel 1 set "OASIS_SOURCE_DIRTY=1"

pushd "%~dp0build\bin\Release"
if errorlevel 1 goto failed
if not exist "OASIS.exe" (
    echo OASIS.exe introuvable apres compilation.
    popd
    goto failed
)
echo OASIS : sequence automatique natif, DLSS 50 pour cent, natif.
".\OASIS.exe" --dlss-loop --diagnostic "%~dp0diagnostics"
set "OASIS_RESULT=%errorlevel%"
popd
if "%OASIS_RESULT%"=="0" echo Capture et journal : "%~dp0diagnostics"
if not "%OASIS_RESULT%"=="0" echo Diagnostic incomplet : "%~dp0diagnostics\OASIS.log"
popd
exit /b %OASIS_RESULT%

:pull_failed
echo Mise a jour impossible. Compilation annulee.
:failed
popd
exit /b 1
