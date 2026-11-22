@echo off
setlocal
cd /d "%~dp0"
where py >nul 2>&1
if not errorlevel 1 (
  py -3.12 "tools\oasis_pbr_pilot.py" %*
) else (
  python "tools\oasis_pbr_pilot.py" %*
)
if errorlevel 1 (
  echo [OASIS PBR] Acquisition ou integration interrompue. Aucune validation visuelle n'est declaree.
  exit /b 1
)
node "tools\sync_oasis_mg2_catalog.mjs"
if errorlevel 1 exit /b 1
echo [OASIS PBR] Fichiers prepares. Lancer commit_and_loop.bat "mg2" puis inspecter en VR.
exit /b 0
