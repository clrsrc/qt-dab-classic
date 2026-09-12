// Reaktiver Spiegel fuer Timer, Aufnahme und Sleep-Timer (dab://app-Ereignisse
// `timers_changed`, `timer_status`, `recording_changed`, `sleep_changed`,
// `sleep_elapsed`; Kern-Ereignis `recording_state`). Wird aus state.svelte.ts
// per Hook gefuellt; Komponenten lesen `tm`.

import { api, type AppEvent } from "./core";
import { t } from "./i18n.svelte";
import { installRecordingGuard } from "./recording";
import { notify } from "./state.svelte";
import { fmtBytes, timerLabel, type RecordingInfo, type SleepState, type Timer, type TimerAppEvent } from "./timers";
import { win } from "./window";

export const tm = $state({
  timers: [] as Timer[],
  recording: null as RecordingInfo | null,
  sleep: null as SleepState | null,
  /** Letzter Aufnahmepfad (fuer den Hinweis nach dem Stopp). */
  lastFile: "" as string,
});

export function recordingActive(): boolean {
  return !!tm.recording?.active;
}

/** Naechster wartender Timer (Statusleiste). */
export function nextTimer(nowMs: number): Timer | null {
  const now = Math.floor(nowMs / 1000);
  let best: Timer | null = null;
  for (const x of tm.timers) {
    if (!x.active || x.fired || x.start_unix < now) continue;
    if (!best || x.start_unix < best.start_unix) best = x;
  }
  return best;
}

export function sleepRemaining(nowMs: number): number {
  return tm.sleep ? Math.max(0, tm.sleep.until_unix - Math.floor(nowMs / 1000)) : 0;
}

function fileName(p: string | null | undefined): string {
  return (p ?? "").split(/[\\/]/).pop() ?? "";
}

/** dab://app-Ereignisse dieses Moduls; liefert false fuer fremde Typen. */
export function applyTimerAppEvent(ev: AppEvent | TimerAppEvent): boolean {
  const e = ev as TimerAppEvent;
  switch (e.type) {
    case "timers_changed":
      tm.timers = e.timers.timers;
      return true;
    case "timer_status": {
      const name = e.title.trim() || e.service.trim();
      const level = e.status === "blocked" || e.status === "missed" || e.status === "failed" ? "warn" : "info";
      notify(level, t(`timer.fired.${e.status}`, { name }), 6000);
      return true;
    }
    case "recording_changed": {
      const was = tm.recording?.active ?? false;
      tm.recording = e.recording;
      if (e.recording.active && !was) {
        tm.lastFile = e.recording.path ?? "";
        notify("info", t("rec.started", { file: fileName(e.recording.path) }), 5000);
      } else if (!e.recording.active && was) {
        notify("info", t("rec.stopped", { file: fileName(e.recording.path ?? tm.lastFile), size: fmtBytes(e.recording.bytes) }), 6000);
      }
      return true;
    }
    case "sleep_changed":
      tm.sleep = e.sleep;
      return true;
    case "sleep_elapsed":
      tm.sleep = null;
      if (e.action === "mute") notify("info", t("sleep.elapsed_mute"), 6000);
      else void win.close();
      return true;
    default:
      return false;
  }
}

/** Kern-Ereignis `recording_state` (Primary): Laufzeit/Bytes fortschreiben. */
export function applyRecordingState(e: Record<string, unknown>): void {
  const active = !!e.active;
  const cur = tm.recording ?? { active: false, path: null, bytes: 0, seconds: 0, sid: 0, service: "", title: "", started_at: 0, timer_id: null, stop_at: null };
  tm.recording = {
    ...cur,
    active,
    path: (e.path as string | null) ?? cur.path,
    bytes: Number(e.bytes ?? cur.bytes),
    seconds: Number(e.seconds ?? cur.seconds),
    sid: Number(e.sid ?? cur.sid),
    timer_id: active ? cur.timer_id : null,
    stop_at: active ? cur.stop_at : null,
  };
}

/** Beim Start: Momentaufnahme holen und die Umschaltsperre-Rueckfrage anmelden. */
export async function initTimers(): Promise<void> {
  installRecordingGuard();
  const [timers, recording, sleep] = await Promise.all([api.timersList(), api.recordingStatus(), api.sleepStatus()]);
  tm.timers = timers.timers;
  tm.recording = recording;
  tm.sleep = sleep;
}

export { timerLabel };
