# Timeshift-Test (Plan M4 1.7) am 60-s-Referenzmitschnitt in Echtzeit:
#   timeshift_configure 120 s -> Dienst Dlf -> 10 s hoeren (live, offset 0,
#   buffered ~10 s) -> pause -> 5 s -> play (offset ~5 s, mode playing) ->
#   skip -3 (offset ~8 s) -> live (offset 0) -> export 8..2 s (Format mp3
#   wird als WAV geschrieben, Warnung im Log) -> WAV ~6 s mit Ton ->
#   stop_service leert den Ring (buffered 0).
# Geprueft wird ausserdem die Speichermeldung beim Anlegen des Rings.
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

$tmpDir = [System.IO.Path]::GetTempPath()
$tag = [System.IO.Path]::GetRandomFileName()
$events = Join-Path $tmpDir "dabcore-ts-$tag.jsonl"
$wav = Join-Path $tmpDir "dabcore-ts-$tag.wav"
$fail = @()

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.Arguments = "--no-audio --no-epg --file `"$file`" --service Dlf --events `"$events`""
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.WorkingDirectory = Split-Path $exe
$p = [System.Diagnostics.Process]::Start($psi)
$stderr = $p.StandardError.ReadToEndAsync()
$stdout = $p.StandardOutput.ReadToEndAsync()

function Send-Cmd([string]$json) {
    $p.StandardInput.WriteLine($json)
    $p.StandardInput.Flush()
}

# Ereignisdatei lesen, waehrend der Kern weiterschreibt
function Get-Events {
    if (-not (Test-Path $events)) { return @() }
    $fs = [System.IO.File]::Open($events, 'Open', 'Read', 'ReadWrite')
    try {
        $sr = New-Object System.IO.StreamReader($fs)
        $text = $sr.ReadToEnd()
    } finally { $fs.Dispose() }
    $out = @()
    foreach ($line in $text -split "`n") {
        $l = $line.Trim()
        if ($l -eq '') { continue }
        try { $out += ($l | ConvertFrom-Json) } catch { }
    }
    return $out
}

function Get-Timeshift {
    $ev = Get-Events
    $ts = $ev | Where-Object { $_.type -eq 'timeshift_state' } | Select-Object -Last 1
    return $ts
}

function Wait-For([string]$type, [int]$timeoutS) {
    for ($i = 0; $i -lt $timeoutS * 5; $i++) {
        $ev = Get-Events
        $hit = $ev | Where-Object { $_.type -eq $type } | Select-Object -Last 1
        if ($hit) { return $hit }
        Start-Sleep -Milliseconds 200
    }
    return $null
}

