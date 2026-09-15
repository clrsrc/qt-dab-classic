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
| `set_gain` | `gain: {lna, vga, amp}` → `gain_changed`. Bei AGC an ist der Satz der neue **Ausgangspunkt** der Regelung (Ramp bzw. Bergsteiger starten dort neu); ein `set_gain` unmittelbar vor `set_channel` gibt damit den gespeicherten Kanalwert vor (siehe „Gain-Regelung“) |
| `set_agc` | `enabled` (Standard an, Entscheidung 26) → `gain_changed`. `false` friert den aktuellen Gain ein (keine Nachführung, im Scan läuft die Akquisitions-Ramp trotzdem und der Satz wird danach wiederhergestellt); `true` startet die Regelung ab dem aktuellen Gain neu |
| `set_ppm` | `ppm` (HackRF: über die Frequenz korrigiert, RTL-SDR: `rtlsdr_set_freq_correction`) |
| `select_service` | `sid`, `scids`, `slot: primary\|background`. Primary: Audio-Ausgabe + Aufnahme (höchstens einer; ein Wechsel bei laufender Aufnahme wird abgelehnt). Background: nur Backend + Aufnahme, **mehrere gleichzeitig** möglich (Entscheidung 24) |
| `stop_service` | `slot`, `sid?` (ohne `sid`: alle Dienste des Slots) |
| `start_scan` | `channels: []` (leer = alle 38 Band-III-Kanäle), `mode: single\|to_data\|continuous`. Nur mit Gerät; laufende Dienste werden gestoppt, `select_service` ist während des Scans abgelehnt. Je Kanal `scan_progress`, dann nach der Verweilzeit (v1 `switchDelay` 6 s, continuous 12 s) oder nach „kein Signal“ bei erschöpfter Akquisitions-Ramp ein `scan_result`: jedes `no_signal` hebt den Gain eine Ramp-Stufe (HackRF VGA +8, zuletzt einmal AMP an bei VGA 40), erst wenn nichts mehr zu probieren ist, gilt der Kanal als leer (ab VGA 24 nach ~5,5 s, ab dem zuletzt erfolgreichen Gain schneller); jeder Kanal startet mit dem zuletzt erfolgreichen Gain (`gain_changed` je Stufe); endet der Scan mitten in einer Ramp (letzter Kanal leer), geht der Gain auf den zuletzt erfolgreichen Wert ohne AMP zurück. Die Ramp läuft im Scan auch bei `set_agc{false}`; danach wird der Gain-Satz von vor dem Scan wiederhergestellt. `single`: alle Kanäle einmal, dann `scan_finished`, der Kern bleibt auf dem letzten Kanal; `to_data`: bis zum ersten Ensemble mit Diensten, dort bleiben; `continuous`: bis `stop_scan` |
| `stop_scan` | → `scan_finished` |
| `set_volume` | `percent` |
| `set_mute` | `muted` |
| `set_audio_device` | `index?` |
| `start_recording` | `path`, `format: {format: wav} \| {format: mp3, kbps, id3?} \| {format: aac_passthrough}`, `slot`, `sid?` (`wav`: 48 kHz, Stereo, 16 Bit, vor der Lautstärke abgegriffen; `mp3`: LAME CBR, `kbps` 32..320, Datei beginnt mit dem `id3`-Tag falls angegeben; `aac_passthrough` noch nicht umgesetzt, `log error`), `pre_s?` (additiv, M4: Vorlauf aus dem Timeshift-Ring, siehe „Timeshift“) |
| `stop_recording` | `slot`, `sid?` |
| `export_timeshift_range` | `from_s`, `to_s`, `path`, `format` (wie `start_recording`, additiv M4b: `mp3`) – `from_s`/`to_s` = Sekunden hinter live, `from_s > to_s >= 0`; der Kern kopiert die Rahmen aus dem Ring und dekodiert sie in einem eigenen Thread durch eine zweite Decoder-Instanz (~250× Echtzeit). `format: wav` → WAV (48 kHz, Stereo, 16 Bit); `format: mp3` → MP3 mit optionalem ID3v2.4-Tag vor den Rahmen; `aac_passthrough` noch nicht umgesetzt, `log warn` und stattdessen WAV. Ende: `recording_state{slot: primary, sid, active: false, path, bytes, seconds}` mit dem Exportpfad. Höchstens ein Export gleichzeitig (sonst `log warn`); ohne Primary-Dienst oder bei leerem Bereich nur `log warn` |
| `id3` (additiv in `format: mp3`) | `title?`, `artist?`, `album?`, `date?` (ISO `yyyy-mm-dd`) als ID3v2.4-Textrahmen (`TIT2`/`TPE1`/`TALB`/`TDRC`, UTF-8); `cover_png_b64?` als `APIC` (Cover front) – **nur PNG**, andere Formate werden vom Kern nicht geprüft/konvertiert, die App muss PNG liefern. Alle Felder optional, leerer/fehlender Tag wird nicht geschrieben |
| `start_iq_dump` / `stop_iq_dump` | `path` – Samples der Quelle als `.uff` (Qt-DAB-XML-Format, 8 Bit: HackRF `int8`, RTL-SDR `uint8`, Datei-Quelle `int8` der resampelten 2,048 MS/s); von `open_device{file}` wieder lesbar |
| `start_frame_dump` / `stop_frame_dump` | `path` |
| `timeshift_configure` | `capacity_s` (60..14400, wird geklemmt, Standard 3600), `backing: {backing: ram} \| {backing: disk, dir}` (`disk` wird angenommen, aber wie `ram` behandelt und geloggt – Entscheidung 4). Nur bei **geänderter** Kapazität wird der Ring neu angelegt (und ist dann leer); der Speicherbedarf steht als `log info` |
| `timeshift_pause` / `timeshift_play` / `timeshift_live` | `pause`: live/playing → `paused`, der Lesezeiger bleibt stehen (der Ring füllt sich weiter). `play`: `paused` → `playing`, ein 24-ms-Takt im Kern spielt ab dem Lesezeiger; erreicht der Lesezeiger den Schreibzeiger, geht es ohne Ruckeln zurück auf `live`. `live`: Lesezeiger = Schreibzeiger, Zustand `live`, **Audio-Puffer wird verworfen** (sonst hört man noch ~0,7 s Altes). Ohne Primary-Audiodienst: `log warn`, sonst nichts |
| `timeshift_seek` | `offset_s` = Sekunden hinter live (0 = live), geklemmt auf 0..`buffered_s`; der Zustand (`paused`/`playing`) bleibt. Aus `live` heraus mit `offset_s > 0` wird `playing` (sonst zöge der Schreibzeiger den Lesezeiger sofort wieder mit) |
| `timeshift_skip` | `delta_s` relativ: `+` geht Richtung live, `−` zurück; über live hinaus = `timeshift_live`. Aus `live` zurück gesprungen wird `playing` (wie `timeshift_seek`) |
| `set_ews` | `enabled`, `autoswitch` (Standard beide an). `autoswitch` steuert die automatische Umschaltung auf den Warndienst bei `trigger`/`sustain` (siehe `ews_switched`); `enabled` wirkt nur auf App-Seite (Alarmfenster/Ton) |
| `ews_dismiss` | |
| `set_home_location` | `lat`, `lon` (beide `number` oder `null`; fehlende Felder gelten als `null`). Heimatposition für das Geofencing der Alarm-Ortscodes (ETSI TS 104 089 Annex F, ASA DE Klausel 7.5/7.6). Der Kern rechnet damit `ews_alert.relevant` aus und schaltet nur bei relevanten Alarmen um. Ohne gesetzte Position (`null`, Ausgangszustand) gilt jeder Alarm als relevant. Die App schickt das Kommando beim Start und nach jeder Änderung der Standort-Einstellung |
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
| `gain_changed` | `lna`, `vga`, `amp`, `agc` – nach `set_gain`/`set_agc`, beim Öffnen eines Geräts und bei jeder Änderung durch die Regelung (nur bei tatsächlich geändertem Satz): Akquisitions-Ramp-Stufe (HackRF VGA +8 je `no_signal`, RTL-SDR +3 Tabellenstufen), AMP-Versuch/Rückfall, Probeschritt und Rücknahme des Bergsteigers (HackRF VGA ±4, RTL-SDR ±1 Stufe), Start beim Kanalwechsel; im Haltezustand kommt mindestens 15 s nichts (siehe „Gain-Regelung“). Die App speichert den Satz je Gerät und Kanal (Entscheidung 26); sinnvoll ist der zuletzt gemeldete Wert bei Sync, nicht ein Wert während der Ramp. `agc` ist der Wunsch aus `set_agc`, auch während der Scan-Ramp. HackRF: LNA 0–40 (8er-Schritte), VGA 0–62 (2er), AMP an/aus; RTL-SDR: `lna` = Tuner-Gain in 0,1 dB (nächster Tabellenwert), `vga`/`amp` ohne Bedeutung |
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
| `ews_alert` | `phase: pre_trigger\|trigger\|sustain\|end`, `sub_ch`, `stage`, `stage_raw`, `iid`, `locations[]`, `is_test` – `stage` = Bits 6..4 des Status-Bytes der FIG 0/15, `stage_raw` = das ganze Status-Byte (Bit 7 Last, Bits 6..4 Stage, Bits 3..0 IId) der ersten Trigger-Instanz; Warntag 2026: `0x01`; bei `sustain` ohne gesehenen Trigger und bei `end` der zuletzt gemerkte Wert (sonst 0). Rust liest fehlendes `stage_raw` als 0. Additiv: `relevant` (`true\|false\|null`) = Geofencing-Ergebnis des Kerns – er übersetzt jeden Ortscode aus `locations[]` (Annex-F-Quadtree, z. B. `Z1:5C+F300`) in Mittelpunkt + Näherungsradius und vergleicht ihn mit der per `set_home_location` gesetzten Position: `true` = mindestens ein Code deckt sie ab, `false` = keiner (dann **keine** Umschaltung, siehe `ews_switched`), `null` = keine Heimatposition gesetzt, also unbekannt und wie bisher immer relevant. Ein Alarm ohne brauchbare Ortscodes (leere `locations[]` wie bei `end`, oder nur unlesbare Codes) schränkt kein Gebiet ein und gilt als relevant. Nicht dekodierbare Codes werden übersprungen und zählen nicht gegen die Relevanz. Rust liest fehlendes `relevant` als `null` |
| `ews_alive` | `sub_ch?` (Unterkanal des aktiven Alarms, höchstens 1/s Ensemble-Zeit; `null` = Heartbeat ohne Alarm, 1/s) |
| `ewf_alarm` | `active`, `sub_ch` |
| `ews_switched` | `to_sid`, `from_sid?` – bei `trigger`/`sustain` (kein Testalarm, `autoswitch` an, der Alarm betrifft den eigenen Standort (`ews_alert.relevant` ≠ `false`, siehe `set_home_location`) **und ein Primary-Dienst lief bereits**) wechselt der Kern den Primary-Slot auf den Audiodienst des in `ews_alert.sub_ch` gemeldeten Unterkanals (`to_sid` = Warndienst, `from_sid` = vorheriger Dienst); bei `end` zurück (`to_sid` = vorheriger Dienst, `from_sid` = Warndienst). Ohne laufenden Primary-Dienst (z. B. reiner EPG-Empfang, Headless-Betrieb ohne `select_service`) bleibt der Alarm rein informativ, es kommt kein `ews_switched`. Dasselbe gilt bei `relevant: false` – der Kern schreibt dann eine `log info`-Zeile mit dem Grund (Geofencing); das trifft z. B. den „Eiffelturm“-Funktionstest des Bundesmux, dessen Ortscodes Paris abdecken, von einem deutschen Standort aus. Die Rückschaltung bei `end` hängt nicht am Geofencing. Kommt nach dem jeweiligen `ews_alert` |
| `recording_state` | `slot`, `sid`, `active`, `path?`, `bytes`, `seconds` (bei Start, 1 Hz während der Aufnahme, bei Ende) |
| `timeshift_state` **LW** | `mode: live\|paused\|playing`, `buffered_s` (Inhalt des Rings), `offset_s` (Abstand Lesezeiger → live, in `live` immer 0), `capacity_s`; additiv (M4): `frame_index` (Schreibzeiger, 24-ms-Rahmen seit dem letzten Leeren) und `live_unix` (Ensemble-Uhrzeit am Schreibzeiger, 0 = unbekannt). Kommt mit **2 Hz**, solange ein Primary-Audiodienst läuft, und sofort bei jedem Zustandswechsel; nach dem Ende des Dienstes einmal mit `buffered_s` 0 |
| `scan_progress` | `channel`, `index`, `total` |
| `scan_result` | `channel`, `eid?`, `ensemble?`, `services[]`, `snr` |
| `scan_finished` | |
| `tii` | `transmitters: [{main_id, sub_id, strength}]` – Sender im Nullsymbol (v1 TII-Detector, Auswertung alle 3 TII-Nullsymbole). Der Kern sendet **höchstens 1×/s und nur bei geänderter Liste** (IDs oder Stärke auf 0,01 gerundet); eine leer gewordene Liste kommt einmal als `[]`. 5C in Langenberg-Reichweite: mainId 20 mit subIds 4 (Langenberg), 2 (Düsseldorf), 1 (Köln), Stärken ~0,3/0,25/0,12 |
| `spectrum` **LW** | `bins_b64` – 2048 Bins (u8) der Eingangssamples nach Frequenzkorrektur, fftshift (Bin 0 = −1,024 MHz, Bin 1024 = Trägermitte), 0,5 dB je Stufe: `dBFS = Wert / 2 − 120` (0 = −120 dBFS, 240 = 0 dBFS). Nur mit `set_scopes{spectrum:true}`, höchstens `rate_hz`/s; kommt auch ohne Sync (Antennenausrichtung) |
| `iq_samples` **LW** | `iq_b64` – Konstellation von OFDM-Symbol 2 (wie das v1-IQ-Scope): 1536 Träger nach der Differenzdemodulation in Frequenzreihenfolge (k = −768…−1, 1…768), je Träger auf den Einheitskreis normiert, als 3072 int8-Werte `I0,Q0,I1,Q1,…` (127 = 1,0), Base64 (4096 Zeichen, ~4,1 kB je Ereignis). Nur mit `set_scopes{iq:true}` und nur bei Sync, höchstens `rate_hz`/s |
| `log` | `level: error\|warn\|info\|debug`, `text` |
| `state_snapshot` | `state: {...}` (siehe `CoreState`). Neben Quelle/Kanal/Gain/Diensten: `ensemble: [eid, name]`, `timeshift: [mode, buffered_s, offset_s, capacity_s]` (`null`, solange kein Primary-Audiodienst läuft), `epg_enabled`, `tii_enabled`, `tii_threshold`, `tii_dx_mode`, `scopes{spectrum,iq,rate_hz}`, `snr` (letzter Wert), `clock_time{unix_utc,lto_minutes}?` (letztes `clock_time`), `ppm`, `scanning` und `running[]` – je laufendem Dienst (alle Slots): `slot`, `sid`, `scids`, `name`, `is_audio`, `codec?` (wie `service_started`, `null` bis zum ersten dekodierten Block), `stereo`, `dls?` (letzter Text), `dl_plus?` (`item_toggle`, `item_running`, `tags`), `slide?` (`mime`, `name`, `data_b64` der letzten `mot_slide`), `recording{active,path?,bytes,seconds}`. Damit kann eine neu verbundene App die Anzeige ohne Warten auf neue Ereignisse aufbauen |
| `exiting` | `reason` |

