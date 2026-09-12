// EPG: Typen, Transport (Tauri-Kommandos aus src-tauri/src/epg_cmds.rs) und
// Hilfsfunktionen fuer die Darstellung. Komponenten rufen nur `epgApi`;
// Cache und Parser liegen in Rust (dab-app::epg, Entscheidung 14).

import { invoke } from "@tauri-apps/api/core";

// ---------------------------------------------------------------------------
// Typen (spiegeln dab-app::epg)
// ---------------------------------------------------------------------------

export interface Programme {
  sid: number;
  /** Ortszeit "yyyy-mm-ddTHH:MM:SS" (chrono NaiveDateTime). */
  start_local: string;
  start_unix: number;
  duration_min: number;
  medium_name: string;
  long_name: string;
  short_desc: string;
  long_desc: string;
  genres: string[];
  /** Datei aus dem alten Kern (Zeit = UTC + 2 min), unveraendert angezeigt. */
  legacy_time: boolean;
}

export interface ProgrammeBrief {
  title: string;
  start_unix: number;
  end_unix: number;
  duration_min: number;
  legacy_time: boolean;
}

export interface NowNext {
  sid: number;
  now: ProgrammeBrief | null;
  next: ProgrammeBrief | null;
}

export interface EpgService {
  sid: number;
  name: string;
}

/** Vertrag mit dem Timer-Modul (Kommando `timer_add_from_epg`). */
export interface TimerFromEpgRequest {
  channel: string;
  eid: number;
  sid: number;
  service: string;
  title: string;
  start_unix: number;
  duration_s: number;
  kind: "record" | "switch";
}

export type EpgAppEvent =
  | { type: "epg_updated"; eid: number; sid: number; day: number }
  | { type: "logo_updated"; eid: number; sid: number }
  | { type: "current_media"; logo_data_url: string | null; now_next: NowNext | null };

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

export interface EpgTransport {
  days(eid: number): Promise<number[]>;
  services(eid: number, day: number): Promise<EpgService[]>;
  programmes(eid: number, sid: number, day: number): Promise<Programme[]>;
  nowNext(sid: number): Promise<NowNext | null>;
  /** Timer des anderen Moduls anlegen; Err-String kommt als Exception. */
  timerAddFromEpg(req: TimerFromEpgRequest): Promise<number>;
}

class TauriEpgTransport implements EpgTransport {
  days(eid: number) {
    return invoke<number[]>("epg_days", { eid });
  }
  services(eid: number, day: number) {
    return invoke<EpgService[]>("epg_services", { eid, day });
  }
  programmes(eid: number, sid: number, day: number) {
    return invoke<Programme[]>("epg_programmes", { eid, sid, day });
  }
  nowNext(sid: number) {
    return invoke<NowNext | null>("epg_now_next", { sid });
  }
  timerAddFromEpg(req: TimerFromEpgRequest) {
    return invoke<number>("timer_add_from_epg", { req });
  }
}

export const epgApi: EpgTransport = new TauriEpgTransport();

// ---------------------------------------------------------------------------
// Aenderungsereignisse (aus state.svelte.ts weitergereicht)
// ---------------------------------------------------------------------------

type Listener = (ev: EpgAppEvent) => void;
const listeners = new Set<Listener>();

export function onEpgEvent(cb: Listener): () => void {
  listeners.add(cb);
  return () => listeners.delete(cb);
}

export function emitEpgEvent(ev: EpgAppEvent) {
  for (const l of listeners) l(ev);
}

// ---------------------------------------------------------------------------
// Darstellung
// ---------------------------------------------------------------------------

export function dayOf(d: Date): number {
  return d.getFullYear() * 10000 + (d.getMonth() + 1) * 100 + d.getDate();
}

export function dateOfDay(day: number): Date {
  return new Date(Math.floor(day / 10000), Math.floor(day / 100) % 100 - 1, day % 100);
}

/** Heute bis +5 Tage (Entscheidung: nur diese werden angeboten). */
export function upcomingDays(count = 6): number[] {
  const out: number[] = [];
  const d = new Date();
  for (let i = 0; i < count; i++) {
    out.push(dayOf(new Date(d.getFullYear(), d.getMonth(), d.getDate() + i)));
  }
  return out;
}

export function fmtClock(unix: number): string {
  const d = new Date(unix * 1000);
  return `${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}`;
}

export function fmtDuration(min: number): string {
  if (min < 60) return `${min}′`;
  const h = Math.floor(min / 60);
  const m = min % 60;
  return m ? `${h}h${String(m).padStart(2, "0")}` : `${h}h`;
}

export function fmtDayShort(day: number, lang: string): string {
  const d = dateOfDay(day);
  const wd = d.toLocaleDateString(lang === "de" ? "de-DE" : "en-GB", { weekday: "short" });
  return `${wd} ${String(d.getDate()).padStart(2, "0")}.${String(d.getMonth() + 1).padStart(2, "0")}.`;
}

/** Fortschritt 0..1 einer Sendung zu `nowMs`, null wenn nicht laufend. */
export function progressOf(startUnix: number, durationMin: number, nowMs: number): number | null {
  const start = startUnix * 1000;
  const end = start + durationMin * 60000;
  if (nowMs < start || nowMs >= end) return null;
  return (nowMs - start) / (end - start);
}

export function remainingMin(startUnix: number, durationMin: number, nowMs: number): number {
  const end = startUnix * 1000 + durationMin * 60000;
  return Math.max(0, Math.ceil((end - nowMs) / 60000));
}
