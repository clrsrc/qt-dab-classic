// DAB Classic – Transportschicht zum Kern und zur App-Schicht (Rust).
//
// Design-Ziel Web-Remote (Entscheidung 23): Komponenten rufen NIE direkt
// Tauri auf, sondern nur diese Schnittstelle. Heute gibt es die
// Tauri-Implementierung (invoke + listen); ein WebSocket-Transport fuer den
// Browser kann spaeter dieselbe Schnittstelle bedienen.

import { invoke } from "@tauri-apps/api/core";
import { emit, listen } from "@tauri-apps/api/event";
import { open as openDialog } from "@tauri-apps/plugin-dialog";
import type { EpgAppEvent, NowNext } from "./epg";
import type { AddOutcome, EpgTimerRequest, RecordingInfo, SleepAction, SleepState, Timer, TimerAppEvent, Timers } from "./timers";
import type { DebugAppEvent, DebugState, TiiSeen } from "./debug";
import type { StationEntry, StationsAppEvent } from "./stations";
import type { TimeshiftAppEvent, TimeshiftInfo } from "./timeshift";
import type { MusicAppEvent, TrackCandidate } from "./music";

// ---------------------------------------------------------------------------
// Typen (spiegeln crates/dab-api und crates/dab-app; JSON snake_case)
// ---------------------------------------------------------------------------

export type ServiceSlot = "primary" | "background";

export interface Gain {
  lna: number;
  vga: number;
  amp: boolean;
}

export type SourceKind =
  | { kind: "hack_rf"; serial: string | null }
  | { kind: "rtl_sdr"; index: number }
  | { kind: "file"; path: string; loop: boolean; fast?: boolean };

export type Command =
  | { type: "open_device"; source: SourceKind }
  | { type: "close_device" }
  | { type: "set_channel"; channel: string }
  | { type: "set_gain"; gain: Gain }
  | { type: "set_agc"; enabled: boolean }
  | { type: "set_ppm"; ppm: number }
  | { type: "select_service"; sid: number; scids: number; slot: ServiceSlot }
  | { type: "stop_service"; slot: ServiceSlot }
  | { type: "start_scan"; channels: string[]; mode: "single" | "to_data" | "continuous" }
  | { type: "stop_scan" }
  | { type: "set_volume"; percent: number }
  | { type: "set_mute"; muted: boolean }
  | { type: "set_audio_device"; index: number | null }
  | { type: "set_ews"; enabled: boolean; autoswitch: boolean }
  | { type: "ews_dismiss" }
  | { type: "get_state" }
  | { type: "shutdown" };

export interface ServiceInfo {
  sid: number;
  scids: number;
  name: string;
  is_audio: boolean;
  is_primary: boolean;
  sub_ch: number;
  bitrate_kbps: number;
  pty: number;
}

export type Codec =
  | { codec: "he_aac"; sbr: boolean; ps: boolean; sample_rate: number }
  | { codec: "mp2"; sample_rate: number }
  | { codec: "data" };

export type EwsPhase = "pre_trigger" | "trigger" | "sustain" | "end";

export interface AppState {
  core_alive: boolean;
  core_version: string;
  core_restarts: number;
  device: { kind: string; name: string; serial: string } | null;
  device_error: string | null;
  channel: string | null;
  synced: boolean;
  snr: number;
  fic_ok: number;
  fic_total: number;
  ensemble: { eid: number; name: string; channel: string } | null;
  services: ServiceInfo[];
  current: { sid: number; scids: number; codec: Codec | null; stereo: boolean } | null;
  dls: string;
  dl_plus: { item_running: boolean; item_toggle: boolean; tags: [number, string][] } | null;
  slide: { sid: number; mime: string; name: string; data_b64: string; received_at: number } | null;
  level: [number, number];
  gain: Gain;
  agc: boolean;
  volume: number;
  muted: boolean;
  scan: { active: boolean; channel: string; index: number; total: number; results: ScanResult[] };
  ews_present: boolean;
  alert: Alert | null;
  ews_switched_from: number | null;
  recording: boolean;
  file: { path: string; loop: boolean; position_s: number; length_s: number; ended: boolean } | null;
  audio_devices: string[];
  audio_device_current: number | null;
  pending: { slot: number | null; channel: string; name: string } | null;
  clock_utc: number | null;
  log_tail: string[];
  /** EPG/Logos (lib/epg.ts, lib/logos.ts): Logo des aktuellen Dienstes (128x128) und Now/Next. */
  logo_data_url: string | null;
  now_next: NowNext | null;
  /** TII/Debug-Panel (lib/debug.ts, lib/tii.ts): Sender im Nullsymbol, Zaehler. */
  tii: TiiSeen[];
  debug: DebugState;
  /** Senderliste ueber alle Ensembles (lib/stations.ts), sortiert Kanal/Ensemble/Name. */
  stations: StationEntry[];
  /** Timeshift-Puffer des laufenden Dienstes (lib/timeshift.ts, Entscheidung 4). */
  timeshift: TimeshiftInfo;
  /** Vorschlagsliste der Musik-Trennung (lib/music.ts, Entscheidungen 6, 7). */
  music_candidates: TrackCandidate[];
  /** Abgeschlossene Alarme dieser Sitzung, neueste zuerst (Bugfixes.txt #10). */
  ews_history: EwsHistoryEntry[];
  /** Verkehrs-/Sonderdurchsagen (dab_app::traffic): laufende, Historie, Unterstuetzung des aktuellen Dienstes. */
  traffic_active: TrafficEntry | null;
  traffic_history: TrafficEntry[];
  traffic_supported: boolean;
}

