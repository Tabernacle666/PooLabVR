@echo off
setlocal EnableExtensions
pushd "%~dp0"
if errorlevel 1 (
  echo [OASIS PBR] Impossible d'ouvrir le dossier du projet.
  exit /b 1
)

rem "where python" alone is NOT sufficient: WindowsApps aliases may be present but inaccessible.
rem Allow an explicit OASIS_PYTHON interpreter, then try known real installations and PATH.
set "OASIS_PBR_PY="
set "OASIS_PBR_PY_ARGS="
if defined OASIS_PYTHON (
  if exist "%OASIS_PYTHON%" (
    "%OASIS_PYTHON%" -c "import sys;sys.exit(sys.version_info < (3,10))" >nul 2>&1
    if not errorlevel 1 set "OASIS_PBR_PY=%OASIS_PYTHON%"
  )
)

if not defined OASIS_PBR_PY (
  for %%P in ("%~dp0.venv\Scripts\python.exe" "D:\gitlink\Apps\Python312\python.exe" "D:\gitlink\Apps\Python313\python.exe" "%LOCALAPPDATA%\Programs\Python\Python313\python.exe" "%LOCALAPPDATA%\Programs\Python\Python312\python.exe") do (
    if not defined OASIS_PBR_PY if exist "%%~P" (
      "%%~P" -c "import sys;sys.exit(sys.version_info < (3,10))" >nul 2>&1
      if not errorlevel 1 set "OASIS_PBR_PY=%%~P"
    )
  )
)

if not defined OASIS_PBR_PY (
  for /f "delims=" %%P in ('where python.exe 2^>nul') do (
    if not defined OASIS_PBR_PY (
      "%%~P" -c "import sys;sys.exit(sys.version_info < (3,10))" >nul 2>&1
      if not errorlevel 1 set "OASIS_PBR_PY=%%~P"
    )
  )
)

if not defined OASIS_PBR_PY (
  for /f "delims=" %%P in ('where py.exe 2^>nul') do (
    if not defined OASIS_PBR_PY (
      "%%~P" -3 -c "import sys;sys.exit(sys.version_info < (3,10))" >nul 2>&1
      if not errorlevel 1 (
        set "OASIS_PBR_PY=%%~P"
        set "OASIS_PBR_PY_ARGS=-3"
      )
    )
  )
)

if not defined OASIS_PBR_PY (
  echo [OASIS PBR] Aucun Python 3.10+ executable trouve. Les alias WindowsApps non fonctionnels sont ignores.
  echo [OASIS PBR] Specifier une vraie installation avec: set "OASIS_PYTHON=D:\chemin\vers\python.exe"
  popd
  exit /b 2
)

echo [OASIS PBR] Interpreteur: "%OASIS_PBR_PY%" %OASIS_PBR_PY_ARGS%
rem No compound IF blocks around execution: CMD parentheses in error strings
rem used to terminate the batch after successful 72-GLB acquisition.
"%OASIS_PBR_PY%" %OASIS_PBR_PY_ARGS% -c "from PIL import Image" >nul 2>&1
if errorlevel 1 goto missing_pillow

rem Merge the current authored MG2 pack into the local CC0 overlay first.
rem Existing acquired scans are kept; only missing or changed meshes are added.
rem All PBR changes stay in .oasis-local; tracked source GLBs remain untouched.
set "OASIS_MG2_PACK=%~dp0.oasis-local\mg2_runtime"
cmake "-DOASIS_PYTHON=%OASIS_PBR_PY%" "-DPREPARE_PBR=OFF" "-DSOURCE_ROOT=%~dp0assets\packs\oasis_mg2_v1" "-DTARGET_ROOT=%OASIS_MG2_PACK%" -P "%~dp0tools\stage_oasis_mg2.cmake"
if errorlevel 1 goto failed_pbr

"%OASIS_PBR_PY%" %OASIS_PBR_PY_ARGS% -u "tools\oasis_pbr_pilot.py" %*
if errorlevel 1 goto failed_pbr

rem Verify the complete GLB/manifest catalogue using the existing Python.
rem Node.js is NOT a user dependency for the MG2 runtime or the PBR pipeline.
"%OASIS_PBR_PY%" %OASIS_PBR_PY_ARGS% -u "tools\mg2_hero_repair.py" "%OASIS_MG2_PACK%"
if errorlevel 1 goto failed_sync

echo [OASIS PBR] Catalogue synchronise dans .oasis-local\mg2_runtime. Git source intact.
echo [OASIS PBR] Lancer commit_and_loop.bat mg2 pour inspection.
popd
exit /b 0

:missing_pillow
echo [OASIS PBR] Pillow manque. Executer la commande suivante :
echo "%OASIS_PBR_PY%" %OASIS_PBR_PY_ARGS% -m pip install Pillow
popd
exit /b 3

:failed_pbr
set "OASIS_PBR_EXIT=%errorlevel%"
echo [OASIS PBR] Acquisition ou integration echouee. Code: %OASIS_PBR_EXIT%
popd
exit /b %OASIS_PBR_EXIT%

:failed_sync
set "OASIS_PBR_EXIT=%errorlevel%"
echo [OASIS PBR] Verification du catalogue echouee. Code: %OASIS_PBR_EXIT%
popd
exit /b %OASIS_PBR_EXIT%
