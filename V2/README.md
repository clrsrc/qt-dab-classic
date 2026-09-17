# DAB Classic v3.0 (Entwicklung)

Neuer Unterbau des DAB+-Empfängers: Qt-freier C++-Empfangskern als eigener
Prozess, Rust-Workspace mit Kanal-API (Kommandos rein, Ereignisse raus),
Oberfläche mit Tauri 2 + Svelte 5. Der bisherige Qt-DAB-Code in
`../qt-dab-master/` bleibt unverändert und dient als Referenz.

Entscheidungen: `docs/Entscheidungen.md`. Analyse: `docs/Analyse-2026-09-11.md`.
Protokoll: `docs/protocol.md`. Bedienung, Treiber, Portable-Ordner und Build-Schritte:
`ANLEITUNG.txt` (liegt auch im Portable-Paket).

## Aufbau

```
V2/
  Cargo.toml                 Rust-Workspace (MSVC-Toolchain)
  crates/
    dab-api/                 Command/Event-Typen, JSON-Zeilen-Protokoll
    dab-core/                startet dabcored.exe, liefert crossbeam-Kanäle
    dab-app/                 App-Logik ohne GUI (Datenordner, Einstellungen, Presets, ...)
    dab-music/               Musik-Trennung, MP3/ID3 (M4)
    dab-cli/                 Headless-Treiber: replay / live / scan / spike
  core-cpp/                  C++-Kern (MSYS2 UCRT64, CMake/Ninja)
    libdabcore/              Empfangspfad (Qt-frei), Ereignis-Fabrik, IPC
    dabcored/                Kernprozess (stdin/stdout)
    tests/                   Rauch- und Replay-Tests (ctest)
  apps/desktop/              Tauri 2 + Svelte 5
  tools/                     build-core.ps1, deploy-portable.ps1, test-replay.ps1
  docs/
```

## Bauen

Kern (einmal pro Änderung am C++-Code; kopiert `dabcored.exe` + DLLs in die
Tauri-Ressourcen):

```powershell
.\tools\build-core.ps1 -Test
```

Rust-Workspace und Headless-Treiber:

```powershell
cargo build --release
cargo test
.\target\release\dab-cli.exe spike --seconds 5
.\target\release\dab-cli.exe replay ..\Warntag-2026\cuts\warnung-110030-2min.uff --service Dlf --events out.jsonl
.\target\release\dab-cli.exe replay ..\Warntag-2026\test\final-l32-g40-60s.uff --fast --events out.jsonl   # ohne Echtzeit-Pacing
```

Kernprozess direkt (Headless, ohne Rust): `--fast` spielt ohne Pacing,
`--duration S` endet nach S Sekunden *Dateizeit*, am Dateiende folgt
`file_ended` und `exiting`:

```powershell
.\core-cpp\build\dabcored.exe --no-audio --file ..\Warntag-2026\test\final-l32-g40-60s.uff --fast --duration 20 --events out.jsonl
```

Stand M0: Datei → Sync → OFDM → FIC (Ensemble, Dienste, Uhrzeit, TII,
EWS FIG 0/15 und 0/19) und der MSC-Pfad (Backend je Dienst, DAB+ Superframe/
Firecode/Reed-Solomon, AAC per faad2 oder FDK-AAC zur Laufzeit, PAD mit DLS,
DL+ und MOT-Slides, MOT über Paketdienste, Audio-Thread mit 48-k-Konvertierung,
Lautstärke, Pegel, PortAudio, WAV-Aufnahme, Frame-Dump) sind Qt-frei in
`libdabcore` portiert. Mehrere Dienste gleichzeitig (Primary + n Background).
Offen: EPG-Compiler (SPI-Binär→XML), MP2, Timeshift.

    dabcored --no-audio --fast --file X.uff --service Dlf --wav out.wav --events out.jsonl
    dab-cli replay X.uff --fast --service Dlf --wav out.wav --duration 30
    python tools/dlplus-stats.py X.uff --duration 300   # Spike 3: DL+-Statistik
