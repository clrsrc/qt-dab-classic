# Replay-Test: dabcored dekodiert die 60-s-Referenzdatei (Kanal 5C, Bundesmux)
# ohne Echtzeit-Pacing und muss Sync, Ensemble, Dienste und EWS-Heartbeats
# melden. Wird von ctest aufgerufen (DABCORED = Pfad zur EXE).
# Fehlt die Referenzdatei, wird der Test uebersprungen (Exit 0 mit Hinweis).
$ErrorActionPreference = 'Stop'
$exe = $env:DABCORED
if (-not $exe -or -not (Test-Path $exe)) { Write-Error "DABCORED nicht gesetzt oder nicht gefunden: $exe"; exit 2 }

$file = $env:DABCORE_REPLAY_FILE
if (-not $file) { $file = 'P:\Projekte\DAB\Warntag-2026\test\final-l32-g40-60s.uff' }
if (-not (Test-Path $file)) {
    Write-Host "SKIP: Referenzdatei fehlt: $file"
    exit 0
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("dabcore-replay-" + [System.IO.Path]::GetRandomFileName() + ".jsonl")
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.Arguments = "--no-audio --file `"$file`" --fast --duration 20 --events `"$tmp`""
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.WorkingDirectory = Split-Path $exe
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$p = [System.Diagnostics.Process]::Start($psi)
# stdin offen lassen: der Prozess endet am (--duration-)Dateiende von selbst
$stderr = $p.StandardError.ReadToEndAsync()
if (-not $p.WaitForExit(120000)) { $p.Kill(); Write-Error "dabcored beendet sich nicht (Timeout)"; exit 1 }
$sw.Stop()
$p.StandardInput.Close()

$events = Get-Content $tmp | Where-Object { $_.Trim() -ne '' } | ForEach-Object { $_ | ConvertFrom-Json }
Remove-Item $tmp -ErrorAction SilentlyContinue
$types = $events | ForEach-Object { $_.type }
$counts = $types | Group-Object | Sort-Object Count -Descending | ForEach-Object { "$($_.Name)=$($_.Count)" }
Write-Host ("Laufzeit {0:N1} s, {1} Ereignisse: {2}" -f $sw.Elapsed.TotalSeconds, $events.Count, ($counts -join ' '))

$fail = @()
if ($types[0] -ne 'ready') { $fail += 'erstes Ereignis ist nicht ready' }
if (-not ($events | Where-Object { $_.type -eq 'synced' -and $_.synced })) { $fail += 'kein synced:true' }
$ens = $events | Where-Object { $_.type -eq 'ensemble_found' } | Select-Object -First 1
if (-not $ens) { $fail += 'kein ensemble_found' }
elseif ($ens.eid -ne 4284 -or $ens.name -ne 'DR Deutschland') { $fail += "ensemble_found falsch: $($ens.eid) '$($ens.name)'" }
# Der Bundesmux "DR Deutschland" traegt 16 Dienste (14 Audio, EPG, GEPOS);
# service_added kommt je Dienst mindestens einmal, mit Programmtyp erneut.
$added = @($events | Where-Object { $_.type -eq 'service_added' })
$services = @{}
$added | ForEach-Object { $services[[string]$_.service.sid] = $_.service }
if ($services.Count -lt 14) { $fail += "nur $($services.Count) Dienste (erwartet >= 14)" }
if ($added.Count -lt 16) { $fail += "nur $($added.Count) service_added (erwartet >= 16)" }
if (-not $services['53776'] -or $services['53776'].name -ne 'Dlf') { $fail += 'Dienst Dlf (SId 53776) fehlt' }
if (-not $services['4292'] -or $services['4292'].name -ne 'ASA DE') { $fail += 'Dienst ASA DE (SId 4292) fehlt' }
$alive = @($events | Where-Object { $_.type -eq 'ews_alive' }).Count
if ($alive -lt 5) { $fail += "nur $alive ews_alive (erwartet >= 5)" }
if ($types -notcontains 'file_ended') { $fail += 'kein file_ended' }
if ($types[-1] -ne 'exiting') { $fail += 'letztes Ereignis ist nicht exiting' }

if ($fail.Count -gt 0) {
    $fail | ForEach-Object { Write-Host "FEHLER: $_" }
    Write-Host "stderr: $($stderr.Result)"
    exit 1
}
Write-Host ("OK: {0} Dienste, {1} ews_alive, Ensemble {2} ({3:X4})" -f $services.Count, $alive, $ens.name, $ens.eid)
exit 0