/** Eine Durchsage (dab_app::traffic::TrafficEntry, FIG 0/18 + 0/19). */
export interface TrafficEntry {
  id: number;
  started_at: number;
  ended_at: number | null;
  channel: string;
  ensemble: string;
  /** ASw-Flags (EN 300 401 8.1.6.2): Bit 1 = Verkehr, Bit 2 = Nahverkehr, ... */
  flags: number;
  cluster: number;
  /** Subkanal, auf dem die Durchsage laeuft, und der Dienst dazu (falls bekannt). */
  sub_ch: number;
  announcing_service: string | null;
  /** Dienste, fuer die die Durchsage gilt (Cluster-Mitglieder). */
  services: string[];
  /** Die App hat waehrend der Durchsage auf den Durchsage-Dienst umgeschaltet. */
  switched: boolean;
}

/** Abgeschlossener Alarm (dab_app::state::EwsHistoryEntry). */
export interface EwsHistoryEntry {
  ended_at: number;
  sub_ch: number;
  stage: number;
  stage_raw: number;
  iid: number;
  locations: string[];
  location_info: EwsLocationInfo[];
  is_test: boolean;
  /** Geofencing-Urteil des Kerns, siehe `Alert.relevant`. */
  relevant: boolean | null;
}

export interface ScanResult {
  channel: string;
  eid: number | null;
  ensemble: string | null;
  services: ServiceInfo[];
  snr: number;
}

export interface Alert {
  phase: EwsPhase;
  sub_ch: number;
  stage: number;
  /** Rohes Status-Byte der FIG 0/15 (Warntag 2026: 0x01); 0 bei aelterem Kern. */
  stage_raw: number;
  iid: number;
  locations: string[];
  /** Ortscodes uebersetzt (Mittelpunkt, Entfernung/Richtung von zu Hause). */
  location_info: EwsLocationInfo[];
  is_test: boolean;
  /** Geofencing-Urteil des Kerns (ETSI TS 104 089 Klausel 7.5/7.6): `null` =
   * keine Heimatkoordinaten hinterlegt, jeder Alarm gilt; `true` = ein Ortscode
   * deckt den eigenen Standort ab; `false` = keiner (z. B. der Eiffelturm-
   * Testalarm). Nur lesen, nie nachrechnen - der Kern entscheidet. */
  relevant: boolean | null;
  dismissed: boolean;
}

/** Uebersetzung eines DAB-EWS-Ortscodes (ETSI TS 104 089 Annex F, dab_app::ews_location). */
export interface EwsLocationInfo {
  code: string;
  lat: number | null;
  lon: number | null;
  radius_km: number | null;
  distance_km: number | null;
  azimuth_deg: number | null;
}

export interface Preset {
  channel: string;
  eid: number;
  sid: number;
  scids: number;
  name: string;
  logo_path: string | null;
  logo_data_url?: string | null;
  stored_at: number;
}

export interface Presets {
  version: number;
  slots: (Preset | null)[];
}

export interface Panels {
  presets: boolean;
  /** Tab "Ensemble" (Dienste des abgestimmten Ensembles). */
  services: boolean;
  /** Tab "Senderliste" (lib/stations.ts). */
  stations: boolean;
  settings: boolean;
  scan: boolean;
  epg: boolean;
  timer: boolean;
  debug: boolean;
  /** Panel "Musik" (lib/music.ts). */
  music: boolean;
  /** EWF-Historie (Bugfixes.txt #10), Button "EWF" in Transport.svelte. */
  ews_history: boolean;
  /** Panel "Verkehr" (Durchsagen), Button "TA" in Transport.svelte. */
  traffic: boolean;
}

