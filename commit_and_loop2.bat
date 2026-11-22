@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 exit /b 1

rem Explicitly fetch before compilation, but never try a merge if main is
rem already current. PBR output is staged only in .oasis-local.
git fetch origin main
if errorlevel 1 goto failed
set "OASIS_LOCAL_SHA="
set "OASIS_REMOTE_SHA="
for /f "delims=" %%H in ('git rev-parse HEAD 2^>nul') do set "OASIS_LOCAL_SHA=%%H"
for /f "delims=" %%H in ('git rev-parse origin/main 2^>nul') do set "OASIS_REMOTE_SHA=%%H"
if not defined OASIS_LOCAL_SHA goto failed
if not defined OASIS_REMOTE_SHA goto failed
if /i "%OASIS_LOCAL_SHA%"=="%OASIS_REMOTE_SHA%" goto source_ready
git pull --ff-only
if errorlevel 1 goto update_blocked
:source_ready
call "%~dp0build.bat" -Target all
if errorlevel 1 goto failed
rem Stage the PBR runtime overlay every time, including a no-op build.
rem CMake repairs any invalid GitHub-authored hero binary in runtime staging.
cmake "-DSOURCE_ROOT=%~dp0assets\packs\oasis_mg2_v1" "-DTARGET_ROOT=%~dp0build\bin\Release\packs\oasis_mg2_v1" -P "%~dp0tools\stage_oasis_mg2.cmake"
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
if /i "%~1"=="relief" set "OASIS_LOOP_MODE=--oasis-lod-loop"
if /i "%~1"=="relief" set "OASIS_LOOP_DIR=%~dp0diagnostics\oasis-lod"
if /i "%~1"=="mg2" set "OASIS_LOOP_MODE=--mg2"
if /i "%~1"=="mg2" if not "%~2"=="" set "OASIS_MG2_FOREST=%~2"
if /i "%~1"=="mg2" set "OASIS_LOOP_DIR=%~dp0diagnostics\mg2"
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
    echo Poignee gauche et stick droit haut/bas : appel ou choix de l'arret.
    echo X gauche : gravite ou vol libre. Y gauche : capture. B droit : navette.
) else if /i "%~1"=="relief" (
    echo OASIS : 12 poses automatiques et forcees de part et d autre des seuils.
) else if /i "%~1"=="mg2" (
    echo OASIS MG2 - depart face a un poste de travail, puis deplacement libre. Echap pour quitter.
) else (
    echo Lettre O : parvis, hall, salle cannee, etage regenere, cage, B1, transfert, vide, penthouse, vitrage face/dos, baie percee et campus 3D/Relief.
)
OASIS.exe %OASIS_LOOP_MODE% --diagnostic "%OASIS_LOOP_DIR%"
set "OASIS_RESULT=%errorlevel%"
popd
if "%OASIS_RESULT%"=="0" echo Captures et journal : "%OASIS_LOOP_DIR%"
if not "%OASIS_RESULT%"=="0" echo Diagnostic incomplet : "%OASIS_LOOP_DIR%\OASIS.log"
popd
exit /b %OASIS_RESULT%

:update_blocked
echo [OASIS] Git local modifie alors que main a avance.
echo [OASIS] NE PAS restaurer les anciens GLB sur le nouveau main.
echo [OASIS] Sauvegarde: git stash push -m "PBR before hero update" -- assets/packs/oasis_mg2_v1
echo [OASIS] Puis: git pull --ff-only
echo [OASIS] Puis: pbr_lot1.bat
goto failed

:failed
popd
exit /b 1
