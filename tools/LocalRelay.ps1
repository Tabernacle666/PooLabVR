[CmdletBinding()]
param([ValidateRange(1024,65535)][int]$Port = 42716)
# Read-only loopback endpoint for RelaisAgents. No Git mutation, no network exposure.
$ErrorActionPreference = 'Stop'
$project = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback,$Port)
$listener.Start()
Write-Host "CooLabVR local relay: http://127.0.0.1:$Port/status (Ctrl+C to stop)"
try {
  while($true) {
    $client = $listener.AcceptTcpClient()
    try {
      $client.ReceiveTimeout = 2000
      $client.SendTimeout = 2000
      $stream = $client.GetStream()
      $reader = [System.IO.StreamReader]::new($stream,[System.Text.Encoding]::ASCII,$false,1024,$true)
      $request = $reader.ReadLine()
      $headers = 0
      do { $line = $reader.ReadLine(); $headers++ } while($null -ne $line -and $line.Length -gt 0 -and $headers -lt 60)
      $ok = $request -match '^GET /status HTTP/1\.[01]$'
      if($ok) {
        $head = (& git -C $project rev-parse HEAD 2>$null)
        $reflog = (& git -C $project reflog -1 --format=%gs HEAD 2>$null)
        $localPath = Join-Path $project '.git\relay_local_status.json'
        $diag = $null
        if(Test-Path -LiteralPath $localPath) {
          try { $diag = Get-Content -LiteralPath $localPath -Raw | ConvertFrom-Json } catch {}
        }
        $body = @{schema_version=1; head=[string]$head; last_head_action=[string]$reflog;
          diagnostic=$diag; observed_utc=(Get-Date).ToUniversalTime().ToString('o')} | ConvertTo-Json -Depth 6 -Compress
        $status = '200 OK'
      } else {
        $body = '{"error":"route inconnue"}'
        $status = '404 Not Found'
      }
      $data = [System.Text.Encoding]::UTF8.GetBytes($body)
      $reply = "HTTP/1.1 $status`r`nContent-Type: application/json; charset=utf-8`r`nAccess-Control-Allow-Origin: *`r`nCache-Control: no-store`r`nContent-Length: $($data.Length)`r`nConnection: close`r`n`r`n"
      $prefix = [System.Text.Encoding]::ASCII.GetBytes($reply)
      $stream.Write($prefix,0,$prefix.Length)
      $stream.Write($data,0,$data.Length)
      $stream.Flush()
    } catch { Write-Warning $_.Exception.Message }
    finally { $client.Close() }
  }
} finally { $listener.Stop() }
