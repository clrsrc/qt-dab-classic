# Snapshot-/Scope-/Beende-Test am 2-min-Ausschnitt des Warntag-Mitschnitts
# (Echtzeit, --service Dlf, kein Audio-Sink):
#   1. audio_devices kommt nach ready (auch mit --no-audio) und nach get_state
#   2. state_snapshot nach 12 s: running[] enthaelt den Primary Dlf mit codec
#      he_aac und dls (der Ausschnitt traegt DLS), ensemble als [eid, name],
#      scopes/tii_enabled/snr/clock_time vorhanden
#   3. set_scopes{spectrum,iq,rate_hz:5}: in 4 s je 14..24 spectrum (2048
#      Bins = 2732 Base64-Zeichen) und iq_samples (3072 int8 = 4096 Zeichen);
#      danach set_scopes aus -> keine weiteren Scope-Ereignisse
#   4. stdin-EOF bei laufendem Dienst -> Prozessende binnen 1 s, exiting zuletzt
# Wird von ctest aufgerufen (DABCORED = Pfad zur EXE); fehlt der Mitschnitt,
# wird der Test uebersprungen (Exit 0 mit Hinweis).
$ErrorActionPreference = 'Stop'
$exe = $env:DABCORED
if (-not $exe -or -not (Test-Path $exe)) { Write-Error "DABCORED nicht gesetzt oder nicht gefunden: $exe"; exit 2 }

$file = $env:DABCORE_SNAPSHOT_FILE
if (-not $file) { $file = 'P:\Projekte\DAB\Warntag-2026\cuts\warnung-110030-2min.uff' }
if (-not (Test-Path $file)) {
    Write-Host "SKIP: Mitschnitt fehlt: $file"
    exit 0
}

$tmpDir = [System.IO.Path]::GetTempPath()
$tag = [System.IO.Path]::GetRandomFileName()
$events = Join-Path $tmpDir "dabcore-snapshot-$tag.jsonl"
$fail = @()

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.Arguments = "--no-audio --file `"$file`" --service Dlf --events `"$events`""
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.WorkingDirectory = Split-Path $exe
$p = [System.Diagnostics.Process]::Start($psi)
$stderr = $p.StandardError.ReadToEndAsync()
$stdout = $p.StandardOutput.ReadToEndAsync()

function Send([string]$line) {
    if ($p.HasExited) { throw "dabcored hat sich vorzeitig beendet (ExitCode $($p.ExitCode)); stderr: $($stderr.Result)" }
    $p.StandardInput.WriteLine($line); $p.StandardInput.Flush()
}
function Count-Lines { if (Test-Path $events) { @(Get-Content $events | Where-Object { $_.Trim() -ne '' }).Count } else { 0 } }

