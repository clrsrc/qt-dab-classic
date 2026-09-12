# M4 Timeshift – Umsetzungsplan (Stand 12.09.2026)

Grundlage: Entscheidungen 4, 5, 7, 14, 15, 18, 21, 24 in `Entscheidungen.md`; Protokoll in `protocol.md`
(Kommandos `timeshift_configure/pause/play/seek/skip/live`, `export_timeshift_range`, Ereignis
`timeshift_state` **LW**). Die Protokollnamen sind FEST, nur Felder dürfen additiv ergänzt werden.

## 1. Kern (libdabcore, C++)

### 1.1 Anzapfpunkt
`Backend::run()` → `deconvolver` → `hardBits` (ein logischer 24-ms-Rahmen, `bitRate*3` Byte) →
`driver.addtoFrame(hardBits)` → `mp4Processor::addtoFrame` (Firecode/RS/AAC). Zwischen Backend und
Driver kommt für den **Primary-Slot** ein `TimeshiftBuffer`:

- **live**: Rahmen an den Driver durchreichen UND an den Ring anhängen.
- **paused**: nur an den Ring anhängen (Lesezeiger steht).
- **playing**: Ring füllt weiter; ein Takt-Thread (24 ms je Rahmen, mit Drift-Korrektur gegen
  `steady_clock`) liest ab Lesezeiger und ruft `driver.addtoFrame`. Erreicht der Lesezeiger den
  Schreibzeiger → automatisch `live`.

### 1.2 Ring
`data/`-frei, nur RAM (Entscheidung 4; `backing: disk` wird angenommen, aber vorerst wie ram behandelt und
geloggt). Kapazität = `capacity_s * 1000 / 24` Rahmen, Rahmengröße = `bitRate*3` Byte (Dlf 104 kbit/s,
60 min ≈ 46,8 MB). Überlauf: ältester Rahmen fällt weg; steht der Lesezeiger dort, rückt er mit
(`offset_s` = Kapazität). Ring wird geleert bei: Dienstwechsel des Primary-Slots, Kanalwechsel,
`close_device`, `timeshift_configure` mit anderer Kapazität, EWS-Umschaltung.

Rahmen tragen einen monotonen Index und die Ensemble-Uhrzeit (`clock_time` unix_utc, wenn bekannt), damit
Export und Musik-Trennung später Absolutzeiten haben.

### 1.3 Kommandos
| Kommando | Verhalten |
|---|---|
| `timeshift_configure{capacity_s, backing}` | Kapazität setzen (Standard 3600, Bereich 60..14400); bei Änderung Ring neu anlegen (leeren) |
| `timeshift_pause` | live/playing → paused; Zeitpunkt merken |
| `timeshift_play` | paused → playing ab Lesezeiger |
| `timeshift_seek{offset_s}` | Lesezeiger auf „offset_s Sekunden hinter live“ (0 = live), Bereich 0..buffered_s; Zustand bleibt (paused bleibt paused) |
| `timeshift_skip{delta_s}` | relativ: +30 = weiter Richtung live, −30 = zurück; über live hinaus → live |
| `timeshift_live` | Lesezeiger = Schreibzeiger, Zustand live; **Audio-Puffer des Sinks verwerfen** (sonst hört man 0,7 s Altes) |
| `export_timeshift_range{from_s, to_s, path, format}` | `from_s/to_s` = Sekunden hinter live (from_s > to_s); Rahmen kopieren und in einem eigenen Thread durch eine ZWEITE `mp4Processor`-Instanz mit NullSink + WAV-Writer dekodieren (schneller als Echtzeit); `format: wav` Pflicht, `mp3`/`aac_passthrough` → `log warn` + WAV (M4b); Ende: `recording_state{slot: primary, active: false, path, bytes, seconds}` mit dem Exportpfad |

Bei `select_service`/`stop_service` auf dem Primary-Slot: Ring leeren, Zustand live.
Beim Pausieren/Spulen den PortAudio-Sink NICHT stoppen (Latenz); Stille wird durch fehlende Rahmen erzeugt
(Sink läuft leer → Underrun-Zähler nicht hochzählen: Flag `starved` im Sink unterdrücken, solange paused).

### 1.4 Ereignis `timeshift_state` (LW, 2 Hz und bei jedem Zustandswechsel sofort)
`mode: live|paused|playing`, `buffered_s` (Inhalt des Rings), `offset_s` (Abstand Lesezeiger→live),
`capacity_s`; additiv: `frame_index` (Schreibzeiger), `live_unix` (Ensemble-Uhrzeit am Schreibzeiger, 0 wenn
unbekannt). `state_snapshot.state.timeshift` = dasselbe Objekt (heute `null`).

### 1.5 EWS (Entscheidung 5)
Bei `ews_alert{phase: trigger}` mit Autoswitch: `timeshift_live` + Umschalten auf den Warndienst (macht der
Kern schon) → Ring leeren; `ews_switched` wie heute. Nach `end`: vorheriger Dienst live, neuer Ring.
Kein Timeshift auf dem Warndienst.

