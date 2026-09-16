// Wiedergabe von Mitschnitten (Durchsagen, Notfallwarnungen, Aufnahmen) im
// Frontend: der Kern spielt nur den Live-/Timeshift-Strom, Dateien laufen
// ueber ein <audio>-Element (Player.svelte). Die Bytes holt das Tauri-
// Kommando `recording_bytes` (nur Dateien im Aufnahme-/Datenordner) - so
// braucht es weder Asset-Protokoll noch Pfad-Scopes.
import { invoke } from "@tauri-apps/api/core";
import { notify } from "./state.svelte";
import { tError } from "./i18n.svelte";

export interface Playback {
  path: string | null;
  url: string | null;
  label: string;
  loading: boolean;
}

export const playback = $state<Playback>({ path: null, url: null, label: "", loading: false });

function baseName(path: string): string {
  const i = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
  return i >= 0 ? path.slice(i + 1) : path;
}

export async function playRecording(path: string, label?: string): Promise<void> {
  if (playback.loading) return;
  playback.loading = true;
  try {
    const bytes = await invoke<ArrayBuffer | Uint8Array>("recording_bytes", { path });
    const mime = path.toLowerCase().endsWith(".wav") ? "audio/wav" : "audio/mpeg";
    const blob = new Blob([bytes as BlobPart], { type: mime });
    stopPlayback();
    playback.url = URL.createObjectURL(blob);
    playback.path = path;
    playback.label = label || baseName(path);
  } catch (e) {
    notify("warn", tError(e));
  } finally {
    playback.loading = false;
  }
}

export function stopPlayback(): void {
  if (playback.url) {
    try {
      URL.revokeObjectURL(playback.url);
    } catch {
      /* egal */
    }
  }
  playback.url = null;
  playback.path = null;
  playback.label = "";
}
