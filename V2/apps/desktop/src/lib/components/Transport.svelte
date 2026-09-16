<script lang="ts">
  import { api } from "$lib/core";
  import { t, tError } from "$lib/i18n.svelte";
  import { notify, s, setMute, setVolume, toggleMute, togglePanel, ui } from "$lib/state.svelte";
  import RecButton from "./RecButton.svelte";

  let volTimer: ReturnType<typeof setTimeout> | undefined;
  function onVolume(e: Event) {
    const v = Number((e.target as HTMLInputElement).value);
    s.volume = v;
    clearTimeout(volTimer);
    volTimer = setTimeout(() => setVolume(v).catch((x) => notify("warn", tError(x))), 40);
  }
  const run = (p: Promise<unknown>) => p.catch((e) => notify("warn", tError(e)));
  async function stop() {
    if (s.current) await run(api.send({ type: "stop_service", slot: "primary" }));
  }
</script>

<section class="transport">
  <button class="btn transport" title={t("transport.prev")} onclick={() => run(api.stepService(-1))}>|◀</button>
  <button class="btn transport" class:on={!!s.current && !s.muted} title={t("transport.unmute")} onclick={() => run(setMute(false))}>▶</button>
  <button class="btn transport" title={t("transport.stop")} onclick={stop}>■</button>
  <button class="btn transport" title={t("transport.next")} onclick={() => run(api.stepService(1))}>▶|</button>
  <button class="btn transport" class:on={s.muted} title={t("transport.mute")} onclick={() => run(toggleMute())}>🔇</button>
  <span class="sep"></span>
  <button class="btn feat" class:on={ui.settings?.panels.epg} title={t("transport.epg")} onclick={() => togglePanel("epg")}>EPG</button>
  <button class="btn feat" class:on={ui.settings?.panels.traffic || !!s.traffic_active} title={t("transport.traffic")} onclick={() => togglePanel("traffic")}>TA</button>
  <button class="btn feat" class:on={s.ews_present} title={t("transport.ewf")} onclick={() => togglePanel("ews_history")}>EWF</button>
  <button class="btn feat" class:on={ui.settings?.panels.timer} title={t("transport.timer")} onclick={() => togglePanel("timer")}>TMR</button>
  <RecButton />
  <span class="sep"></span>
  <span class="vol">VOL</span>
  <input type="range" min="0" max="100" value={s.volume} oninput={onVolume} title={t("transport.volume")} />
  <span class="vol num">{s.volume}</span>
</section>

<style>
  .transport { display: flex; align-items: center; gap: 2px; padding: 3px 6px; flex: none; }
  .sep { width: 8px; }
  .feat { font-size: 8px; min-width: 30px; padding: 1px 3px; }
  .vol { font-size: 9px; color: #7080a0; font-weight: bold; }
  .vol.num { width: 22px; text-align: right; font-family: var(--mono); color: var(--green); }
  input[type="range"] { flex: 1; min-width: 60px; }
</style>
