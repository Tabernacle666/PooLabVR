# Windows PowerShell 5.1 syntax gate. Runs before pull/diagnostic/commit.
$ErrorActionPreference = 'Stop'
$project = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$files = @(
    'tools\DiagnosticAndCommit.ps1',
    'tools\DiagnosticCycle.ps1'
)
foreach($relative in $files) {
    $file = Join-Path $project $relative
    $tokens = $null
    $parseErrors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile(
        $file, [ref]$tokens, [ref]$parseErrors)
    if($parseErrors.Count -gt 0) {
        Write-Host ("FAIL syntax: " + $relative) -ForegroundColor Red
        foreach($e in $parseErrors) {
            Write-Host ("  " + $e.Extent.StartLineNumber + ":" +
                $e.Extent.StartColumnNumber + " " + $e.Message) -ForegroundColor Red
        }
        exit 31
    }
}
Write-Host 'PowerShell syntax: OK'
exit 0
