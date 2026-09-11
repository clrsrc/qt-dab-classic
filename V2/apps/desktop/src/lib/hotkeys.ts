// Tastenkuerzel (Entscheidung 21). Leertaste / Pfeil links/rechts (Timeshift,
// M4), Esc (live), R/E/T sind vorerst Platzhalter bzw. Panel-Umschalter.

import { api } from "./core";
import { s, togglePanel } from "./state.svelte";

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
        run(api.setMute(!s.muted));
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
      case "t":
      case "T":
        void togglePanel("timer");
        break;
      case "r":
      case "R":
      case " ":
      case "ArrowLeft":
      case "ArrowRight":
      case "Escape":
        // Aufnahme (M3), Timeshift (M4): noch ohne Funktion
        ev.preventDefault();
        break;
    }
  };
}