## Gain-Regelung (AGC)

Kern: `core-cpp/libdabcore/src/device/agc-controller.{h,cpp}` (geräteneutral, ctest `agc_controller`).
Sie arbeitet auf einer Gain-Stufe 0..N des Geräts – HackRF: VGA/2 (0..31, 2-dB-Schritte) plus AMP-Flag, LNA bleibt wie gesetzt (Standard 40); RTL-SDR: Index in die Tuner-Gain-Tabelle, kein AMP.
Hintergrund (Messung 12.09.2026, HackRF/NRW): der Standard-VGA 24 reicht für 11D nicht zum Sync, der AMP übersteuert (kein Sync), und oberhalb von VGA ~48 sinkt der SNR wieder.

| Zustand | Verhalten |
|---|---|
| **Akquisition** (kein Sync) | Bei jedem `no_signal` (≈ alle 0,77 s) eine Ramp-Stufe höher: HackRF VGA 24 → 32 → 40 → 48 → 56 → 62, dann **einmal AMP an bei VGA 40**, danach zurück auf VGA 40 / AMP aus und Ende der Ramp (keine Endlosschleife; RTL-SDR: +3 Tabellenstufen bis zum Maximum, dann Rückfall auf den Standardindex). Startpunkt: der aktuell gesetzte Gain – nach `set_gain` genau dieser, sonst der zuletzt erfolgreiche Gain (Sync **mit** dekodierten FIBs). Neustart der Ramp bei `set_channel`, `set_gain`, `set_agc{true}`. **Schein-Sync:** meldet der ofdmHandler `synced` (ggf. flatternd), aber 2,5 s lang keine dekodierten FIBs (`fic_quality` ok = 0; Befund 11D bei VGA 24: SNR 3 dB, kein Ensemble, kaum `no_signal`), zählt das wie ein `no_signal` – eine Ramp-Stufe höher. Der ofdmHandler meldet `no_signal` außerdem zeitbasiert (~0,77 s ohne Sync-Fortschritt), nicht mehr nur bei „kein Null-Symbol-Dip“. |
| **Tracking** (Sync) | Bergsteiger auf dem SNR: nach jeder Stufenänderung 2 s einschwingen (der SNR kommt vom ofdmHandler als träge EMA), 1 s mitteln (nach einem Kanalwechsel zuerst 3,5 s + 1 s), dann Probeschritt (HackRF VGA +4). Mittelwert ≥ 0,25 dB besser → behalten, weiter in derselben Richtung; sonst zurück, Basis neu messen, andere Richtung. Beide Richtungen ohne Gewinn → **Halten**. Der AMP wird im Tracking nie verändert; ein erfolgreicher AMP-Versuch aus der Akquisition bleibt. Sync-Verlust → zurück in die Akquisition, das erste `no_signal` ändert noch nichts (kurzes Fading), danach Ramp ab der aktuellen Stufe (AMP-Versuch nur einmal je Kanal). |
| **Halten** | 15 s keine Änderung; danach Basis neu messen: nur bei um ≥ 0,25 dB verändertem SNR (spätestens in jeder 4. Haltephase, also nach ≤ 60 s) wird neu sondiert – Hysterese gegen dauernde `gain_changed`. |
| **AGC aus** | Eingefroren: `no_signal`/`snr` ändern nichts; `set_gain` wirkt direkt. Ausnahme Scan (siehe `start_scan`). |

