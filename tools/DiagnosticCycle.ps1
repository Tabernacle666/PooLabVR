[CmdletBinding()]
param([ValidateRange(15,600)][int]$Seconds = 20,[switch]$GpuValidation)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe=Join-Path $root 'build\bin\Release\LaboVR.exe'
if(-not (Test-Path -LiteralPath $exe)){throw 'Executable missing: run build.bat'}
$run=Join-Path $root ('diagnostics\runs\'+(Get-Date -Format 'yyyyMMdd_HHmmss_fff'))
New-Item -ItemType Directory -Force -Path $run | Out-Null
$log=Join-Path $run 'runtime.log'
$old=$env:CONSOLEVR_CAPTURE_DIR
try {
    $env:CONSOLEVR_CAPTURE_DIR=$run
    $args=@('--log',$log,'--diagnostic-sweep',[string]$Seconds)
    if($GpuValidation){$args+='--gpu-validation'}
    & $exe @args
    $result=$LASTEXITCODE
} finally {$env:CONSOLEVR_CAPTURE_DIR=$old}
Write-Host "Diagnostic: $run"
Write-Host "Runtime log: $log"
Get-ChildItem -LiteralPath $run -File -Filter '*.png' | ForEach-Object {Write-Host "Screenshot: $($_.FullName)"}
exit $result
