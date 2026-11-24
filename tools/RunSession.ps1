param([switch]$Build, [string]$Mode = 'cosmos')
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Mode)) { $Mode = 'cosmos' }
$Mode = $Mode.ToLowerInvariant()
if ($Mode -notin @('cosmos','foret','hall','loop')) {
    Write-Host 'Usage: pull_compile_run.bat ou run.bat cosmos|foret|hall|loop'
    exit 2
}
$repo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program a echoue ($LASTEXITCODE)" }
}
try {
    $branch = (& git branch --show-current).Trim()
    if ($LASTEXITCODE -ne 0 -or $branch -ne 'main') { throw 'Ce lanceur exige main.' }
    if ($Build) {
        Invoke-Checked git @('pull','--ff-only','origin','main')
        Invoke-Checked powershell.exe @('-NoProfile','-ExecutionPolicy','Bypass','-File', (Join-Path $PSScriptRoot 'Build.ps1'),'-Target','all')
    }
    $bin = Join-Path $repo 'build\bin\Release'
    $exe = Join-Path $bin 'OASIS.exe'
    if (!(Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'OASIS.exe absent. Utiliser pull_compile_run.bat.' }
    $sourcePack = Join-Path $repo 'assets\packs\oasis_mg2_v1'
    if (!$Build -and (Test-Path (Join-Path $sourcePack 'pack.json'))) {
        Invoke-Checked cmake @("-DSOURCE_ROOT=$sourcePack", "-DTARGET_ROOT=$bin\packs\oasis_mg2_v1", '-P', (Join-Path $PSScriptRoot 'stage_oasis_mg2.cmake'))
    }
    $dress = Join-Path $repo 'assets\packs\oasis_dressing_v1'
    if (!$Build -and (Test-Path (Join-Path $dress 'wear_rules.json'))) {
        Invoke-Checked cmake @('-E','copy_directory_if_different',$dress,"$bin\packs\oasis_dressing_v1")
    }
    $checkoutRevision = (& git rev-parse HEAD).Trim()
    $buildIdentity = Join-Path $bin 'OASIS.session-revision.json'
    $exeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash
    if ($Build) {
        @{revision=$checkoutRevision;sha256=$exeHash} | ConvertTo-Json | Set-Content -LiteralPath $buildIdentity -Encoding UTF8
    }
    $env:OASIS_SOURCE_REVISION = 'unknown'
    if (Test-Path -LiteralPath $buildIdentity) {
        $identity = Get-Content -LiteralPath $buildIdentity -Raw | ConvertFrom-Json
        if ($identity.sha256 -eq $exeHash) { $env:OASIS_SOURCE_REVISION = $identity.revision }
    }
    if ($env:OASIS_SOURCE_REVISION -ne $checkoutRevision) {
        Write-Host 'Attention : executable non identifie ou different du HEAD. Compilation volontairement ignoree.'
    }
    $env:OASIS_SOURCE_DIRTY = '0'
    & git diff --quiet
    if ($LASTEXITCODE -ne 0) { $env:OASIS_SOURCE_DIRTY = '1' }
    & git diff --cached --quiet
    if ($LASTEXITCODE -ne 0) { $env:OASIS_SOURCE_DIRTY = '1' }
    $env:OASIS_SESSION_MODE = $Mode
    if ([string]::IsNullOrWhiteSpace($env:OASIS_BAKE_CACHE)) {
        $env:OASIS_BAKE_CACHE = Join-Path $repo '.oasis-local\cache\session-bake'
    }
    New-Item -ItemType Directory -Force -Path $env:OASIS_BAKE_CACHE | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $relative = "diagnostics/sessions/$stamp-$Mode-$PID"
    $folder = Join-Path $repo $relative
    New-Item -ItemType Directory -Path $folder | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $folder 'live') | Out-Null
    Write-Host "Session : $folder"
    $process = Start-Process -FilePath $exe -WorkingDirectory $bin -ArgumentList @('--interactive','--diagnostic',('"' + $folder + '"')) -Wait -PassThru
    $code = $process.ExitCode
    $metadata = [ordered]@{mode=$Mode;revision=$env:OASIS_SOURCE_REVISION;checkout_revision=$checkoutRevision;exe_sha256=$exeHash;dirty=$env:OASIS_SOURCE_DIRTY;exit_code=$code;date=(Get-Date).ToString('o')}
    $metadata | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $folder 'session.json') -Encoding UTF8
    Write-Host "Fin OASIS ($code). Journaux et captures : $folder"
    $comment = Read-Host 'Commentaire du commit local (Entree = commentaire automatique)'
    if ([string]::IsNullOrWhiteSpace($comment)) { $comment = "OASIS session $Mode $stamp (exit $code)" }
    $files = @(Get-ChildItem -LiteralPath $folder -File -Recurse | Where-Object { $_.Extension -in '.log','.png','.jpg','.json','.csv' } | ForEach-Object { $_.FullName })
    if ($files.Count -eq 0) { throw 'Aucun resultat a commiter.' }
    Invoke-Checked git (@('add','-f','--') + $files)
    $messageFile = [IO.Path]::GetTempFileName()
    try {
        [IO.File]::WriteAllText($messageFile,$comment,[Text.UTF8Encoding]::new($false))
        # --only excludes all unrelated staged files.
        Invoke-Checked git (@('commit','--only','-F',$messageFile,'--') + $files)
    } finally { Remove-Item -LiteralPath $messageFile -Force -ErrorAction SilentlyContinue }
    Write-Host 'Commit LOCAL termine. Aucun push automatique. Publier via GitHub Desktop.'
    exit $code
} catch {
    Write-Host "[OASIS] $($_.Exception.Message)"
    exit 1
}
