// Logos: Abruf als data:-URL aus dem Rust-Cache (dab-app::logos) mit kleinem
// Frontend-Cache je (eid, sid, size); `logo_updated` macht Eintraege ungueltig.

import { invoke } from "@tauri-apps/api/core";
import { onEpgEvent } from "./epg";

export type LogoSize = "large" | "medium" | "small";

export interface LogoTransport {
  dataUrl(eid: number, sid: number, size: LogoSize): Promise<string | null>;
  sizes(eid: number, sid: number): Promise<[number, number][]>;
}

class TauriLogoTransport implements LogoTransport {
  dataUrl(eid: number, sid: number, size: LogoSize) {
    return invoke<string | null>("logo_data_url", { eid, sid, size });
  }
  sizes(eid: number, sid: number) {
    return invoke<[number, number][]>("logo_sizes", { eid, sid });
  }
}

export const logoApi: LogoTransport = new TauriLogoTransport();

const cache = new Map<string, Promise<string | null>>();
const key = (eid: number, sid: number, size: LogoSize) => `${eid}:${sid}:${size}`;

/** Logo holen (gecacht); null = keins vorhanden. */
export function getLogo(eid: number, sid: number, size: LogoSize): Promise<string | null> {
  const k = key(eid, sid, size);
  let p = cache.get(k);
  if (!p) {
    p = logoApi.dataUrl(eid, sid, size).catch(() => null);
    cache.set(k, p);
  }
  return p;
}

export function invalidateLogo(eid: number, sid: number) {
  for (const size of ["large", "medium", "small"] as const) cache.delete(key(eid, sid, size));
}

/** Versionszaehler je Dienst, damit Komponenten neu laden koennen. */
const versions = new Map<string, number>();
const versionListeners = new Set<(eid: number, sid: number) => void>();

export function logoVersion(eid: number, sid: number): number {
  return versions.get(`${eid}:${sid}`) ?? 0;
}

export function onLogoUpdated(cb: (eid: number, sid: number) => void): () => void {
  versionListeners.add(cb);
  return () => versionListeners.delete(cb);
}

onEpgEvent((ev) => {
  if (ev.type === "logo_updated") {
    invalidateLogo(ev.eid, ev.sid);
    versions.set(`${ev.eid}:${ev.sid}`, logoVersion(ev.eid, ev.sid) + 1);
    for (const l of versionListeners) l(ev.eid, ev.sid);
  }
});

/** Kurzbezeichner fuer den Platzhalter (bis 3 Zeichen). */
export function initials(name: string | null | undefined): string {
  const n = (name ?? "").trim();
  if (!n) return "";
  const parts = n.split(/\s+/).filter(Boolean);
  if (parts.length >= 2) return (parts[0][0] + parts[1][0]).toUpperCase();
  return n.slice(0, 3);
}
