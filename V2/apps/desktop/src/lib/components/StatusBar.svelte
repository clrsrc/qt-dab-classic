<script lang="ts">
  import { t } from "$lib/i18n.svelte";
  import { alertActive, motFresh, s, ui } from "$lib/state.svelte";

  const clock = $derived(new Date(ui.now).toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit" }));
  const msg = $derived.by(() => {
    if (s.device_error) return { level: "error", text: s.device_error };
    const p = ui.presetStatus;
    if (p && ui.now - p.at < 6000 && p.slot != null) {
      const vars = { n: p.slot + 1, name: p.name, channel: p.channel };
      if (p.status === "tuning") return { level: "info", text: t("preset.tuning", vars) };
      if (p.status === "not_found") return { level: "warn", text: t("preset.not_found", vars) };
      return { level: "info", text: t("preset.selected", vars) };
    }
    if (s.alert?.phase === "pre_trigger") return { level: "warn", text: `${t("alarm.pre")} · SubCh ${s.alert.sub_ch}` };
    if (s.log_tail.length) return { level: "warn", text: s.log_tail[s.log_tail.length - 1] };
    return { level: "info", text: s.core_alive ? "" : t("core.starting") };
  });
</script>

<footer class="statusbar">
  <span class="led" class:on={s.ews_present && !alertActive()} class:alarm={alertActive()}>{t("ewf")}</span>
  <span class="led" class:on={s.synced}>{t("sync")}</span>
  <span class="led" class:on={motFresh()}>{t("mot")}</span>
  <span class="msg {msg.level}">{msg.text}</span>
  <span class="clock">{clock}</span>
</footer>
