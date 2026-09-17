<script lang="ts">
  // Tab "Senderliste": alle Dienste aller bekannten Ensembles (Scan oder
  // gehoert), gruppiert je Ensemble. Klick schaltet um – auch mit
  // Kanalwechsel (dab-app::tune_station, gleiche Zustandsmaschine wie die
  // Speicher). Rechtsklick/Drag belegen einen Speicher wie im Ensemble-Tab.
  import { api } from "$lib/core";
  import { confirm, openMenu } from "$lib/dialogs.svelte";
  import { t, tError } from "$lib/i18n.svelte";
  import { startServiceDrag, storePreset } from "$lib/presets";
  import { notify, s, togglePanel, ui } from "$lib/state.svelte";
  import { groupStations, hexEid, stationsApi, type StationEntry } from "$lib/stations";
  import { matchesPty, metaTooltip, ptyName, ptyOptions } from "$lib/metadata";
  import Logo from "./Logo.svelte";

  let filter = $state("");
  let audioOnly = $state(true);
  // Genre-Filter (Programmtyp FIG 0/17), 0 = alle. Der Textfilter trifft auch den Genre-Namen.
  let ptyFilter = $state(0);
  const options = $derived(ptyOptions(s.stations));
  $effect(() => {
    if (ptyFilter && !options.some((o) => o.pty === ptyFilter)) ptyFilter = 0;
  });

  const visible = $derived.by(() => {
    const q = filter.trim().toLowerCase();
    return s.stations.filter(
      (e) =>
        (!audioOnly || e.is_audio) &&
        (!e.is_audio || matchesPty(e, ptyFilter)) &&
        (!q || e.name.toLowerCase().includes(q) || e.ensemble.toLowerCase().includes(q) || ptyName(e.pty).toLowerCase().includes(q)),
    );
  });
  const groups = $derived(groupStations(visible));
  const total = $derived(s.stations.length);
  const ensembles = $derived(new Set(s.stations.map((e) => `${e.channel}:${e.eid}`)).size);
  const run = (p: Promise<unknown>) => p.catch((e) => notify("warn", tError(e)));
  const isFile = $derived(s.device?.kind === "file");

  const isActive = (e: StationEntry) =>
    !!s.current && !!s.ensemble && s.ensemble.eid === e.eid && s.current.sid === e.sid && s.current.scids === e.scids;

  function tune(e: StationEntry) {
    if (!e.is_audio) return;
    run(stationsApi.tune(e));
  }
  function tuneChannel(channel: string) {
    if (!isFile && !s.scan.active) run(api.setChannel(channel));
  }
  function menu(e: StationEntry, ev: MouseEvent) {
    ev.preventDefault();
    if (!e.is_audio) return;
    openMenu(
      ev.clientX,
      ev.clientY,
      Array.from({ length: 10 }, (_, slot) => ({
        label: t("stations.store_slot", { n: slot + 1, name: ui.presets.slots[slot]?.name ?? t("preset.empty") }),
        action: () => storePreset(slot, e),
      })),
    );
  }
  async function clearAll() {
    const ok = await confirm(t("stations.clear_title"), t("stations.clear_text", { n: total }), t("modal.yes"), t("modal.cancel"));
    if (ok) await run(stationsApi.clear().then(() => notify("info", t("stations.cleared"))));
  }
  function goScan() {
    if (ui.settings && !ui.settings.panels.scan) void togglePanel("scan");
    if (s.device && !isFile && !s.scan.active) run(api.startScan());
  }
</script>

