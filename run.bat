@echo off
setlocal
set "OASIS_DIAGNOSTICS=%~dp0diagnostics"
if not exist "%OASIS_DIAGNOSTICS%\." mkdir "%OASIS_DIAGNOSTICS%"
if not exist "%OASIS_DIAGNOSTICS%\." (
  echo Impossible de creer "%OASIS_DIAGNOSTICS%".
  exit /b 1
)
echo Journal de session : "%OASIS_DIAGNOSTICS%\OASIS.log"
rem PBR authoring modifies GLBs without rebuilding OASIS.exe.
rem Synchronize on normal run, not just on commit_and_loop.
if exist "%~dp0assets\packs\oasis_mg2_v1\pack.json" (
  cmake -E copy_directory_if_different "%~dp0assets\packs\oasis_mg2_v1" "%~dp0build\bin\Release\packs\oasis_mg2_v1"
  if errorlevel 1 (
    echo [OASIS] Impossible de synchroniser le pack MG2.
    exit /b 1
  )
)
pushd "%~dp0build\bin\Release"
if errorlevel 1 exit /b 1
if not exist "OASIS.exe" (
  echo OASIS.exe missing. Run build.bat.
  popd
  exit /b 1
)
set "OASIS_SOURCE_REVISION="
for /f "delims=" %%H in ('git rev-parse HEAD 2^>nul') do set "OASIS_SOURCE_REVISION=%%H"
set "OASIS_SOURCE_DIRTY=0"
git diff --quiet
if errorlevel 1 set "OASIS_SOURCE_DIRTY=1"
git diff --cached --quiet
if errorlevel 1 set "OASIS_SOURCE_DIRTY=1"
echo OASIS : capture automatique apres 10 secondes. Echap pour quitter.
OASIS.exe --interactive %* --diagnostic "%OASIS_DIAGNOSTICS%"
set "OASIS_RESULT=%errorlevel%"
popd
if "%OASIS_RESULT%"=="0" echo Capture et journal : "%~dp0diagnostics"
if not "%OASIS_RESULT%"=="0" echo Diagnostic incomplet : "%~dp0diagnostics\OASIS.log"
exit /b %OASIS_RESULT%