Typische Werte am Messort: 11D Sync nach ~1,2 s (bei VGA 32), Tracking auf VGA 40–44 bei 6,5–9 dB; 5C sofort Sync, VGA 36–44 bei 8–10 dB; 9B/9D/12D VGA 44; AMP im Endzustand nie an.

## Timeshift

Kern: `core-cpp/libdabcore/src/backend/timeshift-buffer.{h,cpp}` (Ring, ctest `timeshift_buffer`) und
`timeshift-controller.{h,cpp}` (Takt-Thread, Kommandos); ctest `dabcored_timeshift`.

Gespeichert werden die **Hardbits** des Subkanals (nicht PCM): je 24-ms-Rahmen `bitRate * 3` Byte
gepackt, also 13 kB/s bei 104 kbit/s – eine Stunde Dlf ≈ 46,8 MB (Entscheidung 4, alles im RAM).
Der Ring hängt nur am **Primary-Audiodienst** und beginnt neu bei: Dienstwechsel des Primary-Slots,
`stop_service`, Kanalwechsel, `close_device`, `timeshift_configure` mit anderer Kapazität und bei
`ews_alert{phase: trigger}` (Entscheidung 5: der Alarm verlässt den Zeitversatz immer, `log info`
„Timeshift: Zeitversatz verworfen (Notfallwarnung)“).

