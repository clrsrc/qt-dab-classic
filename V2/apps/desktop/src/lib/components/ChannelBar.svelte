<script lang="ts">
  import { api, BAND_III, type SourceKind } from "$lib/core";
  import { t, tError } from "$lib/i18n.svelte";
  import { notify, patchSettings, refreshState, s, togglePanel, ui } from "$lib/state.svelte";

  let device = $state("hackrf");
  let filePath = $state("");
  let fileLoop = $state(true);
  let rtlIndex = $state(0);
  let initialised = false;
  $effect(() => {
    if (ui.settings && !initialised) {
      initialised = true;
      device = ui.settings.device;
      filePath = ui.settings.last_file ?? "";
      fileLoop = ui.settings.file_loop;
      rtlIndex = ui.settings.rtlsdr_index;
    }
  });
  const run = (p: Promise<unknown>) => p.catch((e) => notify("warn", tError(e)));
  const found = $derived(new Map(s.scan.results.filter((r) => r.eid != null).map((r) => [r.channel, r.ensemble ?? ""])));

  function onChannel(e: Event) {
    const ch = (e.target as HTMLSelectElement).value;
    if (ch) run(api.setChannel(ch));
  }
  async function open() {
    let source: SourceKind;
    if (device === "file") {
      if (!filePath) return;
      source = { kind: "file", path: filePath, loop: fileLoop };
    } else if (device === "rtlsdr") {
      source = { kind: "rtl_sdr", index: rtlIndex };
    } else {
      source = { kind: "hack_rf", serial: null };
    }
    await run(api.openDevice(source));
    await refreshState();
    if (device !== "file" && s.channel) await run(api.setChannel(s.channel));
    else if (device !== "file" && ui.settings?.last_channel) await run(api.setChannel(ui.settings.last_channel));
  }
  async function pick() {
    const p = await api.pickFile();
    if (p) filePath = p;
  }
  function scan() {
    if (s.scan.active) run(api.stopScan());
    else {
      if (ui.settings && !ui.settings.panels.scan) void togglePanel("scan");
      run(api.startScan());
    }
  }
  function onDevice(e: Event) {
    device = (e.target as HTMLSelectElement).value;
    void patchSettings({ device });
  }
</script>

<section class="chan">
  <span class="lbl">{t("device.label")}</span>
  <select value={device} onchange={onDevice}>
    <option value="hackrf">{t("device.hackrf")}</option>
    <option value="rtlsdr">{t("device.rtlsdr")}</option>
    <option value="file">{t("device.file")}</option>
  </select>
  {#if device === "file"}
    <input type="text" class="path" bind:value={filePath} placeholder={t("device.path")} />
    <button class="btn" onclick={pick} title={t("device.pick")}>…</button>
    <label><input type="checkbox" bind:checked={fileLoop} />{t("device.loop")}</label>
  {:else if device === "rtlsdr"}
    <input type="number" class="idx" bind:value={rtlIndex} min="0" max="9" title={t("device.rtlsdr_index")} />
  {/if}
  <button class="btn" class:on={!!s.device} onclick={open}>{t("device.open")}</button>
  {#if s.device}
    <button class="btn" onclick={() => run(api.send({ type: "close_device" }).then(refreshState))}>{t("device.close")}</button>
  {/if}
  <span class="grow"></span>
  <span class="lbl">{t("channel.label")}</span>
  <select value={s.channel ?? ""} onchange={onChannel} disabled={s.scan.active || device === "file"}>
    <option value="" disabled>--</option>
    {#each BAND_III as ch (ch)}
      <option value={ch}>{ch}{found.has(ch) ? ` ● ${found.get(ch)}` : ""}</option>
    {/each}
  </select>
  <button class="btn" class:on={s.scan.active} onclick={scan} disabled={!s.device || device === "file"}>
    {s.scan.active ? t("channel.scan_stop") : t("channel.scan")}
  </button>
</section>

<style>
  .chan { display: flex; align-items: center; gap: 4px; padding: 3px 6px; flex: none; flex-wrap: wrap; border-top: 1px solid var(--chrome-lo); }
  .lbl { font-size: 9px; font-weight: bold; color: #7080a0; }
  .grow { flex: 1; }
  .path { flex: 1 1 140px; min-width: 100px; }
  .idx { width: 44px; }
  label { display: flex; align-items: center; font-size: 10px; }
</style>
