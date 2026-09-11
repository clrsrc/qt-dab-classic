# DAB Classic v3.0 — Entscheidungen (Stand 2026-09-11)

Grundlage: `Analyse-2026-09-11.md` (Abschnitt 9, Fragen 1–27) und die Antworten von Stefan
Kammann in der Sitzung vom 11.09.2026. Diese Datei ist die verbindliche Kurzfassung.

## Rahmen

- Bestehender Qt-DAB-Code (`qt-dab-master/`) bleibt unverändert. Alles Neue liegt unter `V2/`.
- Nach außen **v3.0**, Produktname **„DAB Classic"**. Gleiches Repository, neuer Branch `v3-dev`
  (abgezweigt von `v2.0-dev`). `.gitignore` wird um V2-Build-Artefakte ergänzt.
- Neue Funktionen: 10 Stationsspeicher, Timeshift, Musikaufnahme mit Titel-Trennung,
  alle bisherigen Funktionen (abzüglich Streichliste), **zusätzlich einfache RTL-SDR-Unterstützung**.

## Architektur

| Nr | Entscheidung |
|---|---|
| 1 | **Kein Rust-Rewrite des DSP.** C++-Kern wird Qt-frei als Bibliothek `libdabcore` herausgelöst. |
| 2 | Kern läuft als **eigener Prozess `dabcored.exe`** (MinGW), Protokoll über stdin/stdout. Rust-Crate `dab-core` startet ihn und liefert crossbeam-Kanäle (`Sender<Command>` / `Receiver<Event>`). `trait CoreBackend` hält In-Process- oder Rust-Kern für später offen. |
| 3 | **Audio-Ausgabe per PortAudio im Kernprozess** (wie heute). GUI sendet nur Lautstärke/Gerätewahl. PCM-Streaming-Schalter für späteres cpal optional. |
| 11 | **Toolchain:** Rust-Workspace + Tauri mit MSVC-Rust (rustup stable, VS Build Tools 2022). Kern mit MSYS2 UCRT64 GCC/CMake/Ninja. PowerShell-Skript baut Kern und kopiert EXE+DLLs in die Tauri-Ressourcen. Kein CMake-Aufruf aus `build.rs`. MSYS2-GNU-Rust wird nicht benutzt. |
| 14 | **App-Logik in Rust** (`dab-app`: Timer, Presets, EPG-Cache/Parser, Logo-Cache, Timeshift-Steuerung, Aufnahme-Regeln, Musik-Schnittliste). Svelte ist reine Darstellung. |
| 17 | **Headless-Testtreiber `dab-cli replay` + Regressionstests am Warntag-Mitschnitt von Anfang an.** Event-Snapshot (Sync, Ensemble, Dienste, DLS, EWS-Phasen mit Zeitstempeln), Audio-Dump-Vergleich mit v1. 10-s-Ausschnitt ins Repo/Download. Pflichttest vor jedem Kern-Commit. |
| 20 | **faad2 (GPL) fest als Standard-AAC-Decoder.** FDK-AAC nur zur Laufzeit nachladbar, wenn die DLL im `core/`-Ordner liegt. Öffentliches Release ohne FDK-DLL. |
| 23 | **Web-Remote als Design-Ziel:** Svelte-Komponenten ohne direkte Tauri-Aufrufe, Transport-Abstraktionsschicht, alle Daten über Events. Nicht jetzt bauen. |
| 24 | **Mehrere Dienste parallel in der Kern-API vorsehen** (`SelectService` mit Slot Primary/Background). Bedienung erst nach den vier Hauptfunktionen. Umschaltsperre dann nur noch bei Kanalwechsel. |
| + | **RTL-SDR:** Qt-DAB `devices/rtlsdr-handler` Qt-frei als zweite Sample-Quelle portieren (Runtime-Load `librtlsdr.dll` wie libhackrf, nativ 2,048 MS/s, Gain-Tabelle, PPM-Korrektur). `OpenDevice{kind: HackRf | RtlSdr | File}`. Kein rtl_tcp. |

## Neue Funktionen

