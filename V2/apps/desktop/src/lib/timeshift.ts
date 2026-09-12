// Timeshift: Typen und Transport (Tauri-Kommandos aus
// src-tauri/src/timeshift_cmds.rs). Der Zustand liegt im Store `s.timeshift`
// (Snapshot + Kern-Ereignis `timeshift_state`, state.svelte.ts); Ring und
// Logik liegen im Kern bzw. in Rust (dab-app::timeshift).

import { invoke } from "@tauri-apps/api/core";

export type TimeshiftMode = "live" | "paused" | "playing";

/** Spiegelt dab-app::timeshift::TimeshiftInfo. */
export interface TimeshiftInfo {
  mode: TimeshiftMode;
  /** Inhalt des Rings in Sekunden. */
  buffered_s: number;
  /** Abstand des Lesezeigers zu live in Sekunden (0 = live). */
  offset_s: number;
  /** Eingestellte Ringgroesse in Sekunden. */
  capacity_s: number;
  /** Ensemble-Uhrzeit am Schreibzeiger (0 = unbekannt). */
  live_unix: number;
  /** Schreibzeiger in Logikrahmen (24 ms); Bezug der Musik-Trennung (lib/music.ts). */
  frame_index: number;
  /** Nur mit DABCLASSIC_TS_DEMO: Leiste ohne Kern-Funktion anzeigen. */
  demo: boolean;
}

/** Hinweis der App-Schicht: der Puffer wurde verworfen (i18n `ts.notice.*`). */
export type TimeshiftNoticeKind = "ews_dropped" | "service_changed";
export type TimeshiftAppEvent = { type: "timeshift_notice"; notice: TimeshiftNoticeKind };

/** Sprungweite je Tastendruck/Knopf (Entscheidung 21). */
export const SKIP_STEP_S = 30;
/** Bereich der Ringkapazitaet in Minuten (Plan M4 1.3: 60 s .. 4 h). */
export const CAPACITY_MIN_MIN = 1;
export const CAPACITY_MAX_MIN = 240;

export function emptyTimeshift(): TimeshiftInfo {
  return { mode: "live", buffered_s: 0, offset_s: 0, capacity_s: 0, live_unix: 0, frame_index: 0, demo: false };
}

export interface TimeshiftTransport {
  pauseToggle(): Promise<void>;
  /** `delta_s > 0` geht Richtung live. */
  skip(deltaS: number): Promise<void>;
  /** `offsetS` = Sekunden hinter live (0 = live). */
  seek(offsetS: number): Promise<void>;
  live(): Promise<void>;
  /** Ausschnitt sichern; liefert den Zielpfad. */
  exportRange(fromS: number, toS: number): Promise<string>;
  status(): Promise<TimeshiftInfo>;
}

class TauriTimeshiftTransport implements TimeshiftTransport {
  pauseToggle() {
    return invoke<void>("timeshift_pause_toggle");
  }
  skip(deltaS: number) {
    return invoke<void>("timeshift_skip", { deltaS });
  }
  seek(offsetS: number) {
    return invoke<void>("timeshift_seek", { offsetS });
  }
  live() {
    return invoke<void>("timeshift_live");
  }
  exportRange(fromS: number, toS: number) {
    return invoke<string>("timeshift_export", { fromS, toS });
  }
  status() {
    return invoke<TimeshiftInfo>("timeshift_status");
  }
}

export const timeshiftApi: TimeshiftTransport = new TauriTimeshiftTransport();

/** "−1:23" bzw. "LIVE"; `seconds` = Versatz hinter live. */
export function fmtOffset(seconds: number): string {
  const v = Math.max(0, Math.round(seconds));
  if (v <= 0) return "LIVE";
  return `−${Math.floor(v / 60)}:${String(v % 60).padStart(2, "0")}`;
}

/** "12:34" fuer Minuten:Sekunden ohne Vorzeichen (Pufferlaenge). */
export function fmtSpan(seconds: number): string {
  const v = Math.max(0, Math.round(seconds));
  return `${Math.floor(v / 60)}:${String(v % 60).padStart(2, "0")}`;
}

/** Uhrzeit am Lesezeiger aus der Ensemble-Uhr (leer, wenn unbekannt). */
export function readerClock(ts: TimeshiftInfo): string {
  if (!ts.live_unix) return "";
  const d = new Date((ts.live_unix - Math.round(ts.offset_s)) * 1000);
  return d.toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit", second: "2-digit" });
}
