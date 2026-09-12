// Tastenkuerzel (Entscheidung 21): Ziffern = Speicher, Strg+Ziffer = belegen,
// Pfeil hoch/ab = Dienst, M/R/E/T/D, Umschalt+M = Musik-Panel, +/-, und fuer den Timeshift-Puffer
// (lib/timeshift.ts) Leertaste = Pause/Play, Pfeil links/rechts = ∓30 s,
// Esc = live.

import { api } from "./core";
import { dialogs } from "./dialogs.svelte";
import { toggleRecording } from "./recording";
import { s, togglePanel } from "./state.svelte";
import { SKIP_STEP_S, timeshiftApi } from "./timeshift";

export interface HotkeyActions {
  storePreset(slot: number): void;
  recallPreset(slot: number): void;
  error(e: unknown): void;
}

function inEditable(target: EventTarget | null): boolean {
  const el = target as HTMLElement | null;
  if (!el) return false;
  const tag = el.tagName;
  return tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA" || el.isContentEditable;
}

export function makeKeyHandler(actions: HotkeyActions) {
  return (ev: KeyboardEvent) => {
    if (inEditable(ev.target) && !(ev.ctrlKey && /^[0-9]$/.test(ev.key))) return;
    if (ev.altKey || ev.metaKey) return;
    const k = ev.key;
    const run = (p: Promise<unknown>) => p.catch(actions.error);

    if (/^[0-9]$/.test(k)) {
      const slot = k === "0" ? 9 : Number(k) - 1;
      ev.preventDefault();
      if (ev.ctrlKey) actions.storePreset(slot);
      else actions.recallPreset(slot);
      return;
    }
    if (ev.ctrlKey) return;
    switch (k) {
      case "ArrowUp":
        ev.preventDefault();
        run(api.stepService(-1));
        break;
      case "ArrowDown":
        ev.preventDefault();
        run(api.stepService(1));
        break;
      case "m":
      case "M":
        // Umschalt+M: Panel "Musik" (lib/music.ts); M allein bleibt Stumm.
        if (ev.shiftKey) {
          ev.preventDefault();
          void togglePanel("music");
        } else {
          run(api.setMute(!s.muted));
        }
        break;
      case "+":
        run(api.setVolume(Math.min(100, s.volume + 5)));
        break;
      case "-":
        run(api.setVolume(Math.max(0, s.volume - 5)));
        break;
      case "e":
      case "E":
        void togglePanel("epg");
        break;
      case "d":
      case "D":
        // Debug-Panel (lib/debug.ts, Entscheidung 25)
        void togglePanel("debug");
        break;
      case "t":
      case "T":
        void togglePanel("timer");
        break;
      case "r":
      case "R":
        ev.preventDefault();
        void toggleRecording();
        break;
      // Timeshift (lib/timeshift.ts). Die Leertaste darf nicht greifen, wenn
      // ein Knopf den Fokus hat (dort loest sie den Knopf aus) – Eingabefelder
      // hat `inEditable` schon oben abgefangen.
      case " ": {
        const el = ev.target as HTMLElement | null;
        if (el && (el.tagName === "BUTTON" || el.closest?.("button"))) return;
        ev.preventDefault();
        run(timeshiftApi.pauseToggle());
        break;
      }
      case "ArrowLeft":
        ev.preventDefault();
        run(timeshiftApi.skip(-SKIP_STEP_S));
        break;
      case "ArrowRight":
        ev.preventDefault();
        run(timeshiftApi.skip(SKIP_STEP_S));
        break;
      case "Escape":
        // Offene Dialoge/Menues haben Vorrang (lib/dialogs.svelte.ts).
        if (dialogs.confirm || dialogs.menu) return;
        ev.preventDefault();
        run(timeshiftApi.live());
        break;
    }
  };
}
