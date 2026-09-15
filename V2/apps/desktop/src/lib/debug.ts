// Debug-Panel (Entscheidung 25): Typen, Transport (Tauri-Kommandos aus
// src-tauri/src/debug_cmds.rs) und der nicht-reaktive Bus fuer die
// Scope-Rohdaten (`spectrum`, `iq_samples` aus dab://event). Die Rohdaten
// gehen bewusst NICHT durch den Svelte-Store: Canvas-Komponenten abonnieren
// `onScopeFrame` und zeichnen per requestAnimationFrame.
// Zaehler/TII-Liste kommen ueber dab://app (`debug_stats`, `tii_updated`)
// und liegen im Store `s.debug` / `s.tii` (state.svelte.ts).

import { invoke } from "@tauri-apps/api/core";
import type { CoreEvent } from "./core";
import { s } from "./state.svelte";

// ---------------------------------------------------------------------------
// Typen (spiegeln dab-app::tii)
// ---------------------------------------------------------------------------

export interface Transmitter {
  name: string;
  lat: number;
  lon: number;
  power_kw: number;
  altitude_m: number;
  height_m: number;
  polarization: string;
  direction: string;
  channel: string;
  ensemble: string;
  country: string;
  eid: number;
  main_id: number;
  sub_id: number;
}

export interface TiiSeen {
  main_id: number;
  sub_id: number;
  strength: number;
  transmitter: Transmitter | null;
  distance_km: number | null;
  azimuth_deg: number | null;
}

export interface ServiceStatsState {
  sid: number;
  frame_errors: number;
  rs_errors: number;
  aac_errors: number;
  rs_corrections: number;
  total_frame_errors: number;
  total_rs_errors: number;
  total_aac_errors: number;
  total_rs_corrections: number;
  seconds: number;
}

export interface DebugState {
  open: boolean;
  scope_rate_hz: number;
  /** SNR je Sekunde, aeltester Wert zuerst (max. 120). */
  snr_history: number[];
  freq_offset_hz: number;
  stats_primary: ServiceStatsState | null;
  stats_background: ServiceStatsState | null;
  tii_db_entries: number;
  tii_db_source: string | null;
}

export function emptyDebugState(): DebugState {
  return { open: false, scope_rate_hz: 5, snr_history: [], freq_offset_hz: 0, stats_primary: null, stats_background: null, tii_db_entries: 0, tii_db_source: null };
}

export type DebugAppEvent = { type: "tii_updated"; tii: TiiSeen[] } | { type: "debug_stats"; debug: DebugState };

/** dab://app-Ereignisse dieses Moduls; liefert false fuer fremde Typen. */
export function applyDebugAppEvent(ev: { type: string }): boolean {
  const e = ev as DebugAppEvent;
  switch (e.type) {
    case "tii_updated":
      s.tii = e.tii;
      return true;
    case "debug_stats":
      s.debug = e.debug;
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

export interface DebugTransport {
  setOpen(open: boolean): Promise<void>;
  setRate(rateHz: number): Promise<void>;
  state(): Promise<DebugState>;
  tiiSet(enabled: boolean, threshold: number, dxMode: boolean): Promise<void>;
  tiiList(): Promise<TiiSeen[]>;
  homeSet(lat: number | null, lon: number | null): Promise<void>;
  /** ASA-"Standort-Code" (z. B. "1253-3513-3668") -> [lat, lon]; wirft bei ungueltiger Pruefsumme/Format. */
  homeCodeDecode(code: string): Promise<[number, number]>;
}

class TauriDebugTransport implements DebugTransport {
  setOpen(open: boolean) {
    return invoke<void>("debug_set_open", { open });
  }
  setRate(rateHz: number) {
    return invoke<void>("debug_set_rate", { rateHz });
  }
  state() {
    return invoke<DebugState>("debug_state");
  }
  tiiSet(enabled: boolean, threshold: number, dxMode: boolean) {
    return invoke<void>("tii_set", { enabled, threshold, dxMode });
  }
  tiiList() {
    return invoke<TiiSeen[]>("tii_list");
  }
  homeSet(lat: number | null, lon: number | null) {
    return invoke<void>("home_set", { lat, lon });
  }
  homeCodeDecode(code: string) {
    return invoke<[number, number]>("home_code_decode", { code });
  }
}

export const debugApi: DebugTransport = new TauriDebugTransport();

// ---------------------------------------------------------------------------
// Scope-Rohdaten (nicht reaktiv)
// ---------------------------------------------------------------------------

export const SPECTRUM_BINS = 2048;
export const IQ_CARRIERS = 1536;

export type ScopeKind = "spectrum" | "iq";
type ScopeListener = (kind: ScopeKind, data: Uint8Array | Int8Array) => void;
const scopeListeners = new Set<ScopeListener>();

/** Zaehler der empfangenen Frames (Beleg: nach dem Schliessen kommen keine mehr). */
export const scopeCounters = { spectrum: 0, iq: 0, lastAt: 0 };
/** Zaehlerstand beim Schliessen des Panels; das Panel zeigt beim Oeffnen die Differenz. */
export const scopeIdle = { closedAt: null as { spectrum: number; iq: number } | null };

export function onScopeFrame(cb: ScopeListener): () => void {
  scopeListeners.add(cb);
  return () => scopeListeners.delete(cb);
}

function b64ToBytes(b64: string): Uint8Array {
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

/** dBFS eines Spektrum-Bins (Protokoll: 0,5 dB je Stufe, 0 = -120 dBFS). */
export function binToDbfs(v: number): number {
  return v / 2 - 120;
}

/** Hook aus state.svelte.ts: Rohdaten-Ereignisse hier abfangen (true = verbraucht). */
export function feedScopeEvent(ev: CoreEvent): boolean {
  if (ev.type === "spectrum") {
    scopeCounters.spectrum++;
    scopeCounters.lastAt = Date.now();
    if (scopeListeners.size) {
      const bins = b64ToBytes(String(ev.bins_b64));
      for (const l of scopeListeners) l("spectrum", bins);
    }
    return true;
  }
  if (ev.type === "iq_samples") {
    scopeCounters.iq++;
    scopeCounters.lastAt = Date.now();
    if (scopeListeners.size) {
      const u8 = b64ToBytes(String(ev.iq_b64));
      const iq = new Int8Array(u8.buffer, u8.byteOffset, u8.length);
      for (const l of scopeListeners) l("iq", iq);
    }
    return true;
  }
  return false;
}

/** Zeichenschleife: `draw` laeuft hoechstens einmal je Bildschirmbild und nur nach neuen Daten. */
export function makeFramePump(draw: () => void): { request(): void; stop(): void } {
  let pending = false;
  let stopped = false;
  return {
    request() {
      if (pending || stopped) return;
      pending = true;
      requestAnimationFrame(() => {
        pending = false;
        if (!stopped) draw();
      });
    },
    stop() {
      stopped = true;
    },
  };
}