export interface Settings {
  version: number;
  language: string | null;
  device: string;
  last_channel: string | null;
  last_service: [number, number] | null;
  volume_percent: number;
  agc: boolean;
  gain_by_channel: Record<string, Record<string, Gain>>;
  ppm: number;
  timeshift_capacity_s: number;
  ews_enabled: boolean;
  ews_autoswitch: boolean;
  /** Bei Verkehrsdurchsagen (FIG 0/19) auf den Durchsage-Dienst umschalten, danach zurueck. */
  traffic_autoswitch: boolean;
  record_pre_s: number;
  record_post_s: number;
  /** Musik-Trennung insgesamt an/aus; aus loescht die Vorschlagsliste. */
  music_enabled: boolean;
  music_auto_save: boolean;
  music_keep_aac: boolean;
  music_mp3_kbps: number;
  audio_device: number | null;
  /** EPG-Paketdienst im Kern mitlaufen lassen (set_epg). */
  epg_enabled: boolean;
  autostart: boolean;
  last_file: string | null;
  file_loop: boolean;
  rtlsdr_index: number;
  panels: Panels;
  /** Aufnahmeordner (null = data/recordings) und Warnton im Alarmfenster (lib/timers.ts). */
  recording_dir: string | null;
  alarm_beep: boolean;
  /** TII/Debug-Panel (lib/debug.ts): Heimatkoordinaten, Detektor, DX-Protokoll, Scope-Rate 1..10. */
  home_lat: number | null;
  home_lon: number | null;
  tii_enabled: boolean;
  tii_threshold: number;
  tii_dx_mode: boolean;
  scope_rate_hz: number;
  /** Fenstergroesse beim letzten Beenden (nur Tauri-Backend, rein-/rausgeschrieben). */
  window_width: number | null;
  window_height: number | null;
}

export interface StoreResult {
  stored: boolean;
  previous: Preset | null;
}

// Ereignisse: bewusst als offener Typ, damit neue Kern-Ereignisse nicht die
// Oberflaeche brechen; der Store prueft `type`.
export interface CoreEvent {
  type: string;
  [key: string]: unknown;
}

export type PresetStatus = "tuning" | "selected" | "not_found";

export type AppEvent =
  | { type: "preset_status"; slot: number | null; status: PresetStatus; name: string; channel: string }
  | { type: "presets_changed"; presets: Presets }
  | { type: "settings_changed"; settings: Settings }
  | { type: "core_restarted"; reason: string; attempt: number }
  | { type: "notice"; level: "info" | "warn" | "error"; text: string }
  | { type: "ews_locations"; iid: number; sub_ch: number; location_info: EwsLocationInfo[] }
  | { type: "ews_history"; history: EwsHistoryEntry[] }
  | { type: "traffic"; active: TrafficEntry | null; history: TrafficEntry[]; supported: boolean }
  | EpgAppEvent
  | TimerAppEvent
  | DebugAppEvent
  | StationsAppEvent
  | TimeshiftAppEvent
  | MusicAppEvent;

// ---------------------------------------------------------------------------
// Schnittstelle
// ---------------------------------------------------------------------------

/** Quittierung eines Alarms, zwischen Haupt- und Alarmfenster ausgetauscht
 * (Review 2026-09-16 Befund 2): die Rust-Seite merkt sich `dismissed` nur
 * still, darum sagen sich die beiden Fenster selbst Bescheid. */
export interface AlarmDismissed {
  iid: number;
  sub_ch: number;
}
const ALARM_DISMISSED_EVENT = "dab://alarm-dismissed";

export interface Transport {
  /// Rohes Kern-Kommando (laeuft ueber die App-Schicht, die Kanal/Geraet mitfuehrt).
  send(cmd: Command): Promise<void>;
  onEvent(handler: (ev: CoreEvent) => void): Promise<() => void>;
  onAppEvent(handler: (ev: AppEvent) => void): Promise<() => void>;
  /// Quittierung an alle Fenster melden bzw. empfangen (Frontend-zu-Frontend).
  alarmDismissed(ev: AlarmDismissed): Promise<void>;
  onAlarmDismissed(handler: (ev: AlarmDismissed) => void): Promise<() => void>;
  alive(): Promise<boolean>;
  getState(): Promise<AppState>;

  getSettings(): Promise<Settings>;
  updateSettings(settings: Settings): Promise<void>;
  getPresets(): Promise<Presets>;
  dataDir(): Promise<[string, boolean]>;

