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
| `open_device` | `source: {kind: hack_rf, serial?} \| {kind: rtl_sdr, index} \| {kind: file, path, loop}` |
| `close_device` | |
| `set_channel` | `channel` ("5C") |
| `set_gain` | `gain: {lna, vga, amp}` |
| `set_agc` | `enabled` |
| `set_ppm` | `ppm` |
| `select_service` | `sid`, `scids`, `slot: primary\|background` |
| `stop_service` | `slot` |
| `start_scan` | `channels: []`, `mode: single\|to_data\|continuous` |
| `stop_scan` | |
| `set_volume` | `percent` |
| `set_mute` | `muted` |
| `set_audio_device` | `index?` |
| `start_recording` | `path`, `format: {format: wav} \| {format: mp3, kbps} \| {format: aac_passthrough}`, `slot` |
| `stop_recording` | `slot` |
| `export_timeshift_range` | `from_s`, `to_s`, `path`, `format` |
| `start_iq_dump` / `stop_iq_dump` | `path` |
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
| `device_error` | `message` |
| `file_progress` **LW** | `position_s`, `length_s` |
| `file_ended` | |
| `synced` | `synced` |
| `no_signal` | `channel` |
| `snr` **LW** | `db` |
| `fic_quality` **LW** | `ok`, `total` |
| `frequency_offset` **LW** | `hz` |
| `ensemble_found` | `eid`, `name`, `channel` |
| `service_added` | `service: {sid, scids, name, is_audio, is_primary, sub_ch, bitrate_kbps, pty}` |
| `ensemble_reconfigured` | |
| `clock_time` | `unix_utc`, `lto_minutes` |
| `service_started` | `slot`, `sid`, `scids`, `codec: {codec: he_aac, sbr, ps, sample_rate} \| {codec: mp2, sample_rate}`, `stereo` |
| `service_stopped` | `slot` |
| `service_stats` **LW** | `slot`, `frame_errors`, `rs_errors`, `aac_errors`, `rs_corrections` |
| `dls` | `slot`, `text` |
| `dl_plus` | `slot`, `item_toggle`, `item_running`, `tags: [[content_type, text], ...]` |
| `mot_slide` | `slot`, `mime`, `name`, `data_b64` |
| `mot_object` | `sid`, `content_type`, `name`, `data_b64` |
| `epg_object` | `sid`, `date_yyyymmdd`, `xml` |
| `announcement` | `kind`, `sub_ch`, `active` |
| `audio_format` | `rate`, `channels` |
| `audio_level` **LW** | `left`, `right` |
| `audio_underrun` | `missed` |
| `audio_devices` | `names[]`, `current?` |
| `ews_present` | |
| `ews_alert` | `phase: pre_trigger\|trigger\|sustain\|end`, `sub_ch`, `stage`, `iid`, `locations[]`, `is_test` |
| `ews_alive` | `sub_ch` |
| `ewf_alarm` | `active`, `sub_ch` |
| `ews_switched` | `to_sid`, `from_sid?` |
| `recording_state` | `slot`, `active`, `path?`, `bytes`, `seconds` |
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
← {"type":"ews_alert","phase":"trigger","sub_ch":1,"stage":1,"iid":1,"locations":["Z1:5C+F300"],"is_test":false}
→ {"type":"shutdown"}
← {"type":"exiting","reason":"shutdown"}
```