<section class="panel stations">
  <div class="panel-head">
    <span>{t("panel.stations")}</span>
    <input type="text" class="filter" bind:value={filter} placeholder={t("stations.filter")} title={t("stations.filter_hint")} />
    <label class="k"><input type="checkbox" bind:checked={audioOnly} />{t("stations.audio_only")}</label>
    {#if options.length}
      <select class="pty" bind:value={ptyFilter} title={t("services.filter_title")}>
        <option value={0}>{t("services.filter_all")}</option>
        {#each options as o (o.pty)}<option value={o.pty}>{o.label}</option>{/each}
      </select>
    {/if}
    <span class="grow"></span>
    <span class="cnt">{total ? t(ensembles === 1 ? "stations.count_one" : "stations.count", { n: total, m: ensembles }) : t("services.empty")}</span>
    {#if total}<button class="btn mini" onclick={clearAll} title={t("stations.clear_hint")}>{t("stations.clear")}</button>{/if}
  </div>
  <div class="list">
    {#if !total}
      <div class="empty">
        <div>{t("stations.empty")}</div>
        <button class="btn" onclick={goScan} disabled={isFile || !s.device}>{s.device && !isFile ? t("stations.scan") : t("stations.scan_open")}</button>
        {#if s.scan.active}<div class="dim">{t("channel.scan_progress", { channel: s.scan.channel, index: s.scan.index + 1, total: s.scan.total })}</div>{/if}
      </div>
    {:else if !groups.length}
      <div class="empty dim">{t("stations.no_match")}</div>
    {/if}
    {#each groups as g (g.key)}
      <button class="row grp" class:cur={s.ensemble?.eid === g.eid && s.channel === g.channel} onclick={() => tuneChannel(g.channel)} title={t("stations.tune_channel", { channel: g.channel })}>
        <span class="ch">{g.channel}</span>
        <span class="grow">{g.ensemble || "–"} <span class="eid">({hexEid(g.eid)})</span></span>
        <span class="meta">{t("stations.group_services", { n: g.entries.length })}</span>
      </button>
      {#each g.entries as e (`${e.sid}:${e.scids}`)}
        <button
          class="row svc"
          class:active={isActive(e)}
          class:data={!e.is_audio}
          draggable={e.is_audio}
          ondragstart={(ev) => startServiceDrag(ev, e)}
          onclick={() => tune(e)}
          oncontextmenu={(ev) => menu(e, ev)}
          title={e.is_audio ? [metaTooltip(e), t("stations.row_hint")].filter(Boolean).join("\n") : ""}
        >
          {#if e.is_audio}<Logo eid={e.eid} sid={e.sid} size="small" name={e.name} px={16} />{/if}
          <span class="grow">{e.name}{e.scids ? ` (${e.scids})` : ""}</span>
          <span class="meta pty">{e.is_audio ? ptyName(e.pty) : ""}</span>
          <span class="meta">{e.is_audio ? `${e.bitrate_kbps} kbps` : t("services.data")}</span>
          <span class="meta">{e.sid.toString(16).toUpperCase().padStart(4, "0")}</span>
        </button>
      {/each}
    {/each}
  </div>
</section>

<style>
  .stations { display: flex; flex-direction: column; flex: 1 1 160px; min-height: 100px; }
  .list { flex: 1; margin: 3px 4px; }
  .filter { width: 110px; min-height: 14px; padding: 0 3px; font-size: 10px; }
  .pty { min-height: 14px; padding: 0 2px; font-size: 9px; font-weight: normal; letter-spacing: 0; text-transform: none; }
  .meta.pty { font-family: var(--font); font-size: 9px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 90px; }
  .k { display: flex; align-items: center; gap: 2px; font-weight: normal; letter-spacing: 0; text-transform: none; font-size: 9px; }
  .cnt { font-weight: normal; letter-spacing: 0; text-transform: none; color: var(--green); }
  .grp { position: sticky; top: 0; background: #0e1614; color: #7fd6a0; font-size: 10px; font-weight: bold; padding: 1px 6px; border-bottom: 1px solid #1a2e22; }
  .grp:hover { background: #142018; }
  .grp.cur { color: var(--green-hi); }
  .grp .ch { width: 30px; font-family: var(--mono); }
  .grp .eid { font-weight: normal; color: #2f8a4a; font-family: var(--mono); }
  .svc { padding-left: 14px; }
  .row.data { color: #2f6a3a; }
  .empty { padding: 10px 8px; display: flex; flex-direction: column; gap: 6px; align-items: flex-start; }
  .dim { color: var(--green-dim); }
</style>
