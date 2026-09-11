<script lang="ts">
  import { startBeep, stopBeep } from "$lib/alarm";
  import { api } from "$lib/core";
  import { t, tError } from "$lib/i18n.svelte";
  import { alertActive, notify, s } from "$lib/state.svelte";

  const visible = $derived(alertActive() && !s.alert?.dismissed);
  const service = $derived.by(() => {
    const a = s.alert;
    if (!a) return "";
    const svc = s.services.find((x) => x.sub_ch === a.sub_ch) ?? (s.current ? s.services.find((x) => x.sid === s.current!.sid) : undefined);
    return svc?.name.trim() ?? `SubCh ${a.sub_ch}`;
  });
  $effect(() => {
    if (visible) startBeep();
    else stopBeep();
    return stopBeep;
  });
  function dismiss() {
    api.ewsDismiss().catch((e) => notify("warn", tError(e)));
    if (s.alert) s.alert.dismissed = true;
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
      {t("alarm.stage", { stage: s.alert.stage })} · {s.alert.phase} · {s.alert.locations.join(", ")}
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
