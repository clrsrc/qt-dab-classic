// Timer, Aufnahme, Sleep: Typen (Spiegel von dab-app timer.rs / recording.rs /
// sleep.rs), Formatierung und der Anlege-Ablauf mit Konfliktdialog (v1
// unified-timer-widget: "Bestehenden Timer ersetzen?").

import { api } from "./core";
import { confirm } from "./dialogs.svelte";
import { t, tError } from "./i18n.svelte";
import { notify } from "./state.svelte";

export type TimerKind = "manual_switch" | "manual_record" | "epg_switch" | "epg_record";

export interface Timer {
  id: number;
  type: TimerKind;
  active: boolean;
  fired: boolean;
  channel: string;
  eid: number;
  sid: number;
  scids: number;
  service: string;
  title: string;
  /** Unix-Sekunden (UTC). */
  start_unix: number;
  /** Sekunden; 0 = nur umschalten bzw. offenes Ende. */
  duration_s: number;
}

export interface Timers {
  version: number;
  id_counter: number;
  timers: Timer[];
}

export type Conflict = { reason: "past" } | { reason: "overlap"; other: Timer } | { reason: "recording_active" };

export interface AddOutcome {
  id: number | null;
  conflict: Conflict | null;
}

/** Vertrag mit dem EPG-Panel (`timer_add_from_epg`). */
export interface EpgTimerRequest {
  channel: string;
  eid: number;
  sid: number;
  service: string;
  title: string;
  start_unix: number;
  duration_s: number;
  kind: "record" | "switch";
}

export type TimerFireStatus = "switched" | "recording_started" | "recording_stopped" | "blocked" | "missed" | "failed";

export interface RecordingInfo {
  active: boolean;
  path: string | null;
  bytes: number;
  seconds: number;
  sid: number;
  service: string;
  title: string;
  started_at: number;
  timer_id: number | null;
  stop_at: number | null;
}

export type SleepAction = "mute" | "quit";

export interface SleepState {
  minutes: number;
  action: SleepAction;
  set_at: number;
  until_unix: number;
}

export type TimerAppEvent =
  | { type: "timers_changed"; timers: Timers }
  | { type: "timer_status"; id: number; kind: TimerKind; service: string; title: string; status: TimerFireStatus }
  | { type: "recording_changed"; recording: RecordingInfo }
  | { type: "sleep_changed"; sleep: SleepState | null }
  | { type: "sleep_elapsed"; action: SleepAction };

export const TIMER_KINDS: TimerKind[] = ["manual_switch", "manual_record", "epg_switch", "epg_record"];

export function isRecordKind(k: TimerKind): boolean {
  return k === "manual_record" || k === "epg_record";
}

export function timerLabel(tm: Timer): string {
  return tm.title.trim() || tm.service.trim();
}

export function timerStatusKey(tm: Timer): string {
  if (tm.fired && tm.active) return "timer.status.running";
  if (tm.active) return "timer.status.waiting";
  if (tm.fired) return "timer.status.done";
  return "timer.status.inactive";
}

const pad = (n: number) => String(n).padStart(2, "0");

/** "dd.MM.yyyy HH:mm" (v1-Format) in lokaler Zeit. */
export function fmtDateTime(unix: number): string {
  const d = new Date(unix * 1000);
  return `${pad(d.getDate())}.${pad(d.getMonth() + 1)}.${d.getFullYear()} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

/** "dd.MM. HH:mm" (v1-Konfliktdialog). */
export function fmtShort(unix: number): string {
  const d = new Date(unix * 1000);
  return `${pad(d.getDate())}.${pad(d.getMonth() + 1)}. ${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

/** Dauer in Minuten ("45 min") oder "-" wie in v1. */
export function fmtDuration(seconds: number): string {
  return seconds > 0 ? `${Math.round(seconds / 60)} min` : "-";
}

/** mm:ss bzw. h:mm:ss fuer Laufzeiten/Restzeiten. */
export function fmtClock(seconds: number): string {
  const s = Math.max(0, Math.floor(seconds));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  return h > 0 ? `${h}:${pad(m)}:${pad(sec)}` : `${m}:${pad(sec)}`;
}

export function fmtBytes(b: number): string {
  if (b >= 1024 * 1024) return `${(b / 1024 / 1024).toFixed(1)} MB`;
  if (b >= 1024) return `${Math.round(b / 1024)} kB`;
  return `${b} B`;
}

/** Unix -> Wert fuer <input type="datetime-local"> (lokal, Minutenaufloesung). */
export function toLocalInput(unix: number): string {
  const d = new Date(unix * 1000);
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

/** <input type="datetime-local"> -> Unix (lokal interpretiert); NaN bei leer. */
export function fromLocalInput(v: string): number {
  const ms = new Date(v).getTime();
  return Number.isFinite(ms) ? Math.floor(ms / 1000) : NaN;
}

/** Leerer Timer fuer den Editor: Start in einer Stunde (v1), volle Minute. */
export function newTimer(kind: TimerKind = "manual_record"): Timer {
  const start = Math.floor(Date.now() / 60000) * 60 + 3600;
  return { id: 0, type: kind, active: true, fired: false, channel: "", eid: 0, sid: 0, scids: 0, service: "", title: "", start_unix: start, duration_s: 0 };
}

/**
 * Timer anlegen oder aendern. Bei Ueberschneidung fragt der Dialog wie v1
 * "Bestehenden Timer ersetzen?" und loescht ihn; bei laufender Aufnahme
 * "Trotzdem anlegen". Liefert true, wenn gespeichert.
 */
export async function saveTimer(timer: Timer, isNew: boolean): Promise<boolean> {
  const call = (force: boolean) => (isNew ? api.timerAdd(timer, force) : api.timerUpdate(timer, force));
  try {
    for (let attempt = 0; attempt < 5; attempt++) {
      const r = await call(false);
      if (r.id != null) {
        notify("info", t("timer.saved"));
        return true;
      }
      const c = r.conflict;
      if (!c) return false;
      if (c.reason === "past") {
        notify("warn", t("timer.conflict.past"));
        return false;
      }
      if (c.reason === "recording_active") {
        const ok = await confirm(t("timer.conflict.title"), t("timer.conflict.recording"), t("timer.conflict.force"), t("modal.cancel"));
        if (!ok) return false;
        const forced = await call(true);
        if (forced.id != null) {
          notify("info", t("timer.saved"));
          return true;
        }
        return false;
      }
      // overlap: v1-Dialog
      const ok = await confirm(
        t("timer.conflict.title"),
        t("timer.conflict.overlap_text", {
          other: timerLabel(c.other),
          other_time: fmtShort(c.other.start_unix),
          new: timer.title.trim() || timer.service.trim(),
          new_time: fmtShort(timer.start_unix),
        }),
        t("timer.conflict.replace"),
        t("modal.cancel"),
      );
      if (!ok) return false;
      await api.timerDelete(c.other.id);
    }
  } catch (e) {
    notify("warn", tError(e));
  }
  return false;
}

export async function deleteTimer(tm: Timer): Promise<void> {
  const ok = await confirm(t("timer.delete"), t("timer.delete_confirm", { name: timerLabel(tm) }), t("modal.yes"), t("modal.cancel"));
  if (!ok) return;
  try {
    await api.timerDelete(tm.id);
  } catch (e) {
    notify("warn", tError(e));
  }
}

export async function toggleTimer(tm: Timer): Promise<void> {
  try {
    await api.timerToggleActive(tm.id);
  } catch (e) {
    notify("warn", tError(e));
  }
}
