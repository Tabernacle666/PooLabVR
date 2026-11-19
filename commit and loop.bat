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

set "OASIS_LOOP_MODE=--sphere-baseline"
set "OASIS_LOOP_NAME=sphere-avant"
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
if "%OASIS_LOOP_MODE%"=="--sphere-baseline" echo OASIS : reference VR native, puis reperes A a F dans le casque.
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