| Nr | Entscheidung |
|---|---|
| 4 | **Timeshift pro Dienst im RAM** (Subkanal-Hardbits, ~40 MB/h). Standard 60 min, Länge einstellbar. Puffer beginnt bei Senderwechsel neu. Gleicher Puffer für rückwirkende Aufnahme und Musik-Trennung. |
| 5 | **EWS-Alarm verlässt immer den Puffer**, springt auf live, schaltet auf den Warndienst. Nach Alarmende vorheriger Sender live weiter (Puffer verworfen, Hinweis in der UI). |
| 6 | **Musikaufnahme als MP3** (LAME, ID3v2.4 mit Cover aus MOT-Slide/Logo) als Standard. **AAC-Passthrough** (.m4a, 960-Sample-Frames) als einschaltbare Option „Original-AAC mitsichern". |
| 7 | **Musik-Trennung liefert Vorschlagsliste** mit Wellenform, verschiebbaren Schnittmarken, „übernehmen" / „alle übernehmen". Zusätzlich Schalter „automatisch speichern". Titelerkennung über DL+ (IT/IR-Bits), Schnitt aus dem Timeshift-Puffer mit Vor-/Nachlauf. |
| 8 | **Presets:** 10 Slot-Buttons + Tasten 1–9/0. Belegen per Rechtsklick „mit aktuellem Sender belegen", Langdruck, Drag aus der Senderliste, Strg+Ziffer. Überschreiben mit Nachfrage. Favoriten-XML aus Qt-DAB einmalig importieren. Slot = Kanal, EId, SId, Name, Logo-Cache. Umschaltsperre bei Aufnahme. |
| 15 | **Reihenfolge:** Presets → Timeshift → Musik-Trennung. |
| 16 | **Verschränkt mit klarer Grenze:** Presets schon in der minimalen Shell (M2). EWS, Timer, Aufnahme, EPG müssen fertig sein, bevor Timeshift beginnt. |
| 18 | **Sleep-Timer** (stumm oder beenden, freie Minuteneingabe) und **Aufnahme-Vor-/Nachlauf** (Standard 2 min vor, 5 min nach; Vorlauf ggf. rückwirkend aus dem Timeshift-Puffer). |

## Oberfläche

| Nr | Entscheidung |
|---|---|
| 9 | **Layout und Charakter der Classic Compact Shell übernehmen** (frameless, dunkel, kompakt). EPG, Timer, Presets, Timeshift-Leiste als ein-/ausklappbare Panels im Hauptfenster. |
| 10 | **Einziges Zusatzfenster: Alarm-Dialog** (always on top). |
| 12 | **Portable mit WebView2 Fixed-Version-Runtime** (~180 MB) im Ordner — läuft garantiert vom Stick. Keine Start.bat mehr: `data/`-Ordner neben der EXE wird automatisch erkannt, sonst Benutzerprofil. |
| 13 | **Datei-Wiedergabe (.uff, rohe int8-.iq) Pflicht** in Kern (`OpenFile{path, loop}`, `FileProgress`) und GUI (Öffnen-Dialog, Fortschritt, Schleife). |
| 21 | **Hotkeys:** 1–9/0 Presets · Strg+Ziffer belegen · Leertaste Timeshift Pause/Play · Pfeil ←/→ ±30 s · Pfeil ↑/↓ Sender prev/next · Esc live · M Mute · R Aufnahme · E EPG-Panel · T Timer-Panel · +/− Lautstärke. |
| 25 | **Ein ausklappbares Debug-Panel:** Spektrum, Konstellation, SNR-Verlauf, TII-Liste mit Sendestandort, Fehlerzähler FIC/RS/AAC. Kern liefert Daten nur bei offenem Panel (5–10 Hz). Wasserfall, Korrelation, Null-Symbol entfallen. |
| 26 | **Gain:** SNR-AGC als Standard an, Scan-AMP-Retry bleibt. Manuelle Werte je Kanal gespeichert, wenn AGC aus. Gain-Sätze je Gerät (HackRF, RTL-SDR) getrennt. |
| 27 | **Sprache:** Deutsch und Englisch umschaltbar (JSON-Sprachdateien, Systemsprache, manuell umschaltbar). Deutsch Standard. |

## Streichliste (19)

Entfallen in v3.0: Journaline, TDC/IP-Datagramme, TCP-Server, Uploader, HTTP-Karte (später evtl.
Leaflet-Panel), 11 fremde SDR-Backends (nur HackRF, RTL-SDR, Datei), MP2-Audio, Legacy-Scheduler
und alte EPG-Zeittabelle, klassischer Qt-DAB-GUI-Modus, Qt-Audio.
Bleiben als Debug-Optionen im Kern: ETI-Ausgabe, Frame-Dump.

## Meilensteine

| M | Ziel | Abnahme |
|---|---|---|
| M0 | Workspace + Headless-Kern am Replay (`dabcored --file … --events … --wav …`), `dab-api`, `dab-core`, `dab-cli` | Event-Log mit Sync, „DR Deutschland (10BC)", Diensten, DLS, EWS-Phasen wie v1; WAV bit-identisch zum v1-Audio-Dump |
| M1 | Audio hörbar, HackRF + RTL-SDR live, AGC, Scan, Aufnahme WAV | `dab-cli live --channel 5C --service Dlf` spielt |
| M2 | Tauri-Shell minimal inkl. Presets, Portable-Datenordner | alltagstauglich hören |
| M3 | Feature-Parität (EWS, Timer, Aufnahme, EPG, Logos, TII, Konfig, Datei-Wiedergabe, Debug-Panel, Deployment) | Checkliste Analyse 5.4 |
| M4 | Timeshift → Musik-Trennung → Sleep-Timer/Nachlauf → parallele Dienste (UI) | |

Vor M0: Spikes (je 1–2 Tage) — IPC-Stub, Kern ohne Qt am 60-s-Referenzfile, DL+-Verfügbarkeit
am Warntag-Mitschnitt; AAC-960-Abspielbarkeit nur für die Passthrough-Option.
