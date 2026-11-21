@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 exit /b 1

git pull --ff-only
if errorlevel 1 goto failed
call "%~dp0build.bat" -Target all
if errorlevel 1 goto failed

set "OASIS_SOURCE_REVISION="
for /f "delims=" %%H in ('git rev-parse HEAD 2^>nul') do set "OASIS_SOURCE_REVISION=%%H"
set "OASIS_SOURCE_DIRTY=0"
git diff --quiet
if errorlevel 1 set "OASIS_SOURCE_DIRTY=1"
git diff --cached --quiet
if errorlevel 1 set "OASIS_SOURCE_DIRTY=1"

set "OASIS_LOOP_DIR=%~dp0diagnostics\megastructure"
set "OASIS_LOOP_MODE=--building-loop"
if /i "%~1"=="live" set "OASIS_LOOP_MODE=--building --interactive"
if /i "%~1"=="live" set "OASIS_LOOP_DIR=%~dp0diagnostics\megastructure-libre"
for /f "delims=" %%T in ('powershell.exe -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss-fff"') do set "OASIS_LOOP_DIR=%OASIS_LOOP_DIR%\%%T"
pushd "%~dp0build\bin\Release"
if errorlevel 1 goto failed
if not exist OASIS.exe (
    popd
    goto failed
)
if /i "%~1"=="live" (
    echo Phosphor : main droite vide, saisie de Couleur puis stick vers le haut pour ouvrir Generation.
    echo Le meme geste ouvre Platrier. Viser main vide, Reglage choisit le profil, S valide.
    echo Selection choisit la portee, Reglage choisit l'action, S valide.
    echo X/Y a gauche : appel pres de la gaine, puis arret precedent/suivant dans la cabine.
    echo Poignee gauche et clic du stick gauche : appui au sol ou vol libre.
) else (
    echo Lettre O : parvis, hall, salle cannee, etage regenere, cage, B1, transfert, vide, penthouse, vitrage face/dos.
)
OASIS.exe %OASIS_LOOP_MODE% --diagnostic "%OASIS_LOOP_DIR%"
set "OASIS_RESULT=%errorlevel%"
popd
if "%OASIS_RESULT%"=="0" echo Captures et journal : "%OASIS_LOOP_DIR%"
if not "%OASIS_RESULT%"=="0" echo Diagnostic incomplet : "%OASIS_LOOP_DIR%\OASIS.log"
popd
exit /b %OASIS_RESULT%

:failed
popd
exit /b 1
