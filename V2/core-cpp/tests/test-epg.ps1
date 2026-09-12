# SPI/EPG-Test: dabcored spielt den 8-min-Ausschnitt des Warntag-Mitschnitts
# (--fast, ohne --service) ab. Der Kern muss den SPI-Paketdienst des
# Bundesmux ("EPG Deutschland", FIG 0/13 Appl-Type 7) selbst als Background-
# Slot starten (service_started codec data) und daraus liefern:
#   - mot_object mit PNG-Logos (content_type 0x0203, Daten beginnen mit
#     89 50 4E 47), SId aus dem Dateinamen ("d210_Dlf_32x32.png" -> 0xD210;
#     der Bundesmux-SPI traegt auch Logos des zweiten Bundesmux, z. B. 1057)
#   - epg_object mit XML des epg-compilers (<epg system="DAB">), sid/date
#     aus dem MOT-Namen ("w20260914dd230c0.EHB" -> 20260914 / 0xD230)
# Zweiter Fall: --no-epg -> kein Paketdienst, keine MOT-/EPG-Ereignisse.
# Wird von ctest aufgerufen (DABCORED = Pfad zur EXE); fehlt der Mitschnitt,
# wird der Test uebersprungen (Exit 0 mit Hinweis).
$ErrorActionPreference = 'Stop'
$exe = $env:DABCORED
if (-not $exe -or -not (Test-Path $exe)) { Write-Error "DABCORED nicht gesetzt oder nicht gefunden: $exe"; exit 2 }

