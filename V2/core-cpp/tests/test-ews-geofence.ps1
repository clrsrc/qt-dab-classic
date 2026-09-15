# EWS-Geofencing (ASA DE, ETSI TS 104 089 Annex F): dabcored spielt den
# 2-min-Ausschnitt des Warntag-Mitschnitts (Bundesmux 5C, echter Alarm mit
# pre_trigger/trigger/sustain/end) mit --service Dlf ab; die Heimatkoordinaten
# kommen wie von der App per set_home_location ueber stdin.
#   Fall 1  Heimat Duesseldorf (51.218, 6.7617): die Ortscodes des Alarms
#           decken sie ab -> ews_alert relevant=true, Umschaltung auf den
#           Warndienst "ASA DE" (SId 4292) per ews_switched, bei end zurueck.
#   Fall 2  Heimat Lissabon (38.7223, -9.1393): kein Ortscode deckt sie ab ->
#           relevant=false und kein ews_switched (so wird der "Eiffelturm"-
#           Funktionstest von einem deutschen Standort aus ignoriert).
#   Fall 3  ohne set_home_location -> relevant=null (Ausgangsverhalten),
#           es wird wie bisher umgeschaltet.
# Wird von ctest aufgerufen (DABCORED = Pfad zur EXE); fehlt der Mitschnitt,
# wird der Test uebersprungen (Exit 0 mit Hinweis).
$ErrorActionPreference = 'Stop'
$exe = $env:DABCORED
if (-not $exe -or -not (Test-Path $exe)) { Write-Error "DABCORED nicht gesetzt oder nicht gefunden: $exe"; exit 2 }

$file = $env:DABCORE_EWS_FILE
if (-not $file) { $file = 'P:\Projekte\DAB\Warntag-2026\cuts\warnung-110030-2min.uff' }
if (-not (Test-Path $file)) {
    Write-Host "SKIP: Mitschnitt fehlt: $file"
    exit 0
}

$tmpDir = [System.IO.Path]::GetTempPath()
$tag = [System.IO.Path]::GetRandomFileName()
$fail = @()

# Einen Durchlauf fahren; $home = $null oder @(lat, lon) fuer set_home_location.
function Invoke-Run([string]$name, $homeCoords) {
    $events = Join-Path $tmpDir "dabcore-ews-$tag-$name.jsonl"
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = "--no-audio --no-epg --fast --duration 70 --file `"$file`" --service Dlf --events `"$events`""
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.WorkingDirectory = Split-Path $exe
    $p = [System.Diagnostics.Process]::Start($psi)
    $stdout = $p.StandardOutput.ReadToEndAsync()
    $stderr = $p.StandardError.ReadToEndAsync()
    # Die App schickt die Heimatkoordinaten direkt nach dem Start; der Alarm
    # liegt erst bei Sekunde 29 der Datei, also lange danach.
    if ($null -ne $homeCoords) {
        $p.StandardInput.WriteLine('{"type":"set_home_location","lat":' + $homeCoords[0] + ',"lon":' + $homeCoords[1] + '}')
        $p.StandardInput.Flush()
    }
    if (-not $p.WaitForExit(300000)) { $p.Kill(); throw "dabcored beendet sich nicht (Timeout, Fall $name)" }
    $p.StandardInput.Close()
    $ev = Get-Content $events | Where-Object { $_.Trim() -ne '' } | ForEach-Object { $_ | ConvertFrom-Json }
    Remove-Item $events -ErrorAction SilentlyContinue
    return $ev
}

# --- Fall 1: Heimat Duesseldorf -> Alarm ist relevant ------------------------
$ev = Invoke-Run 'home-de' @(51.218, 6.7617)
$alerts = @($ev | Where-Object { $_.type -eq 'ews_alert' })
$trigger = @($alerts | Where-Object { $_.phase -eq 'trigger' })
$switched = @($ev | Where-Object { $_.type -eq 'ews_switched' })
Write-Host ("Fall 1 (Duesseldorf): {0} ews_alert ({1}), {2} ews_switched" -f $alerts.Count,
    (($alerts | ForEach-Object { "$($_.phase)/relevant=$($_.relevant)" }) -join ' '), $switched.Count)
