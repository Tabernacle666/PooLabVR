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
    if($actual -ne $expected){
        $stale=Join-Path $root ('build_stale_'+(Get-Date -Format 'yyyyMMdd_HHmmss'))
        Move-Item -LiteralPath $buildDir -Destination $stale -ErrorAction Stop
    }
}
if($Reconfigure -or $Mode -eq 'Configure' -or -not (Test-Path $cache)){
    & cmake -S . -B build -G 'Visual Studio 17 2022' -A x64
    if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
}
if($Mode -eq 'Configure'){exit 0}
$stage=Join-Path $root 'build\bin\Release'
foreach($item in @('assets','shaders','PhysXGpu_64.dll','phonon.dll','LaboVRCook.exe','source_revision.txt','WinPixEventRuntime.dll','D3D12SDKLayers.dll')){
    $path=Join-Path $stage $item
    if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path -Recurse -Force}
}
$targets=if($Target -eq 'core'){@('LaboVR','StageLaboVR')}else{@('LaboVRPackage')}
& cmake --build build --config Release --target $targets --parallel $Parallel
exit $LASTEXITCODE
