# IQ-Dump-Roundtrip ohne Hardware: die 60-s-Referenzdatei wird 5 s (Dateizeit,
# --fast) abgespielt und dabei per --iq-dump als .uff (int8) mitgeschrieben;
# der Dump muss von XmlFileSource lesbar sein und beim Replay Sync, Ensemble
# "DR Deutschland" und Dienste liefern. Fehlt die Referenzdatei, SKIP.
# Wird von ctest aufgerufen (DABCORED = Pfad zur EXE).
$ErrorActionPreference = 'Stop'
$exe = $env:DABCORED
if (-not $exe -or -not (Test-Path $exe)) { Write-Error "DABCORED nicht gesetzt oder nicht gefunden: $exe"; exit 2 }
$file = $env:DABCORE_REPLAY_FILE
if (-not $file) { $file = 'P:\Projekte\DAB\Warntag-2026\test\final-l32-g40-60s.uff' }
if (-not (Test-Path $file)) { Write-Host "SKIP: Referenzdatei fehlt: $file"; exit 0 }

function Run-Core([string]$arguments, [int]$timeoutMs) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = $arguments
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.WorkingDirectory = Split-Path $exe
    $p = [System.Diagnostics.Process]::Start($psi)
    $err = $p.StandardError.ReadToEndAsync()
    $out = $p.StandardOutput.ReadToEndAsync()
    if (-not $p.WaitForExit($timeoutMs)) { $p.Kill(); throw "dabcored beendet sich nicht (Timeout)" }
    $p.StandardInput.Close()
    return $err.Result
}

$tmpDir = [System.IO.Path]::GetTempPath()
$tag = [System.IO.Path]::GetRandomFileName()
$dump = Join-Path $tmpDir "dabcore-iqdump-$tag.uff"
$ev1 = Join-Path $tmpDir "dabcore-iqdump1-$tag.jsonl"
$ev2 = Join-Path $tmpDir "dabcore-iqdump2-$tag.jsonl"
$fail = @()

# 1. Replay 5 s mit Dump
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$stderr1 = Run-Core "--no-audio --fast --duration 5 --file `"$file`" --iq-dump `"$dump`" --events `"$ev1`"" 60000
$sw.Stop()
if (-not (Test-Path $dump)) { $fail += 'Dump-Datei fehlt' }
else {
    $len = (Get-Item $dump).Length
    $expected = 5000 + 5 * 2048000 * 2
    Write-Host ("Dump {0:N0} Byte (erwartet ~{1:N0}, 5 s int8 IQ + 5000 Byte Kopf), Laufzeit {2:N1} s" -f $len, $expected, $sw.Elapsed.TotalSeconds)
    if ($len -lt $expected * 0.9 -or $len -gt $expected * 1.1) { $fail += "Dump-Groesse $len ausserhalb 90..110 % von $expected" }
    # XML-Kopf pruefen
    $head = [System.IO.File]::ReadAllBytes($dump)[0..1500]
    $xml = [System.Text.Encoding]::ASCII.GetString($head) -replace "`0", ''
    if ($xml -notmatch 'Container="int8"') { $fail += 'Kopf ohne Container="int8"' }
    if ($xml -notmatch 'Datablock Count="(\d+)"') { $fail += 'Kopf ohne Datablock Count' }
    else {
        $count = [int64]$Matches[1]
        if ($count -ne ($len - 5000)) { $fail += "Count $count passt nicht zur Datenlaenge $($len - 5000)" }
    }
    if ($xml -notmatch 'Frequency Value="178352"') { $fail += 'Kopf ohne Frequency 178352 kHz' }
}
Remove-Item $ev1 -ErrorAction SilentlyContinue

# 2. Replay des Dumps
if ($fail.Count -eq 0) {
    $stderr2 = Run-Core "--no-audio --fast --file `"$dump`" --events `"$ev2`"" 60000
    $events = Get-Content $ev2 | Where-Object { $_.Trim() -ne '' } | ForEach-Object { $_ | ConvertFrom-Json }
    $types = $events | ForEach-Object { $_.type }
    $counts = $types | Group-Object | Sort-Object Count -Descending | ForEach-Object { "$($_.Name)=$($_.Count)" }
    Write-Host ("Replay des Dumps: {0} Ereignisse: {1}" -f $events.Count, ($counts -join ' '))
    $opened = $events | Where-Object { $_.type -eq 'device_opened' } | Select-Object -First 1
    if (-not $opened) { $fail += 'Dump: kein device_opened' }
    elseif ($opened.bit_depth -ne 8) { $fail += "Dump: bit_depth $($opened.bit_depth), erwartet 8" }
    if (-not ($events | Where-Object { $_.type -eq 'synced' -and $_.synced })) { $fail += 'Dump: kein synced:true' }
    $ens = $events | Where-Object { $_.type -eq 'ensemble_found' } | Select-Object -First 1
    if (-not $ens) { $fail += 'Dump: kein ensemble_found' }
    elseif ($ens.eid -ne 4284) { $fail += "Dump: ensemble_found eid $($ens.eid), erwartet 4284" }
    $services = @{}
    $events | Where-Object { $_.type -eq 'service_added' } | ForEach-Object { $services[[string]$_.service.sid] = 1 }
    if ($services.Count -lt 10) { $fail += "Dump: nur $($services.Count) Dienste (erwartet >= 10)" }
    if ($types[-1] -ne 'exiting') { $fail += 'Dump: letztes Ereignis ist nicht exiting' }
    Remove-Item $ev2 -ErrorAction SilentlyContinue
}
Remove-Item $dump -ErrorAction SilentlyContinue

if ($fail.Count -gt 0) {
    $fail | ForEach-Object { Write-Host "FEHLER: $_" }
    Write-Host "stderr: $stderr1 $stderr2"
    exit 1
}
Write-Host "OK"
exit 0
