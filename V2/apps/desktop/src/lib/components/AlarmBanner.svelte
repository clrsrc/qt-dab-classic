<script lang="ts">
  import { t, tError } from "$lib/i18n.svelte";
  import { alertActive, dismissAlert, notify, s } from "$lib/state.svelte";
  import { compass, fmtDistance } from "$lib/tii";

  const visible = $derived(alertActive() && !s.alert?.dismissed);
  const service = $derived.by(() => {
    const a = s.alert;
    if (!a) return "";
    const svc = s.services.find((x) => x.sub_ch === a.sub_ch) ?? (s.current ? s.services.find((x) => x.sid === s.current!.sid) : undefined);
    return svc?.name.trim() ?? `SubCh ${a.sub_ch}`;
  });
  /** Kompakte Ortsangabe: naechstliegender Ortscode, sonst die rohen Codes
   * (Uebersetzung dab_app::ews_location braucht Heimatkoordinaten). */
  const locationSummary = $derived.by(() => {
    const a = s.alert;
    if (!a) return "";
    const withDistance = a.location_info.filter((l) => l.distance_km != null);
    if (withDistance.length) {
      const nearest = withDistance.reduce((min, l) => (l.distance_km! < min.distance_km! ? l : min));
      return `${fmtDistance(nearest.distance_km)} ${compass(nearest.azimuth_deg!)}`;
    }
    return a.locations.join(", ");
  });
  // Warnton kommt aus dem Alarmfenster (AlarmWindow.svelte, Entscheidung 10),
  // damit er nicht doppelt spielt und in den Einstellungen abschaltbar ist.
  // Quittierung erreicht auch das Alarmfenster (Befund 2, state.dismissAlert).
  function dismiss() {
    dismissAlert().catch((e) => notify("warn", tError(e)));
  }
</script>

{#if visible && s.alert}
  <div class="alarm" role="alert">
    <div class="head">
      <span class="blink">⚠</span>
      <span class="title">{t("alarm.title")}{s.alert.is_test ? ` (${t("alarm.test")})` : ""} – {service}</span>
      <span class="blink">⚠</span>
    </div>
    <div class="info">
      {t("alarm.stage", { stage: s.alert.stage })} · {s.alert.phase} · {locationSummary}
      {#if s.ews_switched_from != null}<br />{t("alarm.switched_from")}{/if}
    </div>
    <button class="btn ok" onclick={dismiss}>{t("alarm.dismiss")}</button>
  </div>
{/if}

<style>
  .alarm { background: #8a0000; color: #fff; border: 2px solid #ff4040; padding: 6px 10px; text-align: center; flex: none; animation: pulse 1s ease-in-out infinite; }
  .head { display: flex; align-items: center; justify-content: center; gap: 10px; font-size: 14px; font-weight: bold; letter-spacing: 0.05em; }
  .blink { animation: blink 0.5s steps(2, start) infinite; }
  .info { font-size: 10px; margin: 3px 0 6px; opacity: 0.9; }
  .ok { background: #fff; color: #8a0000; border-color: #fff; min-width: 120px; font-size: 11px; }
  @keyframes pulse { 50% { background: #b00000; } }
</style>
