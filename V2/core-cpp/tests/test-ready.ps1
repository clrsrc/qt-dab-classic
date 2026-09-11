# Rauchtest: dabcored startet, meldet "ready", beantwortet get_state und
# beendet sich bei shutdown. Wird von ctest aufgerufen (DABCORED = Pfad).
$ErrorActionPreference = 'Stop'
$exe = $env:DABCORED
if (-not $exe -or -not (Test-Path $exe)) { Write-Error "DABCORED nicht gesetzt oder nicht gefunden: $exe"; exit 2 }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.Arguments = '--no-audio'
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.WorkingDirectory = Split-Path $exe
$p = [System.Diagnostics.Process]::Start($psi)

$p.StandardInput.WriteLine('{"type":"get_state"}')
$p.StandardInput.WriteLine('{"type":"shutdown"}')
$p.StandardInput.Close()
if (-not $p.WaitForExit(5000)) { $p.Kill(); Write-Error "dabcored beendet sich nicht"; exit 1 }
$out = $p.StandardOutput.ReadToEnd()

$lines = $out -split "`n" | Where-Object { $_.Trim() -ne '' }
$types = $lines | ForEach-Object { ($_ | ConvertFrom-Json).type }
Write-Host "Ereignisse: $($types -join ', ')"
if ($types[0] -ne 'ready') { Write-Error "erstes Ereignis ist nicht ready"; exit 1 }
if ($types -notcontains 'state_snapshot') { Write-Error "kein state_snapshot"; exit 1 }
if ($types[-1] -ne 'exiting') { Write-Error "letztes Ereignis ist nicht exiting"; exit 1 }
Write-Host "OK"
exit 0
