<script lang="ts">
  import { api, type ServiceInfo } from "$lib/core";
  import { t, tError } from "$lib/i18n.svelte";
  import { startServiceDrag } from "$lib/presets";
  import { matchesPty, metaTooltip, ptyName, ptyOptions } from "$lib/metadata";
  import { notify, s } from "$lib/state.svelte";
  import Logo from "./Logo.svelte";

  // Genre-Filter (Programmtyp FIG 0/17): 0 = alle; Datendienste bleiben sichtbar.
  let ptyFilter = $state(0);
  const options = $derived(ptyOptions(s.services));
  const visible = $derived(s.services.filter((x) => !x.is_audio || matchesPty(x, ptyFilter)));
  $effect(() => {
    if (ptyFilter && !options.some((o) => o.pty === ptyFilter)) ptyFilter = 0;
  });

  function select(svc: ServiceInfo) {
    if (!svc.is_audio) return;
    api.selectService(svc.sid, svc.scids).catch((e) => notify("warn", tError(e)));
  }
  const isActive = (svc: ServiceInfo) => !!s.current && s.current.sid === svc.sid && s.current.scids === svc.scids;
</script>

<section class="panel services">
  <div class="panel-head">
    <span>{t("panel.services")}</span>
    {#if options.length}
      <select class="pty" bind:value={ptyFilter} title={t("services.filter_title")}>
        <option value={0}>{t("services.filter_all")}</option>
        {#each options as o (o.pty)}<option value={o.pty}>{o.label}</option>{/each}
      </select>
    {/if}
    <span class="grow"></span>
    <span>{s.services.length ? t("services.count", { n: s.services.length }) : t("services.empty")}</span>
  </div>
  <div class="list">
    {#each visible as svc (`${svc.sid}:${svc.scids}`)}
      <button
        class="row"
        class:active={isActive(svc)}
        class:data={!svc.is_audio}
        draggable={svc.is_audio}
        ondragstart={(e) => startServiceDrag(e, svc)}
        onclick={() => select(svc)}
        title={metaTooltip(svc)}
      >
        {#if svc.is_audio}<Logo eid={s.ensemble?.eid ?? null} sid={svc.sid} size="small" name={svc.name} px={16} />{/if}
        <span class="grow">{svc.name.trim()}{svc.scids ? ` (${svc.scids})` : ""}</span>
        <span class="meta pty">{svc.is_audio ? ptyName(svc.pty) : ""}</span>
        <span class="meta">{svc.is_audio ? `${svc.bitrate_kbps} kbps` : t("services.data")}</span>
        <span class="meta">{svc.sid.toString(16).toUpperCase().padStart(4, "0")}</span>
      </button>
    {/each}
  </div>
</section>

<style>
  .services { display: flex; flex-direction: column; flex: 1 1 120px; min-height: 80px; }
  .list { flex: 1; margin: 3px 4px; }
  .row.data { color: #2f6a3a; }
  .pty { min-height: 14px; padding: 0 2px; font-size: 9px; margin-left: 6px; font-weight: normal; letter-spacing: 0; text-transform: none; }
  .meta.pty { font-family: var(--font); font-size: 9px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 90px; }
</style>