$file = $env:DABCORE_EPG_FILE
if (-not $file) { $file = 'P:\Projekte\DAB\Warntag-2026\cuts\warnung-105830-8min.uff' }
if (-not (Test-Path $file)) {
    Write-Host "SKIP: Mitschnitt fehlt: $file"
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
$events = Join-Path $tmpDir "dabcore-epg-$tag.jsonl"
$fail = @()

# --- Fall 1: automatischer SPI/EPG-Dienst -----------------------------------
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$p = Start-Core "--no-audio --fast --duration 480 --file `"$file`" --events `"$events`""
$stderr = $p.StandardError.ReadToEndAsync()
$stdout = $p.StandardOutput.ReadToEndAsync()
if (-not $p.WaitForExit(300000)) { $p.Kill(); Write-Host "FEHLER: dabcored beendet sich nicht (Timeout)"; exit 1 }
$sw.Stop()
$p.StandardInput.Close()
$ev = Get-Content $events | Where-Object { $_.Trim() -ne '' } | ForEach-Object { $_ | ConvertFrom-Json }
Remove-Item $events -ErrorAction SilentlyContinue
$types = $ev | ForEach-Object { $_.type }
$counts = $types | Group-Object | Sort-Object Count -Descending | ForEach-Object { "$($_.Name)=$($_.Count)" }
Write-Host ("Fall 1: Laufzeit {0:N1} s, {1} Ereignisse: {2}" -f $sw.Elapsed.TotalSeconds, $ev.Count, ($counts -join ' '))

$started = @($ev | Where-Object { $_.type -eq 'service_started' -and $_.slot -eq 'background' -and $_.codec.codec -eq 'data' })
if ($started.Count -lt 1) { $fail += 'kein service_started (background, codec data) fuer den SPI-Dienst' }
elseif ($started[0].sid -ne 3771797692) { $fail += "SPI-Dienst SId $($started[0].sid), erwartet 3771797692 (EPG Deutschland)" }
if ($types -contains 'service_started' -and ($ev | Where-Object { $_.type -eq 'service_started' -and $_.slot -eq 'primary' })) {
    $fail += 'ein Primary-Dienst wurde ohne --service gestartet'
}

$mots = @($ev | Where-Object { $_.type -eq 'mot_object' })
$pngs = @($mots | Where-Object { $_.content_type -eq 0x0203 })
$bySid = @{}
foreach ($m in $pngs) {
    $bytes = [Convert]::FromBase64String($m.data_b64)
    if ($bytes.Length -lt 8 -or $bytes[0] -ne 0x89 -or $bytes[1] -ne 0x50 -or $bytes[2] -ne 0x4E -or $bytes[3] -ne 0x47) {
        $fail += "PNG-Header ungueltig: $($m.name)"
    }
    if ($m.eid -ne 4284) { $fail += "mot_object eid $($m.eid), erwartet 4284" }
    $bySid[('{0:X4}' -f $m.sid)] += 1
}
Write-Host ("mot_object: {0} gesamt, {1} PNG; je SId: {2}" -f $mots.Count, $pngs.Count,
    (($bySid.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ' '))
if ($pngs.Count -lt 1) { $fail += 'kein mot_object mit PNG' }
if (-not $bySid.ContainsKey('D210') -and -not $bySid.ContainsKey('D220') -and -not $bySid.ContainsKey('D230')) {
    $fail += 'kein Logo mit SId eines Deutschlandradio-Dienstes (D210/D220/D230)'
}
if ($bySid.ContainsKey('0000') -and $bySid['0000'] -gt $pngs.Count / 5) { $fail += 'mehr als ein Fuenftel der Logos ohne SId-Zuordnung' }
$names = $pngs | ForEach-Object { $_.name }
if (($names | Sort-Object -Unique).Count -ne $names.Count) { $fail += 'doppelte Logo-Namen (MOT-Karussell mehrfach gemeldet)' }

$epgs = @($ev | Where-Object { $_.type -eq 'epg_object' })
$epgKeys = @{}
foreach ($e in $epgs) {
    if ($e.sid -eq 0) { continue }   # Service-Information (list.xml)
    if (-not $e.xml.StartsWith('<epg system="DAB" tz="local">')) { $fail += "epg_object ohne <epg system=""DAB"" tz=""local""> ($($e.name))" }
    if ($e.date_yyyymmdd -lt 20260901 -or $e.date_yyyymmdd -gt 20261231) { $fail += "epg_object Datum $($e.date_yyyymmdd) unplausibel ($($e.name))" }
    if ($e.eid -ne 4284) { $fail += "epg_object eid $($e.eid), erwartet 4284" }
    $epgKeys[("{0}_{1:X4}" -f $e.date_yyyymmdd, $e.sid)] += 1
}
Write-Host ("epg_object: {0}; je Datum/SId: {1}" -f $epgs.Count,
    (($epgKeys.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ' '))
if ($epgs.Count -lt 1) { $fail += 'kein epg_object' }
if ($types[-1] -ne 'exiting') { $fail += 'letztes Ereignis ist nicht exiting' }

# --- Fall 2: --no-epg -> kein automatischer Paketdienst ----------------------
$events2 = Join-Path $tmpDir "dabcore-epg2-$tag.jsonl"
$p = Start-Core "--no-audio --fast --duration 30 --file `"$file`" --no-epg --events `"$events2`""
$stderr2 = $p.StandardError.ReadToEndAsync()
$stdout2 = $p.StandardOutput.ReadToEndAsync()
if (-not $p.WaitForExit(120000)) { $p.Kill(); $fail += 'Fall 2: dabcored beendet sich nicht (Timeout)' }
else {
    $p.StandardInput.Close()
    $ev2 = Get-Content $events2 | Where-Object { $_.Trim() -ne '' } | ForEach-Object { $_ | ConvertFrom-Json }
    $t2 = $ev2 | ForEach-Object { $_.type }
    Write-Host ("Fall 2 (--no-epg): {0} Ereignisse, service_started={1}, mot_object={2}" -f $ev2.Count,
        (@($t2 | Where-Object { $_ -eq 'service_started' })).Count, (@($t2 | Where-Object { $_ -eq 'mot_object' })).Count)
    if ($t2 -contains 'service_started') { $fail += 'Fall 2: Dienst trotz --no-epg gestartet' }
    if ($t2 -contains 'mot_object' -or $t2 -contains 'epg_object') { $fail += 'Fall 2: MOT-/EPG-Ereignisse trotz --no-epg' }
}
Remove-Item $events2 -ErrorAction SilentlyContinue

if ($fail.Count -gt 0) {
    $fail | ForEach-Object { Write-Host "FEHLER: $_" }
    Write-Host "stderr: $($stderr.Result)"
    exit 1
}
Write-Host "OK"
exit 0
