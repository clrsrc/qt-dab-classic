// Aufnahme-Bedienung (REC-Knopf, Taste R) und die Umschaltsperre: Lehnt die
// Rust-Seite eine Aktion mit "recording active" ab, fragt der Dialog
// "Aufnahme beenden und wechseln?" und wiederholt die Aktion danach.

import { api, setRecordingGuard } from "./core";
import { confirm } from "./dialogs.svelte";
import { t, tError } from "./i18n.svelte";
import { notify } from "./state.svelte";

export async function toggleRecording(): Promise<void> {
  try {
    await api.recordingToggle();
  } catch (e) {
    notify("warn", tError(e));
  }
}

export async function startRecording(): Promise<void> {
  try {
    await api.recordingStart();
  } catch (e) {
    notify("warn", tError(e));
  }
}

export async function stopRecording(): Promise<void> {
  try {
    await api.recordingStop();
  } catch (e) {
    notify("warn", tError(e));
  }
}

let asking = false;

/** Rueckfrage bei gesperrtem Umschalten; `retry` = die abgewiesene Aktion. */
export async function recordingGuard(retry: () => Promise<void>): Promise<void> {
  if (asking) return;
  asking = true;
  try {
    let file = "";
    try {
      file = (await api.recordingStatus()).path ?? "";
    } catch {
      // ohne Pfad geht es auch
    }
    const ok = await confirm(
      t("rec.locked_title"),
      t("rec.locked_text", { file: file.split(/[\\/]/).pop() ?? "" }),
      t("rec.stop_and_switch"),
      t("modal.cancel"),
    );
    if (!ok) return;
    await api.recordingStop();
    await retry();
  } finally {
    asking = false;
  }
}

export function installRecordingGuard(): void {
  setRecordingGuard(recordingGuard);
}
