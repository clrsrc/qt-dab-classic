// DAB Classic – Transportschicht zum Kern und zur App-Schicht (Rust).
//
// Design-Ziel Web-Remote (Entscheidung 23): Komponenten rufen NIE direkt
// Tauri auf, sondern nur diese Schnittstelle. Heute gibt es die
// Tauri-Implementierung (invoke + listen); ein WebSocket-Transport fuer den
// Browser kann spaeter dieselbe Schnittstelle bedienen.

import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { open as openDialog } from "@tauri-apps/plugin-dialog";

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
  iid: number;
  locations: string[];
  is_test: boolean;
  dismissed: boolean;
}

export interface Preset {
  channel: string;
  eid: number;
  sid: number;
  scids: number;
  name: string;
  logo_path: string | null;
  stored_at: number;
}

export interface Presets {
  version: number;
  slots: (Preset | null)[];
}

export interface Panels {
  presets: boolean;
  services: boolean;
  settings: boolean;
  scan: boolean;
  epg: boolean;
  timer: boolean;
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
  record_pre_s: number;
  record_post_s: number;
  music_auto_save: boolean;
  music_keep_aac: boolean;
  music_mp3_kbps: number;
  audio_device: number | null;
  debug_panel_open: boolean;
  autostart: boolean;
  last_file: string | null;
  file_loop: boolean;
  rtlsdr_index: number;
  panels: Panels;
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
  | { type: "notice"; level: "info" | "warn" | "error"; text: string };

// ---------------------------------------------------------------------------
// Schnittstelle
// ---------------------------------------------------------------------------

export interface Transport {
  /// Rohes Kern-Kommando (laeuft ueber die App-Schicht, die Kanal/Geraet mitfuehrt).
  send(cmd: Command): Promise<void>;
  onEvent(handler: (ev: CoreEvent) => void): Promise<() => void>;
  onAppEvent(handler: (ev: AppEvent) => void): Promise<() => void>;
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
  presetStore(slot: number, force: boolean, service?: { sid: number; scids: number }): Promise<StoreResult>;
  presetClear(slot: number): Promise<Preset | null>;
  presetsImport(path?: string): Promise<number>;
  favoritesPath(): Promise<string | null>;

  /// Datei-Auswahldialog (null = abgebrochen).
  pickFile(): Promise<string | null>;
}

class TauriTransport implements Transport {
  send(cmd: Command) {
    return invoke<void>("core_send", { command: cmd });
  }
  async onEvent(handler: (ev: CoreEvent) => void) {
    return listen<CoreEvent>("dab://event", (e) => handler(e.payload));
  }
  async onAppEvent(handler: (ev: AppEvent) => void) {
    return listen<AppEvent>("dab://app", (e) => handler(e.payload));
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
    return invoke<void>("step_service", { delta });
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
    return invoke<void>("preset_recall", { slot });
  }
  presetStore(slot: number, force: boolean, service?: { sid: number; scids: number }) {
    return invoke<StoreResult>("preset_store", { slot, force, sid: service?.sid ?? null, scids: service?.scids ?? null });
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
