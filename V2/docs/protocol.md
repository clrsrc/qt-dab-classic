# Protokoll Kern ↔ App (Version 1)

Der Kernprozess `dabcored.exe` liest **Kommandos** als JSON-Zeilen von stdin und
schreibt **Ereignisse** als JSON-Zeilen auf stdout. Eine Zeile = ein Objekt,
abgeschlossen mit `\n`, UTF-8. Das Feld `type` (snake_case) bestimmt die
Variante; die übrigen Felder stehen flach daneben. Binärdaten sind Base64.

Maßgebliche Definition: `crates/dab-api/src/lib.rs` (Rust, serde) und
`core-cpp/libdabcore/include/dabcore/events.h` (C++). Beide müssen
feldgenau übereinstimmen; `cargo test -p dab-api` prüft die Rust-Seite,
`core-cpp/tests` die C++-Seite.

## Lebenszyklus

1. Kern startet, sendet sofort `ready` (`core_version`, `protocol_version`, `decoders`).
2. App sendet `open_device`, dann `set_channel`, `select_service`, ...
3. Kern endet bei `shutdown` oder bei EOF auf stdin und sendet zuletzt `exiting`.

## Kommandos (App → Kern)

| type | Felder |
|---|---|
| `open_device` | `source: {kind: hack_rf, serial?} \| {kind: rtl_sdr, index} \| {kind: file, path, loop, fast?}` (`fast`: Datei ohne Echtzeit-Pacing; Standard = `--fast` von dabcored) |
| `close_device` | |
| `set_channel` | `channel` ("5C"). Bei Geräten: laufende Dienste stoppen, Quelle neu abstimmen (v1: 204 800 Samples verwerfen), FIC zurücksetzen; danach kommen `synced`, `ensemble_found`, `service_added` neu oder `no_signal`. Vor `open_device` oder bei Datei-Quellen wird der Kanal nur gemerkt |
| `set_gain` | `gain: {lna, vga, amp}` → `gain_changed` |
| `set_agc` | `enabled` (Standard an, Entscheidung 26) → `gain_changed` |
| `set_ppm` | `ppm` (HackRF: über die Frequenz korrigiert, RTL-SDR: `rtlsdr_set_freq_correction`) |
| `select_service` | `sid`, `scids`, `slot: primary\|background`. Primary: Audio-Ausgabe + Aufnahme (höchstens einer; ein Wechsel bei laufender Aufnahme wird abgelehnt). Background: nur Backend + Aufnahme, **mehrere gleichzeitig** möglich (Entscheidung 24) |
| `stop_service` | `slot`, `sid?` (ohne `sid`: alle Dienste des Slots) |
| `start_scan` | `channels: []` (leer = alle 38 Band-III-Kanäle), `mode: single\|to_data\|continuous`. Nur mit Gerät; laufende Dienste werden gestoppt, `select_service` ist während des Scans abgelehnt. Je Kanal `scan_progress`, dann nach der Verweilzeit (v1 `switchDelay` 6 s, continuous 12 s) oder nach „kein Signal“ ein `scan_result`; beim ersten `no_signal` wird einmal der AMP umgeschaltet und derselbe Kanal weiter beobachtet (AMP-Retry, `gain_changed`). `single`: alle Kanäle einmal, dann `scan_finished`, der Kern bleibt auf dem letzten Kanal; `to_data`: bis zum ersten Ensemble mit Diensten, dort bleiben; `continuous`: bis `stop_scan` |
| `stop_scan` | → `scan_finished` |
| `set_volume` | `percent` |
| `set_mute` | `muted` |
| `set_audio_device` | `index?` |
| `start_recording` | `path`, `format: {format: wav} \| {format: mp3, kbps} \| {format: aac_passthrough}`, `slot`, `sid?` (heute nur `wav`: 48 kHz, Stereo, 16 Bit, vor der Lautstärke abgegriffen) |
| `stop_recording` | `slot`, `sid?` |
| `export_timeshift_range` | `from_s`, `to_s`, `path`, `format` |
| `start_iq_dump` / `stop_iq_dump` | `path` – Samples der Quelle als `.uff` (Qt-DAB-XML-Format, 8 Bit: HackRF `int8`, RTL-SDR `uint8`, Datei-Quelle `int8` der resampelten 2,048 MS/s); von `open_device{file}` wieder lesbar |
| `start_frame_dump` / `stop_frame_dump` | `path` |
| `timeshift_configure` | `capacity_s`, `backing: {backing: ram} \| {backing: disk, dir}` |
| `timeshift_pause` / `timeshift_play` / `timeshift_live` | |
| `timeshift_seek` | `offset_s` |
| `timeshift_skip` | `delta_s` |
| `set_ews` | `enabled`, `autoswitch` |
| `ews_dismiss` | |
| `set_scopes` | `spectrum`, `iq`, `rate_hz` |
| `set_tii` | `enabled`, `threshold`, `dx_mode` |
| `get_state` | → `state_snapshot` |
| `shutdown` | |

