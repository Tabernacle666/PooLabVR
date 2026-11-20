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

set "OASIS_LOOP_MODE=--lunar"
set "OASIS_LOOP_NAME=lune"
if /i "%~1"=="passage" (
    set "OASIS_LOOP_MODE=--solar-passage"
    set "OASIS_LOOP_NAME=franchissement"
)
if /i "%~1"=="solar" (
    set "OASIS_LOOP_MODE=--solar-system"
    set "OASIS_LOOP_NAME=solar-system"
)
if /i "%~1"=="phase1" (
    set "OASIS_LOOP_MODE=--sphere-baseline"
    set "OASIS_LOOP_NAME=sphere-phase1"
)
if /i "%~1"=="phase2" (
    set "OASIS_LOOP_MODE=--sphere-phase2"
    set "OASIS_LOOP_NAME=sphere-phase2"
)
if /i "%~1"=="phase3" (
    set "OASIS_LOOP_MODE=--sphere-phase3"
    set "OASIS_LOOP_NAME=sphere-phase3"
)
if /i "%~1"=="dlss" (
    set "OASIS_LOOP_MODE=--dlss-loop"
    set "OASIS_LOOP_NAME=dlss-loop"
)
set "OASIS_LOOP_DIR=%~dp0diagnostics\%OASIS_LOOP_NAME%"
if not exist "%~dp0diagnostics\." mkdir "%~dp0diagnostics"
pushd "%~dp0build\bin\Release"
if errorlevel 1 goto failed
if not exist "OASIS.exe" (
    echo OASIS.exe introuvable apres compilation.
    popd
    goto failed
)
if "%OASIS_LOOP_MODE%"=="--sphere-baseline" echo OASIS : mesure du relief et du relief allege en C, puis comparaison visuelle dans les zones A a C.
if "%OASIS_LOOP_MODE%"=="--sphere-phase2" echo OASIS : deux spheres et orbites A a C, une qualite fixe par passage.
if "%OASIS_LOOP_MODE%"=="--sphere-phase3" echo OASIS : cent spheres, 6 s de chauffe puis 3 s par pose.
if "%OASIS_LOOP_MODE%"=="--solar-system" echo OASIS : neuf orbites, 6 s de chauffe puis 3 s par pose, outil et gun dans le champ.
if "%OASIS_LOOP_MODE%"=="--solar-passage" echo OASIS : 6 s de chauffe, puis entree, repos a bord et sortie. Outil et gun dans le champ.
if "%OASIS_LOOP_MODE%"=="--lunar" echo OASIS : 6 s de chauffe, puis Lune en Relief, en 3D et en interieur x12. Outil et gun dans le champ.
if "%OASIS_LOOP_MODE%"=="--dlss-loop" echo OASIS : sequence automatique natif, DLSS 50 pour cent, natif.
".\OASIS.exe" %OASIS_LOOP_MODE% --diagnostic "%OASIS_LOOP_DIR%"
set "OASIS_RESULT=%errorlevel%"
popd
if not "%OASIS_RESULT%"=="0" goto incomplete
echo Capture et journal : "%OASIS_LOOP_DIR%"
powershell.exe -NoProfile -Command "$folder = 'diagnostics/' + $env:OASIS_LOOP_NAME; $files = ($folder + '/OASIS.log'), ($folder + '/OASIS.png'); $message = Read-Host 'Commentaire du commit local des resultats (vide pour fermer)'; if ([string]::IsNullOrWhiteSpace($message)) { exit 0 }; git add --intent-to-add -- @files; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; $file = [IO.Path]::GetTempFileName(); try { [IO.File]::WriteAllText($file, $message, [Text.UTF8Encoding]::new($false)); git commit --only -F $file -- @files; $result = $LASTEXITCODE } finally { Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue }; exit $result"
set "OASIS_RESULT=%errorlevel%"
popd
exit /b %OASIS_RESULT%

:incomplete
echo Diagnostic incomplet : "%OASIS_LOOP_DIR%\OASIS.log"
popd
exit /b %OASIS_RESULT%

:pull_failed
echo Mise a jour impossible. Compilation annulee.
:failed
popd
exit /b 1
