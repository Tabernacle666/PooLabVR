param(
    [ValidateSet('core','all')]
    [string]$Target='all',
    [ValidateSet('Auto','Host','All','Configure')]
    [string]$Mode,
    [ValidateRange(1,32)]
    [int]$Parallel=6,
    [switch]$Reconfigure
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=Split-Path $PSScriptRoot -Parent
Set-Location $root
$logDir=Join-Path $root 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log=Join-Path $logDir ('OASIS-build_'+(Get-Date -Format 'yyyyMMdd_HHmmss_fff')+'_'+$PID+'.log')
$writer=New-Object System.IO.StreamWriter($log,$false,(New-Object System.Text.UTF8Encoding($true)))
$writer.AutoFlush=$true
# Keep all warnings in the log; deduplicate only the console.
$seenWarnings = New-Object 'System.Collections.Generic.HashSet[string]'
$script:duplicateWarnings = 0
function Write-BuildLine([string]$line){
    $writer.WriteLine($line)
    if($line -match '(?i)\bwarning\s+(?:[A-Z]+\d+):'){
        $key = $line.Trim() -replace '\s+\[[^\]]+\]\s*$',''
        if(-not $seenWarnings.Add($key)){
            $script:duplicateWarnings++
            return
        }
    }
    Write-Host $line
}
function Invoke-CMakeLogged([string[]]$arguments){
    $saved=$ErrorActionPreference
    try{
        $ErrorActionPreference='Continue'
        & cmake @arguments 2>&1 | ForEach-Object {Write-BuildLine ($_.ToString())}
        $code=$LASTEXITCODE
    }finally{$ErrorActionPreference=$saved}
    return $code
}
try{
if(-not (Get-Command cmake -ErrorAction SilentlyContinue)){
    throw 'CMake executable unavailable'
}
if($Mode){
    switch($Mode){
        'Host' {$Target='core'}
        'All' {$Target='all'}
        'Auto' {$Target='all'}
    }
}
$buildDir=Join-Path $root 'build'
$cache=Join-Path $buildDir 'CMakeCache.txt'
if(Test-Path -LiteralPath $cache){
    $cacheText=[System.IO.File]::ReadAllText($cache)
    $expected=[System.IO.Path]::GetFullPath($root).Replace('\','/').TrimEnd('/').ToLowerInvariant()
    $directory=[regex]::Match($cacheText,'(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$')
    $actual=if($directory.Success){$directory.Groups[1].Value.Trim().Replace('\','/').TrimEnd('/').ToLowerInvariant()}else{''}
    $project=[regex]::Match($cacheText,'(?m)^CMAKE_PROJECT_NAME:STATIC=(.+)$')
    if(-not $project.Success -or $project.Groups[1].Value.Trim() -ne 'OASIS'){$Reconfigure=$true}
    if($actual -ne $expected){
        $stale=Join-Path $root ('build_stale_'+(Get-Date -Format 'yyyyMMdd_HHmmss'))
        Move-Item -LiteralPath $buildDir -Destination $stale -ErrorAction Stop
    }
}
if($Reconfigure -or $Mode -eq 'Configure' -or -not (Test-Path $cache)){
    $code=Invoke-CMakeLogged @('-S','.','-B','build','-G','Visual Studio 17 2022','-A','x64')
    if($code -ne 0){exit $code}
}
if($Mode -eq 'Configure'){exit 0}
$stage=Join-Path $root 'build\bin\Release'
foreach($item in @('shaders')){
    $path=Join-Path $stage $item
    if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path -Recurse -Force}
}
$targets=if($Target -eq 'core'){@('OASIS','StageOASIS')}else{@('OASISPackage')}
$code=Invoke-CMakeLogged (@('--build','build','--config','Release','--target')+$targets+@('--parallel',[string]$Parallel))
exit $code
}catch{
    Write-BuildLine ('ERROR: '+$_.ToString())
    exit 1
}finally{
    if($script:duplicateWarnings -gt 0){Write-Host "Avertissements identiques masques : $script:duplicateWarnings"}
    Write-Host "Journal complet : $log"
    $writer.Dispose()
}