## Ereignisse (Kern → App)

Lückenlos, außer den mit **LW** (latest-wins) markierten, die der Kern drosselt
und die App bei Überlast verwerfen darf.

| type | Felder |
|---|---|
| `ready` | `core_version`, `protocol_version`, `decoders[]` |
| `device_opened` | `name`, `serial`, `bit_depth` |
| `device_closed` | |
| `device_error` | `message` (auch bei USB-Abriss im Betrieb; der Kern schließt die Quelle danach, `device_closed` folgt) |
| `gain_changed` | `lna`, `vga`, `amp`, `agc` – nach `set_gain`/`set_agc`, beim Öffnen eines Geräts, bei jeder AGC-Nachführung (HackRF: VGA ±2 bei SNR < 8 / > 18, RTL-SDR: eine Tuner-Gain-Stufe) und beim AMP-Retry im Scan; die App speichert den Satz je Gerät und Kanal (Entscheidung 26). HackRF: LNA 0–40 (8er-Schritte), VGA 0–62 (2er), AMP an/aus; RTL-SDR: `lna` = Tuner-Gain in 0,1 dB (nächster Tabellenwert), `vga`/`amp` ohne Bedeutung |
| `file_progress` **LW** | `position_s`, `length_s` |
| `file_ended` | |
| `synced` | `synced` |
| `no_signal` | `channel` |
| `snr` **LW** | `db` |
| `fic_quality` **LW** | `ok`, `total` |
| `frequency_offset` **LW** | `hz` |
| `ensemble_found` | `eid`, `name`, `channel` |
| `service_added` | `service: {sid, scids, name, is_audio, is_primary, sub_ch, bitrate_kbps, pty}` – kann für dasselbe `sid`/`scids` erneut kommen (z. B. sobald der Programmtyp aus FIG 0/17 bekannt ist); die App ersetzt den Eintrag |
| `ensemble_reconfigured` | |
| `clock_time` | `unix_utc`, `lto_minutes` |
| `service_started` | `slot`, `sid`, `scids`, `codec: {codec: he_aac, sbr, ps, sample_rate} \| {codec: mp2, sample_rate} \| {codec: data}`, `stereo` – bei Audio erst mit dem ersten dekodierten Block (Codec-Daten stammen aus dem Superframe/Decoder), bei Paketdiensten sofort |
| `service_stopped` | `slot`, `sid` |
| `service_stats` **LW** | `slot`, `sid`, `frame_errors`, `rs_errors`, `aac_errors`, `rs_corrections` – Zähler der letzten Sekunde Sendezeit (42 DAB-Rahmen): Superframes ohne Firecode-/RS-Erfolg, nicht korrigierbare RS-Zeilen, AAC-CRC-/Decoderfehler, korrigierte RS-Symbole |
| `dls` | `slot`, `sid`, `text` – nur bei geändertem Text |
| `dl_plus` | `slot`, `sid`, `item_toggle`, `item_running`, `tags: [[content_type, text], ...]` – je DL+-Kommando (TS 102 980), alle Content-Types 0..63, Text = Ausschnitt des letzten vollständigen Labels |
| `mot_slide` | `slot`, `sid`, `mime`, `name`, `data_b64` (X-PAD-Slideshow eines Audiodienstes) |
| `mot_object` | `sid`, `content_type`, `name`, `data_b64` |
| `epg_object` | `sid`, `date_yyyymmdd`, `xml` |
| `announcement` | `kind`, `sub_ch`, `active` |
| `audio_format` | `rate`, `channels` (nach `service_started` und bei Wechsel; PCM ist immer als L/R-Paare unterwegs, `channels` = 2) |
| `audio_level` **LW** | `left`, `right` |
| `audio_underrun` | `missed` |
| `audio_devices` | `names[]`, `current?` |
| `ews_present` | |
| `ews_alert` | `phase: pre_trigger\|trigger\|sustain\|end`, `sub_ch`, `stage`, `iid`, `locations[]`, `is_test` |
| `ews_alive` | `sub_ch?` (Unterkanal des aktiven Alarms, höchstens 1/s Ensemble-Zeit; `null` = Heartbeat ohne Alarm, 1/s) |
| `ewf_alarm` | `active`, `sub_ch` |
| `ews_switched` | `to_sid`, `from_sid?` |
| `recording_state` | `slot`, `sid`, `active`, `path?`, `bytes`, `seconds` (bei Start, 1 Hz während der Aufnahme, bei Ende) |
| `timeshift_state` **LW** | `mode: live\|paused\|playing`, `buffered_s`, `offset_s`, `capacity_s` |
| `scan_progress` | `channel`, `index`, `total` |
| `scan_result` | `channel`, `eid?`, `ensemble?`, `services[]`, `snr` |
| `scan_finished` | |
| `tii` | `transmitters: [{main_id, sub_id, strength}]` |
| `spectrum` **LW** | `bins_b64` (dB 0..255 je Bin) |
| `iq_samples` **LW** | `iq_b64` |
| `log` | `level: error\|warn\|info\|debug`, `text` |
| `state_snapshot` | `state: {...}` (siehe `CoreState`) |
| `exiting` | `reason` |

