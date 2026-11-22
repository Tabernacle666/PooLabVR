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
"%OASIS_PBR_PY%" %OASIS_PBR_PY_ARGS% -c "from PIL import Image" >nul 2>&1
if errorlevel 1 (
  echo [OASIS PBR] Pillow est absent dans cet interpreteur. Installer avec:
  echo "%OASIS_PBR_PY%" %OASIS_PBR_PY_ARGS% -m pip install Pillow
  popd
  exit /b 3
)

"%OASIS_PBR_PY%" %OASIS_PBR_PY_ARGS% -u "tools\oasis_pbr_pilot.py" %*
set "OASIS_PBR_EXIT=%errorlevel%"
if not "%OASIS_PBR_EXIT%"=="0" (
  echo [OASIS PBR] Acquisition ou integration interrompue (code %OASIS_PBR_EXIT%). Aucune validation visuelle n'est declaree.
  popd
  exit /b %OASIS_PBR_EXIT%
)

where node.exe >nul 2>&1
if errorlevel 1 (
  echo [OASIS PBR] Node.js introuvable: lancer node tools\sync_oasis_mg2_catalog.mjs apres installation.
  popd
  exit /b 4
)

node "tools\sync_oasis_mg2_catalog.mjs"
set "OASIS_PBR_EXIT=%errorlevel%"
if not "%OASIS_PBR_EXIT%"=="0" (
  echo [OASIS PBR] Synchronisation MG2 echouee (code %OASIS_PBR_EXIT%).
  popd
  exit /b %OASIS_PBR_EXIT%
)

echo [OASIS PBR] Fichiers prepares. Lancer commit_and_loop.bat "mg2" puis inspecter en VR.
popd
exit /b 0
