<script lang="ts">
  // Inhalt des Alarmfensters (Entscheidung 5 + 10): eigenes Tauri-Fenster
  // `alarm` (always on top, rahmenlos), das die Rust-Seite bei ews_alert
  // trigger/sustain oeffnet und bei end schliesst. Zeigt Phase, Stufe (Rohwert),
  // Orte, Testkennzeichen, Warndienst; "Quittieren" schickt ews_dismiss.
  import { onMount } from "svelte";
  import { startBeep, stopBeep } from "$lib/alarm";
  import { api, type EwsLocationInfo } from "$lib/core";
  import { t, tError } from "$lib/i18n.svelte";
  import { dispose, init, notify, s, ui } from "$lib/state.svelte";
  import { compass, fmtDistance } from "$lib/tii";

  onMount(() => {
    init().catch((e) => notify("error", String(e)));
    return () => {
      stopBeep();
      dispose();
    };
  });

  const alert = $derived(s.alert);
  const active = $derived(!!alert && (alert.phase === "trigger" || alert.phase === "sustain"));
  const service = $derived.by(() => {
    const a = s.alert;
    if (!a) return "";
    const svc = s.services.find((x) => x.sub_ch === a.sub_ch);
    return svc?.name.trim() ?? `SubCh ${a.sub_ch}`;
  });
  const from = $derived.by(() => {
    const sid = s.ews_switched_from;
    if (sid == null) return "";
    return s.services.find((x) => x.sid === sid)?.name.trim() ?? sid.toString(16).toUpperCase();
  });
  // Rohes Status-Byte der FIG 0/15 aus dem Kern (Warntag 2026: 0x01 = Last-Bit 0, Stufe 0, IId 1)
  const stageHex = $derived((alert?.stage_raw ?? 0).toString(16).toUpperCase().padStart(2, "0"));

  /** Ortscode lesbar machen (dab_app::ews_location): Entfernung/Richtung von
   * zu Hause, sonst nur die Naeherungskoordinate, sonst der rohe Code. */
  function locationLabel(loc: EwsLocationInfo): string {
    if (loc.distance_km != null && loc.azimuth_deg != null) {
      return `${loc.code} – ${fmtDistance(loc.distance_km)} ${compass(loc.azimuth_deg)}`;
    }
    if (loc.lat != null && loc.lon != null) {
      const ns = loc.lat >= 0 ? "N" : "S";
      const ew = loc.lon >= 0 ? "O" : "W";
      return `${loc.code} – ≈ ${Math.abs(loc.lat).toFixed(1)}°${ns} ${Math.abs(loc.lon).toFixed(1)}°${ew}`;
    }
    return loc.code;
  }
  const beepOn = $derived(active && !alert?.dismissed && (ui.settings?.alarm_beep ?? true));

  $effect(() => {
    if (beepOn) startBeep();
    else stopBeep();
  });
  // Alarmende (oder Quittierung aus dem Hauptfenster): Fenster zu.
  $effect(() => {
    if (ui.ready && (!alert || alert.phase === "end" || alert.dismissed)) void api.alarmClose();
  });

  async function acknowledge() {
    stopBeep();
    try {
      await api.ewsDismiss();
    } catch (e) {
      notify("warn", tError(e));
    }
    await api.alarmClose();
  }
  function onKey(e: KeyboardEvent) {
    if (e.key === "Escape" || e.key === "Enter") void acknowledge();
  }
</script>

<svelte:window onkeydown={onKey} oncontextmenu={(e) => e.preventDefault()} />

<div class="alarm" class:test={alert?.is_test} role="alertdialog" aria-modal="true">
  <div class="bar" data-tauri-drag-region>
    <span class="blink">⚠</span>
    <span class="title" data-tauri-drag-region>{t("alarm.window_title")}{alert?.is_test ? ` – ${t("alarm.test")}` : ""}</span>
    <span class="blink">⚠</span>
  </div>
  {#if alert}
    <div class="body">
      <div class="service">{service}</div>
      <div class="grid">
        <span class="k">{t("alarm.phase")}</span><span>{t(`alarm.phase.${alert.phase}`)}</span>
        <span class="k">{t("alarm.stage_label")}</span><span>{t("alarm.stage_raw", { stage: alert.stage, hex: stageHex })}</span>
        <span class="k">IId</span><span>{alert.iid} · SubCh {alert.sub_ch}</span>
      </div>
      {#if alert.locations.length}
        <div class="locs">
          <div class="k">{t("alarm.locations")}</div>
          {#each alert.location_info.length ? alert.location_info : alert.locations.map((code) => ({ code, lat: null, lon: null, radius_km: null, distance_km: null, azimuth_deg: null }) as EwsLocationInfo) as loc (loc.code)}
            <div class="loc">{locationLabel(loc)}</div>
          {/each}
        </div>
      {/if}
      {#if alert.is_test}<div class="hint test">{t("alarm.test_hint")}</div>{/if}
      {#if from}<div class="hint">{t("alarm.switched_from_name", { name: from })}</div>{:else}<div class="hint">{t("alarm.hint")}</div>{/if}
    </div>
  {:else}
    <div class="body"><div class="hint">{t("alarm.none")}</div></div>
  {/if}
  <div class="actions">
    <button class="btn ok" onclick={acknowledge}>{t("alarm.acknowledge")}</button>
  </div>
</div>

<style>
  :global(html), :global(body) { background: #1a0000; }
  .alarm { display: flex; flex-direction: column; height: 100vh; background: #1a0000; color: #fff; border: 3px solid #ff3030; animation: frame 1s ease-in-out infinite; }
  .alarm.test { border-color: var(--amber); animation: none; }
  .bar { display: flex; align-items: center; justify-content: center; gap: 12px; padding: 6px; background: #8a0000; font-size: 18px; font-weight: bold; letter-spacing: 0.08em; cursor: move; }
  .bar .title { color: #fff; }
  .blink { animation: blink 0.5s steps(2, start) infinite; }
  .body { flex: 1; padding: 8px 14px; display: flex; flex-direction: column; gap: 6px; overflow: auto; }
  .service { font-size: 20px; font-weight: bold; color: #ffdddd; text-align: center; }
  .grid { display: grid; grid-template-columns: max-content 1fr; gap: 2px 10px; font-size: 12px; }
  .k { color: #ffb0b0; font-weight: bold; }
  .locs { display: flex; flex-direction: column; gap: 1px; font-size: 12px; }
  .loc { font-family: var(--mono); word-break: break-all; color: #ffdddd; }
  .hint { font-size: 11px; color: #ffcccc; text-align: center; }
  .hint.test { color: var(--amber); font-weight: bold; }
  .actions { display: flex; justify-content: center; padding: 8px; }
  .ok { background: #fff; color: #8a0000; border-color: #fff; min-width: 160px; min-height: 28px; font-size: 13px; }
  @keyframes frame { 50% { border-color: #ffffff; } }
</style>