In `paused` und `playing` bekommt der Decoder nur das, was der Ring liefert; die Ausgabe läuft weiter
(keine Umschaltlatenz), Stille entsteht durch fehlende Rahmen. `audio_underrun` wird in diesen
Zuständen **nicht** gemeldet, ebenso nicht in der ersten Sekunde nach `timeshift_live` (der Decoder
braucht bis zu einen Superframe, bis wieder Ton kommt).

**Aufnahme-Vorlauf** (`start_recording{pre_s}`, Entscheidung 18): Der WAV-Schreiber kann nicht
anhängen, deshalb schreibt der Kern den Vorlauf in eine **eigene Datei** neben der Aufnahme –
`<name>_vorlauf.wav` (`aufnahme.wav` → `aufnahme_vorlauf.wav`). Die laufende Aufnahme beginnt
unverändert live; der Vorlauf wird im Hintergrund dekodiert und nur als `log info` gemeldet (kein
eigenes `recording_state`, damit die App die laufende Aufnahme nicht als beendet sieht). Hat der Ring
weniger als 1 s Inhalt, entfällt der Vorlauf mit `log info`.

`pre_s` kennt bisher nur der Kern: in `dab-api` fehlt das Feld in `Command::StartRecording` noch
(ein zusätzliches Feld in dieser Variante müsste zusammen mit den Aufrufstellen in `dab-app`
eingebaut werden). Wer es heute nutzen will, schickt die JSON-Zeile direkt.