## Beispiel

```
← {"type":"ready","core_version":"3.0.0","protocol_version":1,"decoders":["faad2"]}
→ {"type":"open_device","source":{"kind":"file","path":"P:/.../warnung-110030-2min.uff","loop":false}}
← {"type":"device_opened","name":"file","serial":"warnung-110030-2min.uff","bit_depth":8}
← {"type":"synced","synced":true}
← {"type":"ensemble_found","eid":4284,"name":"DR Deutschland","channel":"5C"}
← {"type":"service_added","service":{"sid":53776,"scids":0,"name":"Dlf","is_audio":true,"is_primary":true,"sub_ch":4,"bitrate_kbps":96,"pty":1}}
→ {"type":"select_service","sid":53776,"scids":0,"slot":"primary"}
← {"type":"service_started","slot":"primary","sid":53776,"scids":0,"codec":{"codec":"he_aac","sbr":true,"ps":false,"sample_rate":48000},"stereo":true}
← {"type":"audio_format","rate":48000,"channels":2}
← {"type":"dls","slot":"primary","sid":53776,"text":"Aus EUDI-Wallet wird \"d-you\" ..., Falk Steiner"}
← {"type":"dl_plus","slot":"primary","sid":53776,"item_toggle":false,"item_running":true,"tags":[[1,"Aus EUDI-Wallet wird \"d-you\" ..."],[4,"Falk Steiner"]]}
← {"type":"ews_alert","phase":"trigger","sub_ch":1,"stage":1,"iid":1,"locations":["Z1:5C+F300"],"is_test":false}
→ {"type":"shutdown"}
← {"type":"exiting","reason":"shutdown"}
```