function Check-Ts($ts, [string]$tag, [string]$mode, [double]$lo, [double]$hi) {
    if (-not $ts) { $script:fail += "$tag : kein timeshift_state"; return }
    Write-Host ("{0,-18} mode={1,-7} offset={2,6:N2} buffered={3,6:N2} capacity={4} frame={5} live_unix={6}" -f `
        $tag, $ts.mode, $ts.offset_s, $ts.buffered_s, $ts.capacity_s, $ts.frame_index, $ts.live_unix)
    if ($ts.mode -ne $mode) { $script:fail += "$tag : mode $($ts.mode), erwartet $mode" }
    if ($ts.offset_s -lt $lo -or $ts.offset_s -gt $hi) {
        $script:fail += ("{0} : offset_s {1:N2} ausserhalb {2}..{3}" -f $tag, $ts.offset_s, $lo, $hi)
    }
}

Send-Cmd '{"type":"timeshift_configure","capacity_s":120,"backing":{"backing":"ram"}}'
$started = Wait-For 'service_started' 25
if (-not $started) {
    $p.Kill()
    Write-Host "FEHLER: kein service_started"
    exit 1
}

# --- 10 s live hoeren ---------------------------------------------------------
Start-Sleep -Seconds 10
$ts = Get-Timeshift
Check-Ts $ts 'live (10 s)' 'live' -0.01 0.1
if ($ts -and ($ts.buffered_s -lt 8 -or $ts.buffered_s -gt 12)) {
    $fail += ("live (10 s): buffered_s {0:N2}, erwartet 8..12" -f $ts.buffered_s)
}
if ($ts -and $ts.capacity_s -ne 120) { $fail += "capacity_s $($ts.capacity_s), erwartet 120" }
if ($ts -and $ts.frame_index -lt 350) { $fail += "frame_index $($ts.frame_index) zu klein" }

# --- pause / play -------------------------------------------------------------
Send-Cmd '{"type":"timeshift_pause"}'
Start-Sleep -Milliseconds 600
Check-Ts (Get-Timeshift) 'pause' 'paused' 0.0 1.5
Start-Sleep -Seconds 5
Send-Cmd '{"type":"timeshift_play"}'
Start-Sleep -Milliseconds 800
Check-Ts (Get-Timeshift) 'play' 'playing' 4.5 6.8

# --- skip -3 ------------------------------------------------------------------
Send-Cmd '{"type":"timeshift_skip","delta_s":-3.0}'
Start-Sleep -Milliseconds 800
Check-Ts (Get-Timeshift) 'skip -3' 'playing' 7.5 9.8

# --- Versatz bleibt beim Abspielen stehen -------------------------------------
Start-Sleep -Seconds 2
Check-Ts (Get-Timeshift) 'playing +2 s' 'playing' 7.5 9.8

# --- live ---------------------------------------------------------------------
Send-Cmd '{"type":"timeshift_live"}'
Start-Sleep -Milliseconds 800
Check-Ts (Get-Timeshift) 'live' 'live' -0.01 0.1

# --- Export 8..2 s (Format mp3 -> Warnung + WAV) ------------------------------
$wavJson = $wav.Replace('\', '\\')
Send-Cmd "{`"type`":`"export_timeshift_range`",`"from_s`":8.0,`"to_s`":2.0,`"path`":`"$wavJson`",`"format`":{`"format`":`"mp3`",`"kbps`":192}}"
$rec = $null
for ($i = 0; $i -lt 60; $i++) {
    $ev = Get-Events
    $rec = $ev | Where-Object { $_.type -eq 'recording_state' -and $_.path -eq $wav } | Select-Object -Last 1
    if ($rec) { break }
    Start-Sleep -Milliseconds 200
}
if (-not $rec) { $fail += 'kein recording_state fuer den Export' }
else {
    Write-Host ("Export: recording_state active={0} bytes={1:N0} seconds={2:N2}" -f $rec.active, $rec.bytes, $rec.seconds)
    if ($rec.active) { $fail += 'recording_state des Exports meldet active=true' }
    if ($rec.slot -ne 'primary') { $fail += "Export-recording_state slot $($rec.slot)" }
    if ($rec.seconds -lt 5.7 -or $rec.seconds -gt 6.3) { $fail += ("Export seconds {0:N2}, erwartet ~6" -f $rec.seconds) }
}
$ev = Get-Events
if (-not ($ev | Where-Object { $_.type -eq 'log' -and $_.level -eq 'warn' -and $_.text -like '*Format mp3*' })) {
    $fail += 'keine Warnung zum Format mp3'
}
$mem = $ev | Where-Object { $_.type -eq 'log' -and $_.text -like 'Timeshift: Ring fuer den Primary-Dienst,*' } | Select-Object -Last 1
if (-not $mem) { $fail += 'keine Speichermeldung des Rings beim Anlegen' }
elseif ($mem.text -notlike '*120 s = 5000 Rahmen x 312 Byte*') { $fail += "Speichermeldung unerwartet: $($mem.text)" }
else { Write-Host $mem.text }

if (-not (Test-Path $wav)) { $fail += 'Export-WAV fehlt' }
else {
    $len = (Get-Item $wav).Length
    $secs = ($len - 44) / 192000.0
    $bytes = [System.IO.File]::ReadAllBytes($wav)
    $sum = 0.0; $n = 0
    for ($i = 44; $i + 1 -lt $bytes.Length; $i += 64) {
        $v = [double][BitConverter]::ToInt16($bytes, $i); $sum += $v * $v; $n++
    }
    $rms = if ($n) { [Math]::Sqrt($sum / $n) } else { 0 }
    Write-Host ("Export-WAV {0:N0} Byte = {1:N2} s, RMS {2:N0}" -f $len, $secs, $rms)
    if ($secs -lt 5.7 -or $secs -gt 6.3) { $fail += ("Export-WAV {0:N2} s, erwartet 6 s +-0,3" -f $secs) }
    if ($rms -lt 500) { $fail += ("Export-WAV zu leise (RMS {0:N0}, erwartet > 500)" -f $rms) }
    Remove-Item $wav -ErrorAction SilentlyContinue
}

# --- Dienstwechsel leert den Ring ---------------------------------------------
Send-Cmd '{"type":"stop_service","slot":"primary"}'
Start-Sleep -Milliseconds 1500
$ts = Get-Timeshift
if (-not $ts) { $fail += 'kein timeshift_state nach stop_service' }
else {
    Write-Host ("nach stop_service   mode={0} offset={1:N2} buffered={2:N2}" -f $ts.mode, $ts.offset_s, $ts.buffered_s)
    if ($ts.mode -ne 'live') { $fail += "nach stop_service mode $($ts.mode)" }
    if ($ts.buffered_s -ne 0) { $fail += "nach stop_service buffered_s $($ts.buffered_s), erwartet 0" }
}

Send-Cmd '{"type":"shutdown"}'
if (-not $p.WaitForExit(6000)) { $p.Kill(); $fail += 'Prozess endet nicht nach shutdown' }
$ev = Get-Events
$types = $ev | ForEach-Object { $_.type }
if ($types[-1] -ne 'exiting') { $fail += 'letztes Ereignis ist nicht exiting' }
$tsCount = ($types | Where-Object { $_ -eq 'timeshift_state' }).Count
Write-Host ("{0} Ereignisse, davon {1} timeshift_state" -f $ev.Count, $tsCount)
if ($tsCount -lt 20) { $fail += "nur $tsCount timeshift_state (erwartet >= 20, 2 Hz)" }
Remove-Item $events -ErrorAction SilentlyContinue

if ($fail.Count -gt 0) {
    $fail | ForEach-Object { Write-Host "FEHLER: $_" }
    Write-Host "stderr: $($stderr.Result)"
    exit 1
}
Write-Host "OK"
exit 0
