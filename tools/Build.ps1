param(
    [ValidateSet('core','all')]
    [string]$Target = 'all',
    [ValidateSet('Auto','Host','All','Configure')]
    [string]$Mode,
    [ValidateRange(1,32)]
    [int]$Parallel = 6,
    [switch]$Reconfigure
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$logDir = Join-Path $root 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$version = (Get-Content -Raw -LiteralPath (Join-Path $root 'VERSION.txt')).Trim()
$log = Join-Path $logDir (('build_console_' + $version + '_') + (Get-Date -Format 'yyyyMMdd_HHmmss_fff') + '_' + $PID + '.log')
$writer = New-Object System.IO.StreamWriter($log, $false, (New-Object System.Text.UTF8Encoding($true)))
$writer.AutoFlush = $true
function Write-BuildLine([string]$Line) { $writer.WriteLine($Line); Write-Host $Line }
function Invoke-CMakeLogged([string[]]$Arguments) {
    Write-BuildLine ('cmake ' + ($Arguments -join ' '))
    $saved = $ErrorActionPreference
    try { $ErrorActionPreference='Continue'; & cmake @Arguments 2>&1 | ForEach-Object { Write-BuildLine ($_.ToString()) }; $code=$LASTEXITCODE }
    finally { $ErrorActionPreference=$saved }
    Write-BuildLine ('cmake.exit=' + $code)
    return $code
}
try {
    Write-BuildLine ('ConsoleVR ' + $version + ' build started ' + (Get-Date -Format o))
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { Write-BuildLine 'ERROR: cmake is not available in PATH.'; exit 1 }
    if ($Mode) { switch ($Mode) { 'Host' {$Target='core'} 'All' {$Target='all'} 'Auto' {$Target='all'} } }
    $buildDir=Join-Path $root 'build'; $cache=Join-Path $buildDir 'CMakeCache.txt'
    if(Test-Path -LiteralPath $cache) {
        $cacheText=[System.IO.File]::ReadAllText($cache)
        $expected=[System.IO.Path]::GetFullPath($root).Replace('\','/').TrimEnd('/').ToLowerInvariant()
        $directory=[regex]::Match($cacheText,'(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$')
        $actual=if($directory.Success){$directory.Groups[1].Value.Trim().Replace('\','/').TrimEnd('/').ToLowerInvariant()}else{''}
        if($actual -ne $expected) {
            Write-BuildLine ("STALE CMAKE CACHE: expected=$expected actual=$actual")
            $stale=Join-Path $root ('build_stale_'+(Get-Date -Format 'yyyyMMdd_HHmmss'))
            Move-Item -LiteralPath $buildDir -Destination $stale -ErrorAction Stop
            Write-BuildLine ("Archived incompatible cache at $stale")
        }
    }
    if ($Reconfigure -or $Mode -eq 'Configure' -or -not (Test-Path $cache)) {
        $r=Invoke-CMakeLogged @('-S','.', '-B','build','-G','Visual Studio 17 2022','-A','x64')
        if ($r -ne 0) { exit $r }
    }
    if ($Mode -eq 'Configure') { exit 0 }
    $targets = if ($Target -eq 'core') { @('LaboVR','StageLaboVR') } else { @('LaboVRPackage') }
    $r=Invoke-CMakeLogged (@('--build','build','--config','Release','--target') + $targets + @('--parallel',[string]$Parallel))
    exit $r
} catch { Write-BuildLine ('ERROR: '+$_.ToString()); exit 1 } finally { $writer.Dispose() }
