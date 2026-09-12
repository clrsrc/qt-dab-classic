// Preset-Bedienablaeufe (Belegen mit Nachfrage, Aufruf, Loeschen, Import),
// gemeinsam fuer Leiste, Kontextmenue, Langdruck, Drag und Tastatur.
// Drag-Quelle sind der Ensemble-Tab (ServiceList) und die Senderliste
// (StationList); beide nutzen `startServiceDrag`, die Speicherleiste
// `parseDragPayload`.

import { api } from "./core";
import { confirm } from "./dialogs.svelte";
import { t, tError } from "./i18n.svelte";
import { currentService, notify, s, ui } from "./state.svelte";

/** Dienst fuer Belegen/Drag: ohne `channel`/`eid` = Dienst des aktuellen Ensembles,
 *  mit beiden = Eintrag der Senderliste (auch aus einem anderen Ensemble). */
export interface DragService {
  sid: number;
  scids: number;
  name: string;
  channel?: string;
  eid?: number;
}

const DRAG_SERVICE = "dab-service:";
const DRAG_STATION = "dab-station:";

export function dragPayload(svc: DragService): string {
  const name = svc.name.trim();
  return svc.channel && svc.eid != null
    ? `${DRAG_STATION}${svc.channel}:${svc.eid}:${svc.sid}:${svc.scids}:${name}`
    : `${DRAG_SERVICE}${svc.sid}:${svc.scids}:${name}`;
}

export function parseDragPayload(raw: string): DragService | null {
  let m = raw.match(/^dab-station:([0-9]+[A-Z]):(\d+):(\d+):(\d+):(.*)$/);
  if (m) return { channel: m[1], eid: Number(m[2]), sid: Number(m[3]), scids: Number(m[4]), name: m[5] };
  m = raw.match(/^dab-service:(\d+):(\d+):(.*)$/);
  if (m) return { sid: Number(m[1]), scids: Number(m[2]), name: m[3] };
  return null;
}

/** `ondragstart` einer Dienstzeile. */
export function startServiceDrag(e: DragEvent, svc: DragService) {
  e.dataTransfer?.setData("text/plain", dragPayload(svc));
  if (e.dataTransfer) e.dataTransfer.effectAllowed = "copy";
}

export async function recallPreset(slot: number) {
  try {
    await api.presetRecall(slot);
  } catch (e) {
    notify("warn", tError(e));
  }
}

/** Belegen: bei belegtem Slot erst nachfragen. `service` = Drag/Kontextmenue aus einer Liste. */
export async function storePreset(slot: number, service?: DragService) {
  const svc = service ?? currentService();
  if (!svc || (!service?.channel && !s.channel)) {
    notify("warn", t("preset.no_service"));
    return;
  }
  try {
    const r = await api.presetStore(slot, false, service);
    if (r.stored) {
      notify("info", t("preset.stored", { n: slot + 1, name: svc.name.trim() }));
      return;
    }
    const prev = r.previous ?? ui.presets.slots[slot];
    const ok = await confirm(
      t("preset.overwrite_title", { n: slot + 1 }),
      t("preset.overwrite_text", { old: prev?.name ?? "?", channel: prev?.channel ?? "?", new: svc.name.trim() }),
      t("modal.yes"),
      t("modal.cancel"),
    );
    if (ok) {
      await api.presetStore(slot, true, service);
      notify("info", t("preset.stored", { n: slot + 1, name: svc.name.trim() }));
    }
  } catch (e) {
    notify("warn", tError(e));
  }
}

export async function clearPreset(slot: number) {
  try {
    await api.presetClear(slot);
    notify("info", t("preset.cleared", { n: slot + 1 }));
  } catch (e) {
    notify("warn", tError(e));
  }
}

export async function importFavorites() {
  try {
    const n = await api.presetsImport();
    notify("info", t("preset.import_done", { n }));
  } catch (e) {
    notify("warn", String(e).includes("not found") ? t("preset.import_none") : tError(e));
  }
}