try {
    Start-Sleep -Seconds 12
    Send '{"type":"get_state"}'
    Start-Sleep -Seconds 1
    $n0 = Count-Lines
    Send '{"type":"set_scopes","spectrum":true,"iq":true,"rate_hz":5}'
    Start-Sleep -Seconds 4
    Send '{"type":"set_scopes","spectrum":false,"iq":false,"rate_hz":5}'
    Start-Sleep -Milliseconds 500
    $n1 = Count-Lines
    Start-Sleep -Seconds 2
    $n2 = Count-Lines
} catch {
    Write-Host "FEHLER: $_"
    if (-not $p.HasExited) { $p.Kill() }
    if (Test-Path $events) { Write-Host "letzte Ereignisse:"; Get-Content $events -Tail 5 | ForEach-Object { $_.Substring(0, [Math]::Min(200, $_.Length)) } }
    exit 1
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$p.StandardInput.Close()
$exited = $p.WaitForExit(5000)
$sw.Stop()
if (-not $exited) { $p.Kill(); $fail += 'Prozess endet nicht binnen 5 s nach stdin-EOF' }
Write-Host ("EOF -> Prozessende nach {0:N0} ms" -f $sw.Elapsed.TotalMilliseconds)
if ($sw.Elapsed.TotalMilliseconds -gt 1000) { $fail += ("Beenden dauerte {0:N0} ms (> 1000)" -f $sw.Elapsed.TotalMilliseconds) }

$lines = Get-Content $events | Where-Object { $_.Trim() -ne '' }
$ev = $lines | ForEach-Object { $_ | ConvertFrom-Json }
Remove-Item $events -ErrorAction SilentlyContinue
$types = $ev | ForEach-Object { $_.type }
$counts = $types | Group-Object | Sort-Object Count -Descending | ForEach-Object { "$($_.Name)=$($_.Count)" }
Write-Host ("{0} Ereignisse: {1}" -f $ev.Count, ($counts -join ' '))

# 1. audio_devices
if ($types[0] -ne 'ready') { $fail += 'erstes Ereignis ist nicht ready' }
$adIdx = @(0..($types.Count - 1) | Where-Object { $types[$_] -eq 'audio_devices' })
if ($adIdx.Count -lt 2) { $fail += "audio_devices $($adIdx.Count)x, erwartet >= 2 (nach ready und nach get_state)" }
else {
    if ($adIdx[0] -gt 3) { $fail += "erstes audio_devices erst an Position $($adIdx[0])" }
    $snapIdx = @(0..($types.Count - 1) | Where-Object { $types[$_] -eq 'state_snapshot' })
    if ($snapIdx.Count -lt 1) { $fail += 'kein state_snapshot' }
    elseif ($types[$snapIdx[0] + 1] -ne 'audio_devices') { $fail += 'audio_devices folgt nicht direkt auf state_snapshot' }
}
$ad = $ev | Where-Object { $_.type -eq 'audio_devices' } | Select-Object -First 1
if ($ad -and $ad.names.Count -ne 0) { $fail += '--no-audio: audio_devices.names nicht leer' }

# 2. state_snapshot
$snap = $ev | Where-Object { $_.type -eq 'state_snapshot' } | Select-Object -First 1
if ($snap) {
    $s = $snap.state
    if (-not ($s.ensemble -is [array]) -or $s.ensemble[0] -ne 4284) { $fail += "state.ensemble ist nicht [4284, name]: $($s.ensemble | ConvertTo-Json -Compress)" }
    if (-not $s.synced) { $fail += 'state.synced false' }
    if ($null -eq $s.scopes -or $s.scopes.rate_hz -ne 5 -or $s.scopes.spectrum) { $fail += "state.scopes unerwartet: $($s.scopes | ConvertTo-Json -Compress)" }
    if (-not $s.tii_enabled) { $fail += 'state.tii_enabled false' }
    if ($s.snr -le 0) { $fail += "state.snr $($s.snr)" }
    if ($null -eq $s.clock_time) { $fail += 'state.clock_time fehlt (FIG 0/10 kam in 12 s nicht?)' }
    $run = @($s.running)
    $dlf = $run | Where-Object { $_.slot -eq 'primary' } | Select-Object -First 1
    if (-not $dlf) { $fail += 'state.running ohne Primary' }
    else {
        if ($dlf.sid -ne 53776) { $fail += "running.primary sid $($dlf.sid)" }
        if ($dlf.codec.codec -ne 'he_aac') { $fail += "running.primary codec $($dlf.codec | ConvertTo-Json -Compress)" }
        if (-not $dlf.dls) { $fail += 'running.primary ohne dls' }
        if ($null -eq $dlf.recording -or $dlf.recording.active) { $fail += 'running.primary.recording unerwartet' }
        Write-Host ("Snapshot Dlf: codec={0} stereo={1} dls='{2}' dl_plus={3} slide={4}" -f $dlf.codec.codec, $dlf.stereo, $dlf.dls, ($null -ne $dlf.dl_plus), ($null -ne $dlf.slide))
    }
    $epg = $run | Where-Object { $_.slot -eq 'background' -and $_.codec.codec -eq 'data' }
    if (-not $epg) { $fail += 'state.running ohne EPG-Hintergrunddienst (codec data)' }
    if ($s.primary[0] -ne 53776) { $fail += "state.primary $($s.primary | ConvertTo-Json -Compress)" }
}

# 3. Scopes: Ereignisse zwischen n0 und n1, keine mehr zwischen n1 und n2
$during = $lines[$n0..($n1 - 1)] | ForEach-Object { $_ | ConvertFrom-Json }
$after = if ($n2 -gt $n1) { $lines[$n1..($n2 - 1)] | ForEach-Object { $_ | ConvertFrom-Json } } else { @() }
$spec = @($during | Where-Object { $_.type -eq 'spectrum' })
$iq = @($during | Where-Object { $_.type -eq 'iq_samples' })
Write-Host ("Scopes in 4 s bei 5 Hz: spectrum={0} (Laenge {1}), iq_samples={2} (Laenge {3})" -f $spec.Count,
    ($(if ($spec.Count) { $spec[0].bins_b64.Length } else { 0 })), $iq.Count, ($(if ($iq.Count) { $iq[0].iq_b64.Length } else { 0 })))
if ($spec.Count -lt 14 -or $spec.Count -gt 24) { $fail += "spectrum $($spec.Count)x in 4 s, erwartet 14..24" }
if ($iq.Count -lt 14 -or $iq.Count -gt 24) { $fail += "iq_samples $($iq.Count)x in 4 s, erwartet 14..24" }
if ($spec.Count -and $spec[0].bins_b64.Length -ne 2732) { $fail += "spectrum bins_b64 Laenge $($spec[0].bins_b64.Length), erwartet 2732 (2048 Bins)" }
if ($iq.Count -and $iq[0].iq_b64.Length -ne 4096) { $fail += "iq_samples iq_b64 Laenge $($iq[0].iq_b64.Length), erwartet 4096 (1536 x I/Q int8)" }
if ($iq.Count) {
    # Konstellation: Betraege nahe 127 (Einheitskreis), nicht alles 0
    $b = [Convert]::FromBase64String($iq[0].iq_b64)
    $onCircle = 0
    for ($i = 0; $i + 1 -lt $b.Length; $i += 2) {
        $re = [int]$b[$i]; if ($re -gt 127) { $re -= 256 }
        $im = [int]$b[$i + 1]; if ($im -gt 127) { $im -= 256 }
        $mag = [Math]::Sqrt($re * $re + $im * $im)
        if ($mag -gt 110 -and $mag -lt 135) { $onCircle++ }
    }
    if ($onCircle -lt 1400) { $fail += "iq_samples: nur $onCircle von 1536 Punkten auf dem Einheitskreis" }
}
$late = @($after | Where-Object { $_.type -eq 'spectrum' -or $_.type -eq 'iq_samples' })
if ($late.Count -gt 0) { $fail += "$($late.Count) Scope-Ereignisse nach set_scopes aus" }
# Vor set_scopes darf es keine Scope-Ereignisse geben
$early = @(($lines[0..($n0 - 1)] | ForEach-Object { $_ | ConvertFrom-Json }) | Where-Object { $_.type -eq 'spectrum' -or $_.type -eq 'iq_samples' })
if ($early.Count -gt 0) { $fail += "$($early.Count) Scope-Ereignisse vor set_scopes" }

# 4. Ende
if ($types[-1] -ne 'exiting') { $fail += "letztes Ereignis ist nicht exiting ($($types[-1]))" }

if ($fail.Count -gt 0) {
    $fail | ForEach-Object { Write-Host "FEHLER: $_" }
    Write-Host "stderr: $($stderr.Result)"
    exit 1
}
Write-Host "OK"
exit 0
