[CmdletBinding()]
param([Parameter(ValueFromRemainingArguments=$true)][string[]]$DiagnosticArgs)

# Pull -> local VR diagnostic -> commit of this run and its relay receipt.
# Only GitHub Desktop performs the final push.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$relayScript = Join-Path $PSScriptRoot 'LocalRelay.ps1'
$diagnosticBat = Join-Path $project 'diagnostic_cycle.bat'
$receiptPath = Join-Path $project 'collaboration\diagnostic\retour.json'
$runsPath = Join-Path $project 'diagnostics\runs'

function GitRequired([string[]]$Arguments) {
    & git -C $project @Arguments
    if($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') : code $LASTEXITCODE" }
}
function RelayAvailable {
    try {
        $status = Invoke-RestMethod -Uri 'http://127.0.0.1:42716/status' -TimeoutSec 2
        return ($status.schema_version -eq 1 -and [string]$status.head -match '^[a-f0-9]{40}$')
    } catch { return $false }
}
try {
    if(-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'Git est absent du PATH.' }
    $branch = (& git -C $project branch --show-current)
    if($LASTEXITCODE -ne 0 -or $branch -ne 'main') { throw 'Le cycle doit etre execute sur la branche main.' }
    $index = @(& git -C $project diff --cached --name-only)
    if($LASTEXITCODE -ne 0 -or $index.Count -ne 0) {
        throw 'Index Git non vide : aucune donnee deja stagee ne doit entrer dans le commit diagnostic.'
    }

    if(-not (RelayAvailable)) {
        Write-Host '[0/4] Demarrage de la passerelle locale...'
        $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"' + $relayScript + '"'))
        Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -WindowStyle Minimized | Out-Null
        $ready = $false
        for($i=0;$i -lt 8;$i++) {
            Start-Sleep -Milliseconds 300
            if(RelayAvailable) { $ready = $true; break }
        }
        if(-not $ready) { Write-Warning 'Passerelle locale indisponible. Le cycle Git/diagnostic reste possible.' }
    } else { Write-Host '[0/4] Passerelle locale deja demarree.' }

    Write-Host '[1/4] git pull --ff-only origin main'
    GitRequired @('pull','--ff-only','origin','main')
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'CheckDiagnosticScripts.ps1')
    if($LASTEXITCODE -ne 0) { throw 'Syntaxe PowerShell invalide apres pull ; diagnostic non lance.' }

    # Prevent accidentally committing an old diagnostic receipt on a failed run.
    $previousRuns = @()
    if(Test-Path -LiteralPath $runsPath -PathType Container) {
        $previousRuns = @(Get-ChildItem -LiteralPath $runsPath -Directory | ForEach-Object { $_.Name })
    }
    $diagnosticStartUtc = (Get-Date).ToUniversalTime()

    Write-Host '[2/4] Diagnostic automatique...'
    & $diagnosticBat @DiagnosticArgs
    $diagnosticExit = $LASTEXITCODE
    Write-Host "Diagnostic termine avec code $diagnosticExit."
    if($diagnosticExit -eq 11) {
        Write-Host 'En attente du programmeur ou de QA : rien a commiter ni a pousser.'
        exit 11
    }

    if(-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
        throw 'Aucun recu diagnostic produit. Aucune donnee ancienne ne sera commitee.'
    }
    $receiptFile = Get-Item -LiteralPath $receiptPath
    if($receiptFile.LastWriteTimeUtc -lt $diagnosticStartUtc.AddSeconds(-2)) {
        throw 'Diagnostic sans nouveau recu : consulter son message d erreur; aucun ancien resultat ne sera reutilise.'
    }
    $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    $id = [string]$receipt.run_id
    if($id -notmatch '^[0-9]{8}_[0-9]{6}_[0-9]{3}_[0-9]+$') { throw 'Identifiant du diagnostic invalide.' }
    if($previousRuns -contains $id) { throw 'Le diagnostic n a pas cree de nouveau repertoire.' }
    $runDir = Join-Path $runsPath $id
    if(-not (Test-Path -LiteralPath $runDir -PathType Container)) { throw 'Repertoire du nouveau diagnostic absent.' }
    $manifestPath = Join-Path $runDir 'manifest.json'
    if(-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw 'Manifeste du nouveau diagnostic absent.' }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if($manifest.run_id -ne $id -or $manifest.source_commit -ne $receipt.source_sha -or
       $manifest.verification_commit_sha -ne $receipt.verification_commit_sha -or
       $manifest.cycle_id -ne $receipt.cycle_id -or $manifest.billet -ne $receipt.billet) {
        throw 'Le recu ne correspond pas au manifeste. Aucun commit.'
    }
    $expectedManifest = "diagnostics/runs/$id/manifest.json"
    if($receipt.manifest_path -ne $expectedManifest) { throw 'Chemin du manifeste inattendu.' }
    if(($diagnosticExit -eq 0) -ne ($receipt.etat -eq 'complet')) {
        throw 'Etat du diagnostic contradictoire avec son code de sortie.'
    }

    Write-Host '[3/4] Preparation des journaux, JPG et recu...'
    $files = @(Get-ChildItem -LiteralPath $runDir -Recurse -File | Where-Object {
        -not $_.FullName.StartsWith((Join-Path $runDir '_raw') + '\',[StringComparison]::OrdinalIgnoreCase)
    } | ForEach-Object {
        $_.FullName.Substring($project.Length+1).Replace('\','/')
    })
    if($files.Count -eq 0) { throw 'Aucun fichier diagnostic admissible.' }
    $allowedPrefix = "diagnostics/runs/$id/"
    $toStage = @('collaboration/diagnostic/retour.json') + $files
    & git -C $project add -f -- @toStage
    if($LASTEXITCODE -ne 0) { throw 'Impossible de preparer les fichiers du diagnostic.' }

    $staged = @(& git -C $project diff --cached --name-only)
    if($LASTEXITCODE -ne 0 -or $staged.Count -eq 0) { throw 'Aucune modification prete a etre commitee.' }
    foreach($path in $staged) {
        if($path -ne 'collaboration/diagnostic/retour.json' -and
           -not $path.StartsWith($allowedPrefix,[StringComparison]::Ordinal)) {
            throw "Fichier inattendu dans l index : $path ; aucun commit."
        }
    }

    Write-Host '[4/4] git commit -m "Diagnostic PhysX"'
    GitRequired @('commit','-m','Diagnostic PhysX')
    Write-Host ''
    Write-Host 'Commit local cree. Il reste uniquement a cliquer PUSH dans GitHub Desktop.'
    Write-Host "Resultat : $($receipt.etat) ; essai $($receipt.tentative) ; $id."
    if($diagnosticExit -ne 0) { exit $diagnosticExit }
    exit 0
} catch {
    Write-Host ('ERREUR : ' + $_.Exception.Message) -ForegroundColor Red
    Write-Host 'Aucun push effectue. Verifier les messages precedents.'
    exit 30
}