## EWS-Geofencing (Ortscodes)

Kern: `core-cpp/libdabcore/src/fic/ews-location.{h,cpp}` (reine Rechnung, ctest `ews_location`;
Ende-zu-Ende am Warntag-Mitschnitt: ctest `dabcored_ews_geofence`). Rust-Zwilling für die Anzeige:
`crates/dab-app/src/ews_location.rs`.

Die `locations[]` einer FIG 0/15 sind keine benannten Regionen, sondern ein **Quadtree über der
Erdkugel** (ETSI TS 104 089 Annex F): 6-Bit-Zone (36×36-Grad-Kachel bzw. Polzone) und bis zu sechs
weitere 4-Bit-Ziffern, die die Kachel je Stufe in 4×4 Felder teilen – gerendert als
`Z<Zone>:<Hexziffern>[+<Subcode>]`, z. B. `Z1:5C+F300`. Zur Anzeige (Alarmfenster, EWF-Historie)
rechnet der Kern jeden Code auf Mittelpunkt und Näherungsradius (halbe Diagonale der Kachel) zurück
(`decodeEwsLocation`); die Subcode-Bitmaske wird dafür ignoriert, das Ergebnis ist also nie feiner
als der Code selbst.

**Fürs Matching selbst** (`ews_alert.relevant`) verwendet der Kern **nicht** diese Näherung, sondern
den von ETSI TS 104 089 Klausel 7.5.4 vorgeschriebenen **Ziffernvergleich**: aus der per
`set_home_location` gesetzten Position wird einmalig der eigene Standort-Code bei maximaler Auflösung
berechnet (`encodeEwsLocation`, Umkehrung von `decodeEwsLocation`). Für jeden Alarm-Ortscode wird der
eigene Code links-bündig auf dessen Ziffernzahl gekürzt und auf exakte Übereinstimmung von Zone und
Ziffern geprüft; trägt der Code zusätzlich einen Subcode (Annex D.2.3 – ein Stem-Code, eine Ziffer
kürzer, plus 16-Bit-Bitmaske über die möglichen Kindzellen), muss zusätzlich das Bit der eigenen
nächsten Ziffer gesetzt sein. `relevant: true`, sobald irgendein Code im Alarm-Satz trifft. Kein
Distanz-/Trigonometrie-Aufwand – der Vergleich ist reine Ganzzahl-/Bit-Arithmetik, entsprechend der
Auskunft von Digitalradio Deutschland e. V., der Algorithmus sei „sehr daten- und rechenzeitsparsam“.

