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
| `set_epg` | `enabled` (Standard an). Der Kern startet den SPI/EPG-Paketdienst des Ensembles (FIG 0/13 Appl-Type 7, DSCTy 60, im Bundesmux „EPG Deutschland“) selbst als Background-Slot, sobald die FIC ihn meldet – auch ohne Primary-Dienst (z. B. nach dem Scan); Kanalwechsel/`close_device` beenden ihn, `enabled: false` beendet nur die vom Kern gestarteten Dienste. `service_started{slot: background, codec: {codec: data}}` wie bei `select_service`; `state.epg_enabled` |
| `set_scopes` | `spectrum`, `iq` (getrennt schaltbar), `rate_hz` (1–10, Standard 5, gemeinsame Rate; der Kern klemmt). Wirkt sofort und bleibt über `open_device`/`set_channel` erhalten. Beide aus (Standard): kein Datenfluss und kein Rechenaufwand |
| `set_tii` | `enabled` (Standard an), `threshold` (Standard 6), `dx_mode` (wird gemerkt, `state.tii_dx_mode`; im Kern heute **ohne Wirkung** – in v1 war es ein Anzeigemodus des TII-Fensters) |
| `get_state` | → `state_snapshot`, danach `audio_devices` |
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
| `mot_object` | `eid`, `sid`, `content_type`, `name`, `data_b64` – Objekt aus dem SPI-Paketdienst (Logos PNG `0x0203`/JPEG `0x0201`, selten Text). `sid` = Dienst, dem das Logo gilt: die Hex-SId vor dem ersten `_` des Namens (`d210_Dlf_320x240.png`, `10c4_ASA DE_320x240.png`), wenn sie ein Dienst des Ensembles ist, sonst 0. Jedes Objekt kommt einmal; Wiederholungen des Karussells (neue MOT-Verzeichnisversion, Bundesmux alle ~25 min) meldet der Kern nur bei geändertem Inhalt. Cache (App): `data/logos/<eid>/<name>`, größtes PNG je SId; die Zuordnung Logo→Dienst steht außerdem in der Service-Information (`epg_object` mit `sid` 0) |
| `epg_object` | `eid`, `sid`, `date_yyyymmdd`, `name`, `xml` – XML-Text des portierten epg-compilers (TS 102 371 → Format wie v1 `Qt-DAB-files/<EId>/<yyyyMMdd>_<SId>_SI.xml`, Wurzel `<epg system="DAB" tz="local">`, Zeiten `yyyy-M-dTHH:mm` in der **Ortszeit des Kern-Systems** (Sendezeit ist UTC, Umrechnung per `localtime` inkl. Sommerzeit), Dauer `PTnnHnnM`). Das Attribut `tz="local"` unterscheidet neue Dateien von v1-Dateien ohne Attribut, deren Zeiten UTC + LTO-*Minuten* waren (v1-Fehler: `mktime` als Ortszeit plus `lto` Stunden als Minuten addiert – am Bundesmux UTC + 2 min). Steuerzeichen (< 0x20 außer Tab/LF/CR) werden beim Serialisieren entfernt, alles andere (z. B. `width="257"`) bleibt wie v1. Sendeplan: `sid`/`date_yyyymmdd` aus dem MOT-Namen (`w20260914dd230c0.EHB` → 20260914 / 0xD230, wie v1 `extractName`), Cache (App): `data/epg/<eid>/<yyyymmdd>_<SID>_SI.xml`. Service-Information (Logo-Zuordnung, Wurzel `<serviceInformation>`, v1 `list.xml`): `sid` = 0, `date_yyyymmdd` = 0. Im Bundesmux senden nur die Deutschlandradio-Dienste (Dlf, Dlf Kultur, Dlf Nova) EPG, je Tag eine Datei, heute bis +5 Tage; ein Karussell-Umlauf dauert einige Minuten |
| `announcement` | `kind`, `sub_ch`, `active` |
| `audio_format` | `rate`, `channels` (nach `service_started` und bei Wechsel; PCM ist immer als L/R-Paare unterwegs, `channels` = 2) |
| `audio_level` **LW** | `left`, `right` |
| `audio_underrun` | `missed` |
| `audio_devices` | `names[]`, `current?` – nach `ready` (als drittes Ereignis, nach der Viterbi-Log-Zeile), nach `get_state` (direkt nach `state_snapshot`) und nach `set_audio_device`; mit `--no-audio` leere Liste und `current: null` |
| `ews_present` | |
| `ews_alert` | `phase: pre_trigger\|trigger\|sustain\|end`, `sub_ch`, `stage`, `stage_raw`, `iid`, `locations[]`, `is_test` – `stage` = Bits 6..4 des Status-Bytes der FIG 0/15, `stage_raw` = das ganze Status-Byte (Bit 7 Last, Bits 6..4 Stage, Bits 3..0 IId) der ersten Trigger-Instanz; Warntag 2026: `0x01`; bei `sustain` ohne gesehenen Trigger und bei `end` der zuletzt gemerkte Wert (sonst 0). Rust liest fehlendes `stage_raw` als 0 |
| `ews_alive` | `sub_ch?` (Unterkanal des aktiven Alarms, höchstens 1/s Ensemble-Zeit; `null` = Heartbeat ohne Alarm, 1/s) |
| `ewf_alarm` | `active`, `sub_ch` |
| `ews_switched` | `to_sid`, `from_sid?` |
| `recording_state` | `slot`, `sid`, `active`, `path?`, `bytes`, `seconds` (bei Start, 1 Hz während der Aufnahme, bei Ende) |
| `timeshift_state` **LW** | `mode: live\|paused\|playing`, `buffered_s`, `offset_s`, `capacity_s` |
| `scan_progress` | `channel`, `index`, `total` |
| `scan_result` | `channel`, `eid?`, `ensemble?`, `services[]`, `snr` |
| `scan_finished` | |
| `tii` | `transmitters: [{main_id, sub_id, strength}]` – Sender im Nullsymbol (v1 TII-Detector, Auswertung alle 3 TII-Nullsymbole). Der Kern sendet **höchstens 1×/s und nur bei geänderter Liste** (IDs oder Stärke auf 0,01 gerundet); eine leer gewordene Liste kommt einmal als `[]`. 5C in Langenberg-Reichweite: mainId 20 mit subIds 4 (Langenberg), 2 (Düsseldorf), 1 (Köln), Stärken ~0,3/0,25/0,12 |
| `spectrum` **LW** | `bins_b64` – 2048 Bins (u8) der Eingangssamples nach Frequenzkorrektur, fftshift (Bin 0 = −1,024 MHz, Bin 1024 = Trägermitte), 0,5 dB je Stufe: `dBFS = Wert / 2 − 120` (0 = −120 dBFS, 240 = 0 dBFS). Nur mit `set_scopes{spectrum:true}`, höchstens `rate_hz`/s; kommt auch ohne Sync (Antennenausrichtung) |
| `iq_samples` **LW** | `iq_b64` – Konstellation von OFDM-Symbol 2 (wie das v1-IQ-Scope): 1536 Träger nach der Differenzdemodulation in Frequenzreihenfolge (k = −768…−1, 1…768), je Träger auf den Einheitskreis normiert, als 3072 int8-Werte `I0,Q0,I1,Q1,…` (127 = 1,0), Base64 (4096 Zeichen, ~4,1 kB je Ereignis). Nur mit `set_scopes{iq:true}` und nur bei Sync, höchstens `rate_hz`/s |
| `log` | `level: error\|warn\|info\|debug`, `text` |
| `state_snapshot` | `state: {...}` (siehe `CoreState`). Neben Quelle/Kanal/Gain/Diensten: `ensemble: [eid, name]`, `epg_enabled`, `tii_enabled`, `tii_threshold`, `tii_dx_mode`, `scopes{spectrum,iq,rate_hz}`, `snr` (letzter Wert), `clock_time{unix_utc,lto_minutes}?` (letztes `clock_time`), `ppm`, `scanning` und `running[]` – je laufendem Dienst (alle Slots): `slot`, `sid`, `scids`, `name`, `is_audio`, `codec?` (wie `service_started`, `null` bis zum ersten dekodierten Block), `stereo`, `dls?` (letzter Text), `dl_plus?` (`item_toggle`, `item_running`, `tags`), `slide?` (`mime`, `name`, `data_b64` der letzten `mot_slide`), `recording{active,path?,bytes,seconds}`. Damit kann eine neu verbundene App die Anzeige ohne Warten auf neue Ereignisse aufbauen |
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
← {"type":"ews_alert","phase":"trigger","sub_ch":1,"stage":0,"stage_raw":1,"iid":1,"locations":["Z1:5C+F300"],"is_test":false}
→ {"type":"shutdown"}
← {"type":"exiting","reason":"shutdown"}
```