  openDevice(source: SourceKind): Promise<void>;
  setChannel(channel: string): Promise<void>;
  selectService(sid: number, scids: number): Promise<void>;
  stepService(delta: number): Promise<void>;
  setVolume(percent: number): Promise<void>;
  setMute(muted: boolean): Promise<void>;
  setGain(gain: Gain): Promise<void>;
  startScan(channels?: string[]): Promise<void>;
  stopScan(): Promise<void>;
  ewsDismiss(): Promise<void>;
  restartCore(): Promise<void>;

  presetRecall(slot: number): Promise<void>;
  /// `service` ohne channel/eid: Dienst des aktuellen Ensembles; mit beiden: Eintrag der Senderliste (lib/stations.ts).
  presetStore(slot: number, force: boolean, service?: { sid: number; scids: number; channel?: string; eid?: number }): Promise<StoreResult>;
  presetClear(slot: number): Promise<Preset | null>;
  presetsImport(path?: string): Promise<number>;
  favoritesPath(): Promise<string | null>;

  /// Datei-Auswahldialog (null = abgebrochen).
  pickFile(): Promise<string | null>;
  /// Ordner-Auswahldialog, z. B. Aufnahmeordner (null = abgebrochen).
  pickDirectory(start?: string | null): Promise<string | null>;

  // Timer / Aufnahme / Sleep / Alarmfenster (lib/timers.ts, lib/recording.ts)
  timersList(): Promise<Timers>;
  timerAdd(timer: Timer, force?: boolean): Promise<AddOutcome>;
  timerUpdate(timer: Timer, force?: boolean): Promise<AddOutcome>;
  timerDelete(id: number): Promise<void>;
  timerToggleActive(id: number): Promise<void>;
  /// Vertrag mit dem EPG-Panel: Timer-Id oder Fehler/i18n-Schluessel `timer.conflict.*`.
  timerAddFromEpg(req: EpgTimerRequest): Promise<number>;
  recordingStart(): Promise<void>;
  recordingStop(): Promise<void>;
  recordingToggle(): Promise<void>;
  recordingStatus(): Promise<RecordingInfo>;
  sleepSet(minutes: number, action: SleepAction): Promise<void>;
  sleepCancel(): Promise<void>;
  sleepStatus(): Promise<SleepState | null>;
  alarmClose(): Promise<void>;
}

/** Umschaltsperre (lib/recording.ts): wird bei "recording active" gefragt und darf die Aktion wiederholen. */
export type RecordingGuard = (retry: () => Promise<void>) => Promise<void>;
let recordingGuard: RecordingGuard | null = null;
export function setRecordingGuard(g: RecordingGuard | null) {
  recordingGuard = g;
}
export async function guarded(run: () => Promise<void>): Promise<void> {
  try {
    await run();
  } catch (e) {
    if (recordingGuard && String(e) === "recording active") return recordingGuard(run);
    throw e;
  }
}

