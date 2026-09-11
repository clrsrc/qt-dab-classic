// DAB Classic – Transportschicht zum Kern.
//
// Design-Ziel Web-Remote (Entscheidung 23): Komponenten rufen NIE direkt
// Tauri auf, sondern nur diese Schnittstelle. Heute gibt es die
// Tauri-Implementierung (invoke + listen); ein WebSocket-Transport fuer den
// Browser kann spaeter dieselbe Schnittstelle bedienen.

import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";

// Typen spiegeln crates/dab-api (JSON, extern getaggt mit "type", snake_case).
export type ServiceSlot = "primary" | "background";

export type Command =
  | { type: "open_device"; source: SourceKind }
  | { type: "close_device" }
  | { type: "set_channel"; channel: string }
  | { type: "set_gain"; gain: { lna: number; vga: number; amp: boolean } }
  | { type: "set_agc"; enabled: boolean }
  | { type: "select_service"; sid: number; scids: number; slot: ServiceSlot }
  | { type: "stop_service"; slot: ServiceSlot }
  | { type: "start_scan"; channels: string[]; mode: "single" | "to_data" | "continuous" }
  | { type: "stop_scan" }
  | { type: "set_volume"; percent: number }
  | { type: "set_mute"; muted: boolean }
  | { type: "timeshift_pause" }
  | { type: "timeshift_play" }
  | { type: "timeshift_live" }
  | { type: "timeshift_skip"; delta_s: number }
  | { type: "set_scopes"; spectrum: boolean; iq: boolean; rate_hz: number }
  | { type: "ews_dismiss" }
  | { type: "get_state" }
  | { type: "shutdown" };

export type SourceKind =
  | { kind: "hack_rf"; serial: string | null }
  | { kind: "rtl_sdr"; index: number }
  | { kind: "file"; path: string; loop: boolean };

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

// Ereignisse: bewusst als offener Typ, damit neue Kern-Ereignisse nicht die
// Oberflaeche brechen; die Komponenten pruefen `type`.
export interface CoreEvent {
  type: string;
  [key: string]: unknown;
}

export interface CoreTransport {
  send(cmd: Command): Promise<void>;
  onEvent(handler: (ev: CoreEvent) => void): Promise<() => void>;
  alive(): Promise<boolean>;
}

class TauriTransport implements CoreTransport {
  async send(cmd: Command): Promise<void> {
    await invoke("core_send", { command: cmd });
  }
  async onEvent(handler: (ev: CoreEvent) => void): Promise<() => void> {
    const un: UnlistenFn = await listen<CoreEvent>("dab://event", (e) => handler(e.payload));
    return un;
  }
  async alive(): Promise<boolean> {
    return invoke<boolean>("core_alive");
  }
}

export const core: CoreTransport = new TauriTransport();

// App-Daten (Einstellungen, Presets) laufen ebenfalls ueber die Rust-Seite.
export const app = {
  getSettings: () => invoke<Record<string, unknown>>("get_settings"),
  saveSettings: (settings: Record<string, unknown>) => invoke<void>("save_settings", { settings }),
  getPresets: () => invoke<{ version: number; slots: (Preset | null)[] }>("get_presets"),
  savePresets: (presets: { version: number; slots: (Preset | null)[] }) => invoke<void>("save_presets", { presets }),
  dataDir: () => invoke<[string, boolean]>("data_dir"),
};

export interface Preset {
  channel: string;
  eid: number;
  sid: number;
  scids: number;
  name: string;
  logo_path: string | null;
  stored_at: number;
}
