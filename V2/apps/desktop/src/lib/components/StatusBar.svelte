<script lang="ts">
  import { t } from "$lib/i18n.svelte";
  import { alertActive, motFresh, s, ui } from "$lib/state.svelte";
  import { fmtClock, fmtDateTime, timerLabel } from "$lib/timers";
  import { nextTimer, sleepRemaining, tm } from "$lib/timers.svelte";
  import { strongestLabel } from "$lib/tii";

  // TII (lib/tii.ts): staerkster Sender, z. B. "Langenberg/Hordtberg 30 km NE"
  const tiiLabel = $derived(strongestLabel());

  const clock = $derived(new Date(ui.now).toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit" }));
  // Timer/Aufnahme/Sleep (lib/timers.svelte.ts)
  const recOn = $derived(s.recording || !!tm.recording?.active);
  const nextT = $derived(nextTimer(ui.now));
  const sleepLeft = $derived(sleepRemaining(ui.now));
  const msg = $derived.by(() => {
    if (s.device_error) return { level: "error", text: s.device_error };
    const p = ui.presetStatus;
    if (p && ui.now - p.at < 6000 && p.slot != null) {
      const vars = { n: p.slot + 1, name: p.name, channel: p.channel };
      if (p.status === "tuning") return { level: "info", text: t("preset.tuning", vars) };
      if (p.status === "not_found") return { level: "warn", text: t("preset.not_found", vars) };
      return { level: "info", text: t("preset.selected", vars) };
    }
    // Senderliste (lib/stations.ts): gleiche Zustandsmaschine ohne Slot; Start-Wiederherstellung hat keinen Namen
    if (p && ui.now - p.at < 6000 && p.slot == null && p.name) {
      const vars = { name: p.name, channel: p.channel };
      if (p.status === "tuning") return { level: "info", text: t("stations.tuning", vars) };
      if (p.status === "not_found") return { level: "warn", text: t("stations.not_found", vars) };
      return { level: "info", text: t("stations.selected", vars) };
    }
    if (s.alert?.phase === "pre_trigger") return { level: "warn", text: `${t("alarm.pre")} · SubCh ${s.alert.sub_ch}` };
    if (s.log_tail.length) return { level: "warn", text: s.log_tail[s.log_tail.length - 1] };
    if (nextT) return { level: "info", text: t("timer.next", { name: timerLabel(nextT), time: fmtDateTime(nextT.start_unix) }) };
    return { level: "info", text: s.core_alive ? "" : t("core.starting") };
  });
</script>

<footer class="statusbar">
  <span class="led" class:on={s.ews_present && !alertActive()} class:alarm={alertActive()}>{t("ewf")}</span>
  <span class="led" class:on={s.synced}>{t("sync")}</span>
  <span class="led" class:on={motFresh()}>{t("mot")}</span>
  <span class="msg {msg.level}">{msg.text}</span>
  {#if recOn}<span class="led" style="color: var(--red)" title={tm.recording?.path ?? ""}>● REC {fmtClock(tm.recording?.seconds ?? 0)}</span>{/if}
  {#if tm.sleep}<span class="led on" title={t("sleep.label")}>{t("sleep.remaining", { time: fmtClock(sleepLeft) })}</span>{/if}
  {#if tiiLabel}<span class="led on" title={t("tii.title")} style="font-weight:normal;max-width:180px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">TII {tiiLabel}</span>{/if}
  <span class="clock">{clock}</span>
</footer>