class TauriTransport implements Transport {
  send(cmd: Command) {
    return guarded(() => invoke<void>("core_send", { command: cmd }));
  }
  async onEvent(handler: (ev: CoreEvent) => void) {
    return listen<CoreEvent>("dab://event", (e) => handler(e.payload));
  }
  async onAppEvent(handler: (ev: AppEvent) => void) {
    return listen<AppEvent>("dab://app", (e) => handler(e.payload));
  }
  alarmDismissed(ev: AlarmDismissed) {
    return emit(ALARM_DISMISSED_EVENT, ev);
  }
  async onAlarmDismissed(handler: (ev: AlarmDismissed) => void) {
    return listen<AlarmDismissed>(ALARM_DISMISSED_EVENT, (e) => handler(e.payload));
  }
  alive() {
    return invoke<boolean>("core_alive");
  }
  getState() {
    return invoke<AppState>("get_state");
  }
  getSettings() {
    return invoke<Settings>("get_settings");
  }
  updateSettings(settings: Settings) {
    return invoke<void>("update_settings", { settings });
  }
  getPresets() {
    return invoke<Presets>("get_presets");
  }
  dataDir() {
    return invoke<[string, boolean]>("data_dir");
  }
  openDevice(source: SourceKind) {
    return invoke<void>("open_device", { source });
  }
  setChannel(channel: string) {
    return this.send({ type: "set_channel", channel });
  }
  selectService(sid: number, scids: number) {
    return this.send({ type: "select_service", sid, scids, slot: "primary" });
  }
  stepService(delta: number) {
    return guarded(() => invoke<void>("step_service", { delta }));
  }
  setVolume(percent: number) {
    return this.send({ type: "set_volume", percent: Math.round(percent) });
  }
  setMute(muted: boolean) {
    return this.send({ type: "set_mute", muted });
  }
  setGain(gain: Gain) {
    return invoke<void>("set_gain", { gain });
  }
  startScan(channels: string[] = []) {
    return this.send({ type: "start_scan", channels, mode: "single" });
  }
  stopScan() {
    return this.send({ type: "stop_scan" });
  }
  ewsDismiss() {
    return this.send({ type: "ews_dismiss" });
  }
  restartCore() {
    return invoke<void>("restart_core");
  }
  presetRecall(slot: number) {
    return guarded(() => invoke<void>("preset_recall", { slot }));
  }
  presetStore(slot: number, force: boolean, service?: { sid: number; scids: number; channel?: string; eid?: number }) {
    return invoke<StoreResult>("preset_store", {
      slot,
      force,
      sid: service?.sid ?? null,
      scids: service?.scids ?? null,
      channel: service?.channel ?? null,
      eid: service?.eid ?? null,
    });
  }
  presetClear(slot: number) {
    return invoke<Preset | null>("preset_clear", { slot });
  }
  presetsImport(path?: string) {
    return invoke<number>("presets_import", { path: path ?? null });
  }
  favoritesPath() {
    return invoke<string | null>("favorites_path");
  }
  timersList() {
    return invoke<Timers>("timers_list");
  }
  timerAdd(timer: Timer, force = false) {
    return invoke<AddOutcome>("timer_add", { timer, force });
  }
  timerUpdate(timer: Timer, force = false) {
    return invoke<AddOutcome>("timer_update", { timer, force });
  }
  timerDelete(id: number) {
    return invoke<void>("timer_delete", { id });
  }
  timerToggleActive(id: number) {
    return invoke<void>("timer_toggle_active", { id });
  }
  timerAddFromEpg(req: EpgTimerRequest) {
    return invoke<number>("timer_add_from_epg", { req });
  }
  recordingStart() {
    return invoke<void>("recording_start");
  }
  recordingStop() {
    return invoke<void>("recording_stop");
  }
  recordingToggle() {
    return invoke<void>("recording_toggle");
  }
  recordingStatus() {
    return invoke<RecordingInfo>("recording_status");
  }
  sleepSet(minutes: number, action: SleepAction) {
    return invoke<void>("sleep_set", { minutes, action });
  }
  sleepCancel() {
    return invoke<void>("sleep_cancel");
  }
  sleepStatus() {
    return invoke<SleepState | null>("sleep_status");
  }
  alarmClose() {
    return invoke<void>("alarm_close");
  }
  async pickFile() {
    const r = await openDialog({
      multiple: false,
      directory: false,
      filters: [
        { name: "DAB IQ", extensions: ["uff", "iq", "raw", "sdr"] },
        { name: "*", extensions: ["*"] },
      ],
    });
    return typeof r === "string" ? r : null;
  }
  async pickDirectory(start?: string | null) {
    const r = await openDialog({ multiple: false, directory: true, defaultPath: start ?? undefined });
    return typeof r === "string" ? r : null;
  }
}

export const api: Transport = new TauriTransport();

/** Band-III-Kanaele (EN 300 401), Reihenfolge des Scans. */
export const BAND_III = [
  "5A", "5B", "5C", "5D", "6A", "6B", "6C", "6D", "7A", "7B", "7C", "7D", "8A", "8B", "8C", "8D",
  "9A", "9B", "9C", "9D", "10A", "10B", "10C", "10D", "11A", "11B", "11C", "11D", "12A", "12B", "12C", "12D",
  "13A", "13B", "13C", "13D", "13E", "13F",
];

const FREQ_KHZ: Record<string, number> = {
  "5A": 174928, "5B": 176640, "5C": 178352, "5D": 180064, "6A": 181936, "6B": 183648, "6C": 185360, "6D": 187072,
  "7A": 188928, "7B": 190640, "7C": 192352, "7D": 194064, "8A": 195936, "8B": 197648, "8C": 199360, "8D": 201072,
  "9A": 202928, "9B": 204640, "9C": 206352, "9D": 208064, "10A": 209936, "10B": 211648, "10C": 213360, "10D": 215072,
  "11A": 216928, "11B": 218640, "11C": 220352, "11D": 222064, "12A": 223936, "12B": 225648, "12C": 227360, "12D": 229072,
  "13A": 230784, "13B": 232496, "13C": 234208, "13D": 235776, "13E": 237488, "13F": 239200,
};

export function channelMhz(ch: string | null): string {
  const k = ch ? FREQ_KHZ[ch.toUpperCase()] : undefined;
  return k ? (k / 1000).toFixed(3) : "---.---";
}