Die Entscheidung fällt **im Kern**, weil die Umschaltung auf den Warndienst synchron im FIC-Thread
passiert – für eine Rückfrage bei der App ist keine Zeit. Wichtig dabei: die Trigger-Phase kann sich
über bis zu vier FIG-0/15-Instanzen aufbauen (Annex E, C/N-Flag startet den Satz, NFF = 0 beendet ihn);
`ews_alert` für `trigger` wird deshalb erst emittiert, wenn das Alarmgebiet vollständig gesammelt ist,
sonst würde ein Ortscode, der erst in einer späteren Instanz kommt, fälschlich als „nicht relevant“
gewertet. `pre_trigger` sammelt (noch) nicht über mehrere Instanzen und kann daher kurzzeitig auf einem
unvollständigen Ausschnitt beruhen – unschädlich, da `pre_trigger` nie eine Umschaltung auslöst (nur
`trigger`/`sustain`/`end` tun das), höchstens der Statusleisten-Hinweis kurz ungenau ist, bis das
folgende `trigger`-Ereignis nachzieht.

Hintergrund (Auskunft Digitalradio Deutschland e. V., 09/2026): der alle fünf Minuten im Bundesmux
laufende Alarm ist ein **Funktionstest („Eiffelturm-Alert“)** mit echten Ortscodes um Paris, damit der
Handel das Aufwachen aus dem Standby vorführen kann (laut der ASA-Anleitung alle 5 Minuten für
30 Sekunden, absichtlich ohne Warndurchsage). Ein Empfänger mit deutscher Heimatposition soll ihn
ignorieren; ein Gerät, das auf diese Pariser Position eingestellt ist, reagiert – handelsübliche
ASA-Radios bekommen dafür den Testcode `1253-3513-3668` als „Standort-Code“ eingetippt (ETSI TS 104 089
Annex A, oktal + Prüfsumme kodierte Form desselben Ortscodes, siehe `decode_presentation_code` in
`crates/dab-app/src/ews_location.rs`; echte Standortcodes adressgenau über www.asa.radio). Echte Alarme
haben immer Vorrang: ohne gesetzte Heimatposition und bei Alarmen ohne brauchbare Ortscodes bleibt es
beim bisherigen Verhalten (umschalten).

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
→ {"type":"set_home_location","lat":51.218,"lon":6.7617}
← {"type":"ews_alert","phase":"trigger","sub_ch":1,"stage":0,"stage_raw":1,"iid":1,"locations":["Z1:5C+F300"],"is_test":false,"relevant":true}
→ {"type":"shutdown"}
← {"type":"exiting","reason":"shutdown"}
```
