# Audio-Test: dabcored dekodiert den Dienst "Dlf" aus der 60-s-Referenzdatei
# (--fast, 20 s Dateizeit, kein Audio-Sink) und schreibt den WAV-Dump.
# Erwartet: service_started mit he_aac, WAV ~ 20 s * 48000 * 2 * 2 Byte
# (Toleranz -15 %, der Dienst startet erst nach Sync/FIC), mindestens ein
# dls, service_stats, Prozessende mit exiting. Zweiter Fall: shutdown per
# stdin nach 3 s bei laufendem Dienst (Echtzeit) -> Ende binnen 3 s.
# Wird von ctest aufgerufen (DABCORED = Pfad zur EXE); fehlt die
# Referenzdatei, wird der Test uebersprungen (Exit 0 mit Hinweis).
$ErrorActionPreference = 'Stop'
$exe = $env:DABCORED
if (-not $exe -or -not (Test-Path $exe)) { Write-Error "DABCORED nicht gesetzt oder nicht gefunden: $exe"; exit 2 }

$file = $env:DABCORE_REPLAY_FILE
if (-not $file) { $file = 'P:\Projekte\DAB\Warntag-2026\test\final-l32-g40-60s.uff' }
if (-not (Test-Path $file)) {
    Write-Host "SKIP: Referenzdatei fehlt: $file"
    exit 0
}

function Start-Core([string]$arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = $arguments
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.WorkingDirectory = Split-Path $exe
    return [System.Diagnostics.Process]::Start($psi)
}

$tmpDir = [System.IO.Path]::GetTempPath()
$tag = [System.IO.Path]::GetRandomFileName()
$events = Join-Path $tmpDir "dabcore-audio-$tag.jsonl"
$wav = Join-Path $tmpDir "dabcore-audio-$tag.wav"
$fail = @()

# --- Fall 1: Replay 20 s, WAV-Dump ------------------------------------------
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$p = Start-Core "--no-audio --fast --duration 20 --file `"$file`" --service Dlf --wav `"$wav`" --events `"$events`""
$stderr = $p.StandardError.ReadToEndAsync()
$stdout = $p.StandardOutput.ReadToEndAsync()
if (-not $p.WaitForExit(120000)) { $p.Kill(); Write-Host "FEHLER: dabcored beendet sich nicht (Timeout)"; exit 1 }
$sw.Stop()
$p.StandardInput.Close()
$ev = Get-Content $events | Where-Object { $_.Trim() -ne '' } | ForEach-Object { $_ | ConvertFrom-Json }
Remove-Item $events -ErrorAction SilentlyContinue
$types = $ev | ForEach-Object { $_.type }
$counts = $types | Group-Object | Sort-Object Count -Descending | ForEach-Object { "$($_.Name)=$($_.Count)" }
Write-Host ("Fall 1: Laufzeit {0:N1} s (20 s Dateizeit, Faktor {1:N1}x), {2} Ereignisse: {3}" -f $sw.Elapsed.TotalSeconds, (20 / $sw.Elapsed.TotalSeconds), $ev.Count, ($counts -join ' '))

$started = $ev | Where-Object { $_.type -eq 'service_started' } | Select-Object -First 1
if (-not $started) { $fail += 'kein service_started' }
else {
    if ($started.sid -ne 53776) { $fail += "service_started fuer SId $($started.sid), erwartet 53776 (Dlf)" }
    if ($started.codec.codec -ne 'he_aac') { $fail += "codec $($started.codec.codec), erwartet he_aac" }
    if ($started.codec.sample_rate -ne 48000) { $fail += "sample_rate $($started.codec.sample_rate)" }
}
if (-not (Test-Path $wav)) { $fail += 'WAV fehlt' }
else {
    $len = (Get-Item $wav).Length
    $expected = 20 * 48000 * 2 * 2
    Write-Host ("WAV {0:N0} Byte = {1:N1} s (erwartet ~{2:N0}, -15 %)" -f $len, (($len - 44) / 192000.0), $expected)
    if ($len -lt $expected * 0.85 -or $len -gt $expected * 1.15) { $fail += "WAV-Groesse $len ausserhalb 85..115 % von $expected" }
    # Stille = Fehler: Mittelwert der Betraege ueber die Datei
    $bytes = [System.IO.File]::ReadAllBytes($wav)
    $sum = 0.0; $n = 0
    for ($i = 44; $i + 1 -lt $bytes.Length; $i += 64) {
        $v = [BitConverter]::ToInt16($bytes, $i); $sum += [Math]::Abs($v); $n++
    }
    $mean = if ($n) { $sum / $n } else { 0 }
    Write-Host ("WAV mittlerer Betrag {0:N0}" -f $mean)
    if ($mean -lt 100) { $fail += "WAV praktisch still (mittlerer Betrag $mean)" }
    Remove-Item $wav -ErrorAction SilentlyContinue
}
if (($types | Where-Object { $_ -eq 'dls' }).Count -lt 1) { $fail += 'kein dls' }
if (($types | Where-Object { $_ -eq 'service_stats' }).Count -lt 1) { $fail += 'kein service_stats' }
if ($types -notcontains 'recording_state') { $fail += 'kein recording_state' }
if ($types[-1] -ne 'exiting') { $fail += 'letztes Ereignis ist nicht exiting' }

# --- Fall 2: shutdown bei laufendem Dienst ----------------------------------
$events2 = Join-Path $tmpDir "dabcore-audio2-$tag.jsonl"
$p = Start-Core "--no-audio --file `"$file`" --service Dlf --events `"$events2`""
$stderr2 = $p.StandardError.ReadToEndAsync()
$stdout2 = $p.StandardOutput.ReadToEndAsync()
Start-Sleep -Seconds 3
$sw2 = [System.Diagnostics.Stopwatch]::StartNew()
$p.StandardInput.WriteLine('{"type":"shutdown"}')
$p.StandardInput.Flush()
$exited = $p.WaitForExit(3000)
$sw2.Stop()
if (-not $exited) { $p.Kill(); $fail += 'Fall 2: Prozess endet nicht binnen 3 s nach shutdown' }
else {
    $ev2 = Get-Content $events2 | Where-Object { $_.Trim() -ne '' } | ForEach-Object { $_ | ConvertFrom-Json }
    $t2 = $ev2 | ForEach-Object { $_.type }
    Write-Host ("Fall 2: shutdown -> Ende nach {0:N2} s, {1} Ereignisse, service_started={2}" -f $sw2.Elapsed.TotalSeconds, $ev2.Count, (($t2 | Where-Object { $_ -eq 'service_started' }).Count))
    if ($t2 -notcontains 'service_started') { $fail += 'Fall 2: Dienst lief nicht (kein service_started nach 3 s)' }
    if ($t2[-1] -ne 'exiting') { $fail += 'Fall 2: letztes Ereignis ist nicht exiting' }
}
Remove-Item $events2 -ErrorAction SilentlyContinue

if ($fail.Count -gt 0) {
    $fail | ForEach-Object { Write-Host "FEHLER: $_" }
    Write-Host "stderr: $($stderr.Result)"
    exit 1
}
Write-Host "OK"
exit 0