```

Stand M1 (Live-Empfang): HackRF One (`src/device/hackrf-source.*`, Port von
Qt-DAB `hackrf-handler`: 4,096 MS/s mit 2:1-Mittelung, Bandbreite 1536 kHz,
LNA/VGA/AMP, ppm über die Frequenz) und RTL-SDR (`rtlsdr-source.*`, Port von
`rtlsdr-handler`: 2,048 MS/s, Tuner-Gain-Tabelle, ppm; `librtlsdr.dll` aus dem
Osmocom-Windows-Release liegt in `core-cpp/third_party/bin/`), beide per
LoadLibrary zur Laufzeit. `set_channel` mit Dienste-Stopp und FIC-Reset,
SNR-AGC (Entscheidung 26, v1 `adjustGain`), `gain_changed`-Ereignis,
Band-III-Scan mit AMP-Retry (`src/scan/scan-controller.*`, Verweilzeit 6 s wie
v1 `switchDelay`), IQ-Dump als `.uff` (portierter `xml-filewriter`, auch aus
der Datei-Quelle), USB-Abriss → `device_error` + `device_closed`.

```powershell
.\target\release\dab-cli.exe live --channel 5C --service Dlf --duration 30 --events live.jsonl
.\target\release\dab-cli.exe live --channel 5C --gain 40,24,0 --no-agc --iq-dump 5c.uff --duration 10
.\target\release\dab-cli.exe scan --device hackrf            # Tabelle aller 38 Kanaele
.\core-cpp\build\dabcored.exe --device hackrf --channel 5C --service Dlf --duration 30 --events live.jsonl
.\core-cpp\build\dabcored.exe --no-audio --device hackrf --scan   # Tabelle auf stderr
```

Oberfläche (Entwicklung mit Hot-Reload):

```powershell
cd apps\desktop
pnpm install
pnpm tauri dev
```

## Stand M3

Feature-Parität zur Classic Compact Shell: EWS-Alarm mit eigenem Alarmfenster
(FIG 0/15, `ews_alert` inkl. rohem Status-Byte `stage_raw`) und Ortsabgleich,
Timer (manuell und aus dem EPG, Vor-/Nachlauf, Import der v1-Timer), WAV-Aufnahme mit
Umschaltsperre, Sleep-Timer, EPG-Panel mit Jetzt/Danach, Senderlogos,
TII-Sendestandorte mit Entfernung/Azimut, Debug-Panel (Spektrum, Konstellation,
SNR-Verlauf, Zähler), Datei-Wiedergabe, vollständiges Einstellungs-Panel
(Gerät/Gain je Gerät und Kanal, Audio, EWS, EPG, Aufnahmeordner, Panels,
Sprache DE/EN), portabler Datenordner `data/`. Audio-Ausgabegerät (17.09.2026):
Liste nur aus WASAPI (jedes Gerät einmal, Windows-Standard markiert), Auswahl per
stabiler Endpoint-ID statt PortAudio-Index, „Standard“ folgt dem Windows-Standardgerät
zur Laufzeit (Wächter im Kern), fehlende Geräte fallen auf den Standard zurück und
werden beim Anstecken wieder übernommen.

Hybrid Radio (17.09.2026, Standard aus): Mit dem Schalter „Internet-Ergänzung
(RadioDNS)“ in den Einstellungen holt `dab-app::radiodns` für die Audiodienste des
abgestimmten Ensembles nach, was dem Broadcast-EPG fehlt – Logos und die Sendepläne
von heute und morgen – per RadioDNS (ETSI TS 103 270: CNAME auf
`<scids>.<sid>.<eid>.<gcc>.dab.radiodns.org`, SRV `_radiospi`/`_radioepg`) und SPI
über IP (TS 102 818, `SI.xml` und `<jjjjmmtt>_PI.xml`). Der Kern liefert dafür den ECC
aus FIG 0/9 (`ensemble_found.ecc`, `ensemble_ecc`). Broadcast hat Vorrang; die Zwischen-
ergebnisse liegen in `data/radiodns/`.

TPEG-Verkehrsmeldungen (17.09.2026, Standard an, nur Broadcast): Der Kern erkennt den
TPEG-Paketdienst des Ensembles (FIG 0/13 Appl-Type 4, DSCTy 5 TDC, ETSI TS 103 551 – in
NRW „ARD TPEG“ auf WDR 11D/9A, nicht im Bundesmux) und startet ihn wie den EPG-Dienst als
Background-Slot (`set_tpeg`, `--no-tpeg`); die geprüften MSC-Datengruppen gehen als
`tdc_group` an die App. `dab-app::tpeg` dekodiert TPEG2: Transport-/Dienstrahmen (zlib),
SNI, TEC 3.2 (MMC, Ereignis mit Ursache/Unterursache, Spuren, Länge, Verzögerung, Zeiten,
Hinweise) und die Ortsreferenz (OpenLR-Koordinaten, Straßenklasse/-art, Richtung, Länge;
TMC-Code zusätzlich). Das Verkehr-Panel (TA) zeigt die Liste, mit Heimatkoordinaten nach
Entfernung sortiert. Autobahnnummern und Anschlussstellen kommen aus einer eingebauten
Tabelle (`crates/dab-app/data/autobahnen.bin`, erzeugt mit `tools/build-autobahnen.py`
aus OpenStreetMap – © OpenStreetMap-Mitwirkende, ODbL): naechstes Autobahn-Teilstueck zum
ersten Punkt, Anschlussstellen am Anfang und Ende. Die Binärregeln (ISO 21219-3) wurden am
Mitschnitt nachvollzogen und gegen die Apache-2.0-Referenz `fenghlkevin/tpeg-item`
abgeglichen; TFP (Verkehrsfluss) wird nur erkannt.

Ortsabgleich (Geofencing) beim EWS-Alarm: Die Heimatkoordinaten gehen als
`set_home_location` in den Kern, der die Ortscodes eines Alarms (TS 104 089
Annex F) damit vergleicht und sein Urteil als `ews_alert.relevant` zurückmeldet.
Nur passende Alarme schalten um und öffnen das Alarmfenster; ortsfremde (z. B.
der Eiffelturm-Funktionstest des Bundesmux) werden still in der EWF-Historie
vermerkt. Ohne Heimatkoordinaten bleibt es beim ungefilterten Verhalten.

Portable-Paket: `.	ools\deploy-portable.ps1 -Zip` baut Kern und Shell im
Release und packt `dist\DAB-Classic-portable\` (EXE, `core\`, `tii\`, `data\`,
WebView2-Fixed-Version-Runtime aus `third_party\webview2\*.cab`) sowie
`dist\DAB-Classic-v3.0-dev-portable-win64.zip`.

## Meilensteine

M0 Headless-Kern am Replay → M1 Audio/HackRF/RTL-SDR live → M2 Tauri-Shell
inkl. Presets → M3 Feature-Parität → M4 Timeshift, Musik-Trennung, Sleep-Timer.
