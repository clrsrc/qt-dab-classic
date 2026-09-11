# Geraete-Rauchtest ohne Hardware: open_device{rtl_sdr} ohne Bibliothek/Stick
# muss binnen 2 s ein device_error liefern, der Prozess laeuft weiter,
# set_channel / set_gain / set_agc / start_scan ohne Geraet stuerzen nicht ab,
# get_state antwortet, shutdown beendet den Prozess mit exiting.
# Liegt eine rtlsdr.dll neben der EXE und ist ein Stick angeschlossen, wird
# stattdessen device_opened erwartet (dann close_device).
# Wird von ctest aufgerufen (DABCORED = Pfad zur EXE).
$ErrorActionPreference = 'Stop'
$exe = $env:DABCORED
if (-not $exe -or -not (Test-Path $exe)) { Write-Error "DABCORED nicht gesetzt oder nicht gefunden: $exe"; exit 2 }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.Arguments = '--no-audio'
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.WorkingDirectory = Split-Path $exe
$p = [System.Diagnostics.Process]::Start($psi)
$stderr = $p.StandardError.ReadToEndAsync()

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$p.StandardInput.WriteLine('{"type":"open_device","source":{"kind":"rtl_sdr","index":0}}')
$p.StandardInput.Flush()
# Auf device_error / device_opened warten (max. 2 s)
$answer = $null
$lines = New-Object System.Collections.Generic.List[string]
while ($sw.Elapsed.TotalSeconds -lt 2) {
    $task = $p.StandardOutput.ReadLineAsync()
    if (-not $task.Wait(200)) { continue }
    $line = $task.Result
    if ($null -eq $line) { break }
    $lines.Add($line)
    $ev = $line | ConvertFrom-Json
    if ($ev.type -eq 'device_error' -or $ev.type -eq 'device_opened') { $answer = $ev; break }
}
$tAnswer = $sw.Elapsed.TotalSeconds
if (-not $answer) { $p.Kill(); Write-Host "FEHLER: keine Antwort auf open_device binnen 2 s"; Write-Host "stderr: $($stderr.Result)"; exit 1 }
Write-Host ("open_device rtl_sdr -> {0} nach {1:N2} s: {2}" -f $answer.type, $tAnswer, $answer.message)
if ($answer.type -eq 'device_opened') { $p.StandardInput.WriteLine('{"type":"close_device"}') }

# Kommandos ohne Geraet duerfen nicht abstuerzen
$p.StandardInput.WriteLine('{"type":"set_channel","channel":"5C"}')
$p.StandardInput.WriteLine('{"type":"set_gain","gain":{"lna":32,"vga":20,"amp":false}}')
$p.StandardInput.WriteLine('{"type":"set_agc","enabled":false}')
$p.StandardInput.WriteLine('{"type":"start_scan","channels":[],"mode":"single"}')
$p.StandardInput.WriteLine('{"type":"stop_scan"}')
$p.StandardInput.WriteLine('{"type":"start_iq_dump","path":"x.uff"}')
$p.StandardInput.WriteLine('{"type":"get_state"}')
$p.StandardInput.WriteLine('{"type":"shutdown"}')
$p.StandardInput.Flush()
$p.StandardInput.Close()
if (-not $p.WaitForExit(5000)) { $p.Kill(); Write-Host "FEHLER: dabcored beendet sich nicht nach shutdown"; exit 1 }
$rest = $p.StandardOutput.ReadToEnd()
foreach ($l in ($rest -split "`n")) { if ($l.Trim() -ne '') { $lines.Add($l) } }
$types = $lines | ForEach-Object { ($_ | ConvertFrom-Json).type }
Write-Host "Ereignisse: $($types -join ', ')"
$fail = @()
if ($types[0] -ne 'ready') { $fail += 'erstes Ereignis ist nicht ready' }
if ($types -notcontains 'scan_finished') { $fail += 'kein scan_finished (start_scan ohne Geraet)' }
$state = $lines | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.type -eq 'state_snapshot' } | Select-Object -First 1
if (-not $state) { $fail += 'kein state_snapshot' }
else {
    if ($state.state.channel -ne '5C') { $fail += "state.channel = '$($state.state.channel)', erwartet 5C" }
    if ($state.state.agc -ne $false) { $fail += 'state.agc nicht false' }
    if ($state.state.gain.lna -ne 32) { $fail += "state.gain.lna = $($state.state.gain.lna), erwartet 32" }
}
if ($types[-1] -ne 'exiting') { $fail += 'letztes Ereignis ist nicht exiting' }
if ($p.ExitCode -ne 0) { $fail += "Exit-Code $($p.ExitCode)" }
if ($fail.Count -gt 0) {
    $fail | ForEach-Object { Write-Host "FEHLER: $_" }
    Write-Host "stderr: $($stderr.Result)"
    exit 1
}
Write-Host "OK"
exit 0