### 1.6 Timer/Aufnahme-Vorlauf (Entscheidung 18)
`start_recording` bekommt additiv `pre_s?`: wenn > 0 und der Ring so viel hat, zuerst
`export_timeshift_range(pre_s..0)` in dieselbe Datei schreiben und dann live weiter anhängen (WAV-Writer
muss anhängen können). Falls das im WAV-Writer aufwendig ist: Vorlauf als eigene Datei
`<name>_vorlauf.wav` und im `recording_state` den Pfad mitgeben – dann in der App dokumentieren.

### 1.7 Tests (ctest)
`dabcored_timeshift` mit der 60-s-Referenzdatei (`core-cpp/tests`, wie `test-audio.ps1`): configure 120 s →
Dlf → nach 10 s pause → 5 s → play → `timeshift_state` zeigt offset ≈ 5 s, mode playing; skip −3 → offset ≈ 8;
live → offset 0; export 8..2 → WAV mit ≈ 6 s (±0,3) und RMS > 500; Kanal-/Dienstwechsel leert (buffered 0).
Speicherbedarf loggen (`log info` bei configure: Rahmen × Bytes).

## 2. App (dab-app, Rust)

`timeshift.rs`: Zustand aus `timeshift_state` (Truth), Kommandos als Effects: `pause_toggle()`,
`skip(±30)`, `seek(offset)`, `live()`, `configure(capacity)` (aus `settings.timeshift_capacity_s`, beim
Start und bei Settings-Änderung). Regeln:
- Bei laufender Aufnahme ist Timeshift-Bedienung erlaubt (Aufnahme läuft live weiter, Kern trennt das).
- Dienst-/Kanalwechsel: Zustand auf live (Kern leert), UI-Leiste zurücksetzen.
- Hinweis-Ereignis `AppEvent::TimeshiftNotice` bei EWS-Verlassen („Alarm: Zeitversatz verworfen“).
- Tauri `timeshift_cmds.rs`: `timeshift_pause_toggle`, `timeshift_skip{delta_s}`, `timeshift_seek{offset_s}`,
  `timeshift_live`, `timeshift_export{from_s,to_s}` (Dateiname wie Aufnahme mit Suffix `_timeshift`).
- Unit-Tests: Effekte je Kommando, Reset bei Wechsel, Kapazität aus Settings.

## 3. Shell (Svelte)

`TimeshiftBar.svelte` unter dem Transport (immer sichtbar, wenn ein Audiodienst läuft und Quelle ≠ Datei):
Pufferbalken (buffered/capacity), Marke für den Lesezeiger, Anzeige „−1:23“ (offset) bzw. „LIVE“,
Knöpfe ⏸/▶, −30 s, +30 s, LIVE; Klick/Drag auf den Balken = seek. Hotkeys (Entscheidung 21): Leertaste
Pause/Play, ← / → ±30 s, Esc live (in `hotkeys.ts` sind sie als Platzhalter vorhanden). Display zeigt bei
offset > 0 ein „TIMESHIFT −m:ss“-Feld statt des Live-Indikators. Einstellungen: Kapazität in Minuten
(1..240). i18n `ts.*` de/en. Anleitung ergänzen.

## 4. Abnahme (live, HackRF)
Dlf: 1 min hören → Leertaste → 20 s → Leertaste → Ton setzt an der Pausestelle fort (DLS-Text der Stelle),
Leiste zeigt −0:20; ← zweimal → −1:20 (begrenzt durch Puffer); Esc → live, Ton springt; Senderwechsel →
Leiste leer; Export 60..0 → WAV ≈ 60 s. Warntag-Ausschnitt `warnung-110030-2min.uff` mit Pause vor dem
Alarm → Alarm verlässt Timeshift, Hinweis im Hauptfenster.

## 5. Danach: M4b Musik-Trennung (Kurzfassung, Details bei Start)
`dab-music` (Datenmodell existiert): DL+ `ITEM.TITLE/ARTIST` + `item_toggle/item_running` → Kandidaten
mit Start/Ende als Rahmenindex im Ring (Vorlauf 2 s, Nachlauf 2 s), DLS-„A - B“-Fallback für Dienste ohne
DL+ (Spike 3); Export je Kandidat über `export_timeshift_range` → WAV → LAME (MP3 192 kbit/s, ID3v2.4
mit Titel/Artist/Sender/Datum/Cover = Slide oder Logo) im Kern (`format: mp3` implementieren, libmp3lame
liegt in MSYS2 vor); Option AAC-Passthrough (.m4a, 960-Sample-Frames) als Archiv. Panel „Musik“ mit
Vorschlagsliste, Wellenform (aus `audio_level`-Verlauf oder Export-PCM), verschiebbare Marken,
„übernehmen“/„alle übernehmen“, Schalter „automatisch speichern“ (Entscheidung 7).
