// Musik-Trennung (Entscheidungen 6, 7): Typen und Transport zu den
// Tauri-Kommandos aus src-tauri/src/music_cmds.rs. Die Vorschlagsliste liegt
// im Store (`s.music_candidates`, Snapshot + App-Ereignis `music_candidates`);
// Erkennung und Schnitt liegen in Rust (dab-app::music) bzw. im Kern.

import { invoke } from "@tauri-apps/api/core";

/** Dauer eines DAB-Logikrahmens (dab-music::FRAME_S). */
export const FRAME_S = 0.024;

/** Spiegelt dab_music::TrackCandidate. */
export interface TrackCandidate {
  /** Schreibzeiger beim Titelbeginn – massgeblich fuer den Schnitt. */
  start_frame: number;
  /** Schreibzeiger beim Titelende; null = laeuft noch. */
  end_frame: number | null;
  /** Nur Anzeige (Rahmenzeit in Sekunden). */
  start_s: number;
  end_s: number | null;
  title: string | null;
  artist: string | null;
  album: string | null;
  genre: string | null;
  station: string | null;
  item_running: boolean;
  /** Aus DLS geraten (Dienst ohne DL+): Titel/Interpret sind unsicher. */
  from_dls: boolean;
  /** Schon uebernommen (Export an den Kern geschickt). */
  taken: boolean;
}

export type MusicAppEvent = { type: "music_candidates"; candidates: TrackCandidate[] };

export interface MusicTransport {
  list(): Promise<TrackCandidate[]>;
  /** "uebernehmen": liefert den Zielpfad der MP3. */
  exportCandidate(index: number): Promise<string>;
  clear(): Promise<void>;
}

class TauriMusicTransport implements MusicTransport {
  list() {
    return invoke<TrackCandidate[]>("music_list");
  }
  exportCandidate(index: number) {
    return invoke<string>("music_export", { candidateIndex: index });
  }
  clear() {
    return invoke<void>("music_clear");
  }
}

export const musicApi: MusicTransport = new TauriMusicTransport();

/** Laenge des Titels in Sekunden (null, solange er laeuft). */
export function durationS(c: TrackCandidate): number | null {
  if (c.end_frame === null) return null;
  return Math.max(0, c.end_frame - c.start_frame) * FRAME_S;
}

/** "vor 3:20": Abstand des Titelbeginns zum jetzigen Schreibzeiger. */
export function ageS(c: TrackCandidate, currentFrame: number): number {
  return Math.max(0, currentFrame - c.start_frame) * FRAME_S;
}

/** "3:20" aus Sekunden (ohne Vorzeichen). */
export function fmtLen(seconds: number | null): string {
  if (seconds === null || !isFinite(seconds)) return "–:––";
  const v = Math.max(0, Math.round(seconds));
  return `${Math.floor(v / 60)}:${String(v % 60).padStart(2, "0")}`;
}

/** "Interpret - Titel" (wie dab_music::TrackCandidate::label). */
export function label(c: TrackCandidate): string {
  if (c.artist && c.title) return `${c.artist} - ${c.title}`;
  return c.title ?? c.artist ?? "";
}
