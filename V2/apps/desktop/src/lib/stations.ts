// Senderliste ueber alle Ensembles: Typen und Transport (Tauri-Kommandos aus
// src-tauri/src/stations_cmds.rs). Die Liste selbst liegt im Store `s.stations`
// (Snapshot + `stations_changed` aus dab://app, state.svelte.ts); Logik und
// Persistenz (data/stations.json) in Rust (dab-app::stations).

import { invoke } from "@tauri-apps/api/core";
import { guarded } from "./core";

/** Spiegelt dab-app::stations::StationEntry. */
export interface StationEntry {
  channel: string;
  eid: number;
  ensemble: string;
  sid: number;
  scids: number;
  name: string;
  is_audio: boolean;
  bitrate_kbps: number;
  pty: number;
  last_seen_unix: number;
}

export type StationsAppEvent = { type: "stations_changed"; stations: StationEntry[] };

export interface StationsTransport {
  list(includeData?: boolean): Promise<StationEntry[]>;
  /** Umschalten, auch mit Kanalwechsel; Umschaltsperre bei Aufnahme wie bei Presets. */
  tune(st: Pick<StationEntry, "channel" | "eid" | "sid" | "scids">): Promise<void>;
  clear(): Promise<void>;
}

class TauriStationsTransport implements StationsTransport {
  list(includeData = false) {
    return invoke<StationEntry[]>("stations_list", { includeData });
  }
  tune(st: Pick<StationEntry, "channel" | "eid" | "sid" | "scids">) {
    return guarded(() => invoke<void>("station_tune", { channel: st.channel, eid: st.eid, sid: st.sid, scids: st.scids }));
  }
  clear() {
    return invoke<void>("stations_clear");
  }
}

export const stationsApi: StationsTransport = new TauriStationsTransport();

/** Gruppe je Ensemble (Kanal + EId) in Listenreihenfolge. */
export interface StationGroup {
  key: string;
  channel: string;
  eid: number;
  ensemble: string;
  entries: StationEntry[];
}

export function groupStations(list: StationEntry[]): StationGroup[] {
  const groups: StationGroup[] = [];
  let cur: StationGroup | null = null;
  for (const e of list) {
    const key = `${e.channel}:${e.eid}`;
    if (!cur || cur.key !== key) {
      cur = { key, channel: e.channel, eid: e.eid, ensemble: e.ensemble, entries: [] };
      groups.push(cur);
    }
    cur.entries.push(e);
  }
  return groups;
}

export const hexEid = (eid: number) => eid.toString(16).toUpperCase().padStart(4, "0");
