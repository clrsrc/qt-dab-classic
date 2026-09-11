# DAB Classic v3.0 (Entwicklung)

Neuer Unterbau des DAB+-Empfängers: Qt-freier C++-Empfangskern als eigener
Prozess, Rust-Workspace mit Kanal-API (Kommandos rein, Ereignisse raus),
Oberfläche mit Tauri 2 + Svelte 5. Der bisherige Qt-DAB-Code in
`../qt-dab-master/` bleibt unverändert und dient als Referenz.

Entscheidungen: `docs/Entscheidungen.md`. Analyse: `docs/Analyse-2026-09-11.md`.
Protokoll: `docs/protocol.md`.

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

Oberfläche (Entwicklung mit Hot-Reload):

```powershell
cd apps\desktop
pnpm install
pnpm tauri dev
```

## Meilensteine

M0 Headless-Kern am Replay → M1 Audio/HackRF/RTL-SDR live → M2 Tauri-Shell
inkl. Presets → M3 Feature-Parität → M4 Timeshift, Musik-Trennung, Sleep-Timer.
