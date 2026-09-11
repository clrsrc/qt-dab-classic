// Preset-Bedienablaeufe (Belegen mit Nachfrage, Aufruf, Loeschen, Import),
// gemeinsam fuer Leiste, Kontextmenue, Langdruck, Drag und Tastatur.

import { api } from "./core";
import { confirm } from "./dialogs.svelte";
import { t, tError } from "./i18n.svelte";
import { currentService, notify, s, ui } from "./state.svelte";

export async function recallPreset(slot: number) {
  try {
    await api.presetRecall(slot);
  } catch (e) {
    notify("warn", tError(e));
  }
}

/** Belegen: bei belegtem Slot erst nachfragen. `service` = Drag aus der Liste. */
export async function storePreset(slot: number, service?: { sid: number; scids: number; name: string }) {
  const svc = service ?? currentService();
  if (!svc || !s.channel) {
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
