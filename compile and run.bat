@echo off
setlocal
(
    cd /d "%~dp0"
    echo Mise a jour du depot...
    git pull --ff-only
    if errorlevel 1 (
        echo Mise a jour impossible. Compilation annulee.
        exit /b 1
    )
    call build.bat -Target all
    if errorlevel 1 exit /b 1
    call run.bat %*
    exit /b
)