if ($trigger.Count -lt 1) { $fail += 'Fall 1: kein ews_alert mit phase=trigger' }
elseif ($trigger[0].relevant -ne $true) { $fail += "Fall 1: trigger relevant=$($trigger[0].relevant), erwartet true" }
# pre_trigger ist rein informativ (ETSI Klausel 7.5.1: Alert matching greift erst
# bei Trigger; core.cpp ruft handleEwsAutoswitch auch nur bei phase != 0 auf) und
# hat - anders als trigger/sustain - KEINEN eigenen C/N-Sammel-Mechanismus in
# fib-decoder.cpp: sein "relevant" kann auf einem unvollstaendigen Ortscode-
# Ausschnitt der ersten FIG-0/15-Instanz beruhen. Nur die Phasen pruefen, die
# tatsaechlich die Umschaltung ausloesen.
if (@($alerts | Where-Object { $_.phase -ne 'pre_trigger' -and $_.locations.Count -gt 0 -and $_.relevant -ne $true }).Count -gt 0) {
    $fail += 'Fall 1: ein Trigger/Sustain/End-Alarm mit Ortscodes wurde als nicht relevant gemeldet'
}
if ($switched.Count -lt 1) { $fail += 'Fall 1: kein ews_switched trotz relevantem Alarm' }
elseif ($switched[0].to_sid -ne 4292) { $fail += "Fall 1: ews_switched to_sid $($switched[0].to_sid), erwartet 4292 (ASA DE)" }

# --- Fall 2: Heimat Lissabon -> Alarm betrifft den Standort nicht ------------
$ev2 = Invoke-Run 'home-pt' @(38.7223, -9.1393)
$alerts2 = @($ev2 | Where-Object { $_.type -eq 'ews_alert' })
$trigger2 = @($alerts2 | Where-Object { $_.phase -eq 'trigger' })
$switched2 = @($ev2 | Where-Object { $_.type -eq 'ews_switched' })
Write-Host ("Fall 2 (Lissabon): {0} ews_alert ({1}), {2} ews_switched" -f $alerts2.Count,
    (($alerts2 | ForEach-Object { "$($_.phase)/relevant=$($_.relevant)" }) -join ' '), $switched2.Count)
if ($trigger2.Count -lt 1) { $fail += 'Fall 2: kein ews_alert mit phase=trigger' }
elseif ($trigger2[0].relevant -ne $false) { $fail += "Fall 2: trigger relevant=$($trigger2[0].relevant), erwartet false" }
if ($switched2.Count -gt 0) { $fail += "Fall 2: $($switched2.Count) ews_switched trotz nicht zutreffendem Standort" }
if (@($ev2 | Where-Object { $_.type -eq 'log' -and $_.text -like '*Geofencing*' }).Count -lt 1) {
    $fail += 'Fall 2: kein Hinweis im Log, warum nicht umgeschaltet wurde'
}

# --- Fall 3: ohne Heimatkoordinaten -> relevant null, Verhalten wie bisher ---
$ev3 = Invoke-Run 'no-home' $null
$alerts3 = @($ev3 | Where-Object { $_.type -eq 'ews_alert' })
$trigger3 = @($alerts3 | Where-Object { $_.phase -eq 'trigger' })
$switched3 = @($ev3 | Where-Object { $_.type -eq 'ews_switched' })
Write-Host ("Fall 3 (ohne Heimat): {0} ews_alert, relevant={1}, {2} ews_switched" -f $alerts3.Count,
    (($alerts3 | ForEach-Object { "$($_.relevant)" }) -join ','), $switched3.Count)
if ($trigger3.Count -lt 1) { $fail += 'Fall 3: kein ews_alert mit phase=trigger' }
elseif ($null -ne $trigger3[0].relevant) { $fail += "Fall 3: relevant=$($trigger3[0].relevant), erwartet null" }
if ($switched3.Count -lt 1) { $fail += 'Fall 3: kein ews_switched ohne gesetzte Heimatkoordinaten' }

if ($fail.Count -gt 0) {
    $fail | ForEach-Object { Write-Host "FEHLER: $_" }
    exit 1
}
Write-Host 'OK'
exit 0
