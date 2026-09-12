# Musik-Export-Test (Plan M4b 1.5) am 60-s-Referenzmitschnitt in Echtzeit:
#   timeshift_configure 120 s -> Dienst Dlf -> ~12 s puffern ->
#   export_timeshift_range 10..2 s mit format {mp3, kbps 192, id3{...}} ->
#   Datei beginnt mit "ID3", der Tag enthaelt TIT2/TPE1/TALB/TDRC und APIC,
#   direkt hinter dem Tag steht ein MPEG-1-Layer-III-Rahmen (0xFF 0xFB/0xFA),
#   recording_state meldet ~8 s und die Datei ist deutlich kleiner als WAV.
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
$events = Join-Path $tmpDir "dabcore-mus-$tag.jsonl"
$mp3 = Join-Path $tmpDir "dabcore-mus-$tag.mp3"
$fail = @()

# 1x1-PNG als Cover (Base64), damit auch der APIC-Rahmen geprueft wird
$coverB64 = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=='

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

function Wait-For([string]$type, [int]$timeoutS) {
    for ($i = 0; $i -lt $timeoutS * 5; $i++) {
        $hit = Get-Events | Where-Object { $_.type -eq $type } | Select-Object -Last 1
        if ($hit) { return $hit }
        Start-Sleep -Milliseconds 200
    }
    return $null
}

Send-Cmd '{"type":"timeshift_configure","capacity_s":120,"backing":{"backing":"ram"}}'
if (-not (Wait-For 'service_started' 25)) {
    $p.Kill()
    Write-Host "FEHLER: kein service_started"
    exit 1
}

# --- 12 s puffern -------------------------------------------------------------
Start-Sleep -Seconds 12
$ts = Get-Events | Where-Object { $_.type -eq 'timeshift_state' } | Select-Object -Last 1
if (-not $ts -or $ts.buffered_s -lt 10) {
    $p.Kill()
    Write-Host "FEHLER: Ring hat nur $($ts.buffered_s) s"
    exit 1
}

# --- Export 10..2 s als MP3 mit ID3 -------------------------------------------
$mp3Json = $mp3.Replace('\', '\\')
$cmd = '{"type":"export_timeshift_range","from_s":10.0,"to_s":2.0,"path":"' + $mp3Json +
       '","format":{"format":"mp3","kbps":192,"id3":{"title":"Test","artist":"X",' +
       '"album":"Dlf","date":"2026-09-12","cover_png_b64":"' + $coverB64 + '"}}}'
Send-Cmd $cmd

$rec = $null
for ($i = 0; $i -lt 100; $i++) {
    $rec = Get-Events | Where-Object { $_.type -eq 'recording_state' -and $_.path -eq $mp3 } | Select-Object -Last 1
    if ($rec) { break }
    Start-Sleep -Milliseconds 200
}
if (-not $rec) { $fail += 'kein recording_state fuer den MP3-Export' }
else {
    Write-Host ("Export: recording_state active={0} bytes={1:N0} seconds={2:N2}" -f $rec.active, $rec.bytes, $rec.seconds)
    if ($rec.active) { $fail += 'recording_state des Exports meldet active=true' }
    if ($rec.seconds -lt 7.7 -or $rec.seconds -gt 8.3) { $fail += ("Export seconds {0:N2}, erwartet ~8" -f $rec.seconds) }
}

Send-Cmd '{"type":"shutdown"}'
if (-not $p.WaitForExit(8000)) { $p.Kill(); $fail += 'Prozess endet nicht nach shutdown' }

# --- Datei pruefen ------------------------------------------------------------
if (-not (Test-Path $mp3)) { $fail += 'Export-MP3 fehlt' }
else {
    $b = [System.IO.File]::ReadAllBytes($mp3)
    Write-Host ("Export-MP3 {0:N0} Byte" -f $b.Length)
    if ($b.Length -lt 1000) { $fail += "Export-MP3 nur $($b.Length) Byte" }
    elseif ($b[0] -ne 0x49 -or $b[1] -ne 0x44 -or $b[2] -ne 0x33) {
        $fail += ("Export-MP3 beginnt nicht mit 'ID3' ({0:X2} {1:X2} {2:X2})" -f $b[0], $b[1], $b[2])
    }
    elseif ($b[3] -ne 0x04 -or $b[4] -ne 0x00) {
        $fail += ("ID3-Version {0}.{1}, erwartet 4.0" -f $b[3], $b[4])
    }
    else {
        # Tag-Groesse als syncsafe Integer (4 x 7 Bit), Header 10 Byte
        $size = ([int]$b[6] -shl 21) -bor ([int]$b[7] -shl 14) -bor ([int]$b[8] -shl 7) -bor [int]$b[9]
        $end = 10 + $size
        Write-Host ("ID3v2.4-Tag {0} Byte (Rahmen ab 10 bis {1})" -f $size, $end)
        if ($size -lt 40 -or $end -ge $b.Length) { $fail += "ID3-Tag-Groesse $size unplausibel" }
        else {
            $tagText = [System.Text.Encoding]::UTF8.GetString($b, 10, $size)
            foreach ($frame in @('TIT2', 'TPE1', 'TALB', 'TDRC', 'APIC')) {
                if ($tagText.IndexOf($frame) -lt 0) { $fail += "ID3-Rahmen $frame fehlt" }
            }
            foreach ($txt in @('Test', 'X', 'Dlf', '2026-09-12', 'image/png')) {
                if ($tagText.IndexOf($txt) -lt 0) { $fail += "ID3-Inhalt '$txt' fehlt" }
            }
            # direkt hinter dem Tag muss ein MPEG-Audio-Rahmen stehen
            $sync = $b[$end]
            $hdr = $b[$end + 1]
            Write-Host ("erstes Byte nach dem Tag: {0:X2} {1:X2}" -f $sync, $hdr)
            if ($sync -ne 0xFF -or ($hdr -ne 0xFB -and $hdr -ne 0xFA)) {
                $fail += ("kein MP3-Rahmen-Sync nach dem Tag ({0:X2} {1:X2}, erwartet FF FB/FA)" -f $sync, $hdr)
            }
            # 192 kbit/s * 8 s ~ 192 kByte, jedenfalls deutlich unter WAV (1,5 MB)
            $kbits = ($b.Length - $end) * 8.0 / 1000.0 / 8.0
            Write-Host ("MP3-Rahmen {0:N0} Byte = {1:N0} kbit/s" -f ($b.Length - $end), $kbits)
            if ($kbits -lt 150 -or $kbits -gt 240) { $fail += ("Bitrate {0:N0} kbit/s, erwartet ~192" -f $kbits) }
        }
    }
    Remove-Item $mp3 -ErrorAction SilentlyContinue
}
Remove-Item $events -ErrorAction SilentlyContinue

if ($fail.Count -gt 0) {
    $fail | ForEach-Object { Write-Host "FEHLER: $_" }
    Write-Host "stderr: $($stderr.Result)"
    exit 1
}
Write-Host "OK"
exit 0
