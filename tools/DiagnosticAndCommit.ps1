[CmdletBinding()]
param([Parameter(ValueFromRemainingArguments=$true)][string[]]$DiagnosticArgs)

# Pull -> build if stale -> two viewpoints -> JPG/log -> commit this one run.
# The user performs the final GitHub push from their PC.
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$project=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$diagnosticBat=Join-Path $project 'diagnostic_cycle.bat'
$latest=Join-Path $project 'diagnostics\latest.json'
$runs=Join-Path $project 'diagnostics\runs'

function GitRequired([string[]]$Arguments) {
    & git -C $project @Arguments
    if($LASTEXITCODE -ne 0) {throw "git $($Arguments -join ' ') failed ($LASTEXITCODE)"}
}
try {
    if(-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'Git is not on PATH.' }
    $branch=(& git -C $project branch --show-current)
    if($LASTEXITCODE -ne 0 -or $branch -ne 'main') {throw 'Diagnostic requires main.'}
    $stagedBefore=@(& git -C $project diff --cached --name-only)
    if($LASTEXITCODE -ne 0 -or $stagedBefore.Count -ne 0) {
        throw 'Git index is not empty. Existing staged files will not be included.'
    }
    Write-Host '[1/4] Pull fast-forward main'
    GitRequired @('pull','--ff-only','origin','main')
    Write-Host '[2/4] Verify PowerShell syntax'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'CheckDiagnosticScripts.ps1')
    if($LASTEXITCODE -ne 0) {throw 'Diagnostic script syntax check failed.'}
    $head=(& git -C $project rev-parse HEAD)
    if($LASTEXITCODE -ne 0 -or $head -notmatch '^[0-9a-f]{40}$') {throw 'HEAD unavailable.'}
    $oldRunId=$null
    if(Test-Path -LiteralPath $latest -PathType Leaf){
        try {$oldRunId=[string](Get-Content -LiteralPath $latest -Raw|ConvertFrom-Json).run_id} catch {}
    }
    $started=(Get-Date).ToUniversalTime()
    Write-Host '[3/4] Run Windows/OpenXR diagnostic'
    & $diagnosticBat @DiagnosticArgs
    $diagnosticExit=$LASTEXITCODE
    if(-not (Test-Path -LiteralPath $latest -PathType Leaf)) {
        throw "No diagnostic receipt generated; exit=$diagnosticExit."
    }
    $receiptFile=Get-Item -LiteralPath $latest
    if($receiptFile.LastWriteTimeUtc -lt $started.AddSeconds(-2)) {
        throw 'Only an old diagnostic receipt was found; no commit.'
    }
    $receipt=Get-Content -LiteralPath $latest -Raw|ConvertFrom-Json
    $id=[string]$receipt.run_id
    if($id -notmatch '^[0-9]{8}_[0-9]{6}_[0-9]{3}_[0-9]+$' -or $id -eq $oldRunId) {
        throw 'Diagnostic run ID missing or reused.'
    }
    if($receipt.source_sha -ne $head) {
        throw 'Diagnostic receipt does not match the pulled HEAD.'
    }
    $runDir=Join-Path $runs $id
    $manifestPath=Join-Path $runDir 'manifest.json'
    if(-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {throw 'Manifest missing.'}
    $manifest=Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json
    if($manifest.run_id -ne $id -or $manifest.source_commit -ne $head -or
       $receipt.manifest_path -ne "diagnostics/runs/$id/manifest.json") {
        throw 'Manifest and diagnostic receipt disagree.'
    }
    if(($diagnosticExit -eq 0) -ne ($manifest.complete -and $receipt.status -eq 'complete')) {
        throw 'Diagnostic exit code and result disagree.'
    }
    $items=@(Get-ChildItem -LiteralPath $runDir -Recurse -File | Where-Object {
        -not $_.FullName.StartsWith((Join-Path $runDir '_raw')+'\',
                                     [StringComparison]::OrdinalIgnoreCase)
    } | ForEach-Object {
        $_.FullName.Substring($project.Length+1).Replace('\','/')
    })
    if($items.Count -eq 0) {throw 'Diagnostic run has no files.'}
    $files=@('diagnostics/latest.json')+$items
    Write-Host '[4/4] Commit only the new diagnostic'
    & git -C $project add -f -- @files
    if($LASTEXITCODE -ne 0) {throw 'Could not stage diagnostic files.'}
    $staged=@(& git -C $project diff --cached --name-only)
    if($LASTEXITCODE -ne 0 -or $staged.Count -eq 0) {throw 'No staged diagnostic files.'}
    $prefix="diagnostics/runs/$id/"
    foreach($path in $staged) {
        if($path -ne 'diagnostics/latest.json' -and
           -not $path.StartsWith($prefix,[StringComparison]::Ordinal)) {
            throw "Unexpected staged file: $path."
        }
    }
    GitRequired @('commit','-m',"Urban diagnostic $id")
    Write-Host ("Run: "+$id+" ; result: "+$receipt.status+" ; source: "+$head)
    Write-Host 'A local commit was created. Push in GitHub Desktop to deliver JPGs/logs.'
    exit $diagnosticExit
} catch {
    Write-Host ('DIAGNOSTIC ERROR: '+$_.Exception.Message) -ForegroundColor Red
    Write-Host 'No source or unrelated changes were committed.'
    exit 30
}
