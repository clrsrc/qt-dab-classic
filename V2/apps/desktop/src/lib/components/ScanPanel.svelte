<script lang="ts">
  import { api } from "$lib/core";
  import { t, tError } from "$lib/i18n.svelte";
  import { notify, s } from "$lib/state.svelte";

  const results = $derived([...s.scan.results].sort((a, b) => a.channel.localeCompare(b.channel, undefined, { numeric: true })));
  const ensembles = $derived(results.filter((r) => r.eid != null).length);
  const pct = $derived(s.scan.total ? Math.round(((s.scan.index + 1) / s.scan.total) * 100) : 0);
  function tune(ch: string) {
    if (!s.scan.active) api.setChannel(ch).catch((e) => notify("warn", tError(e)));
  }
</script>

<section class="panel scan">
  <div class="panel-head">
    <span>{t("panel.scan")}</span>
    <span class="grow"></span>
    <span class="prog">
      {#if s.scan.active}{t("channel.scan_progress", { channel: s.scan.channel, index: s.scan.index + 1, total: s.scan.total })}
      {:else if results.length}{t("channel.scan_done", { n: ensembles })}
      {:else}{t("channel.scan_empty")}{/if}
    </span>
  </div>
  {#if s.scan.active}
    <div class="bar"><i style="width:{pct}%"></i><span>5A … 13F</span></div>
  {/if}
  <div class="list">
    {#if !results.length && !s.scan.active}
      <div class="row"><span class="grow dim">{t("channel.scan_hint")}</span></div>
    {/if}
    {#each results as r (r.channel)}
      <button class="row" class:active={r.channel === s.channel} class:nosig={r.eid == null} onclick={() => tune(r.channel)}>
        <span class="ch">{r.channel}</span>
        <span class="grow">{r.eid != null ? `${r.ensemble ?? ""} (${r.eid.toString(16).toUpperCase().padStart(4, "0")})` : t("channel.no_signal")}</span>
        {#if r.eid != null}<span class="meta">{t("channel.scan_services", { n: r.services.length })}</span>{/if}
        <span class="meta">{r.snr.toFixed(1)} dB</span>
      </button>
    {/each}
  </div>
</section>

<style>
  .scan { display: flex; flex-direction: column; flex: 1 1 100px; min-height: 60px; }
  .prog { font-weight: normal; text-transform: none; letter-spacing: 0; color: var(--green); }
  .bar { position: relative; height: 12px; margin: 3px 4px 0; background: #0a0e14; border: 1px inset #2a2e36; font-size: 8px; text-align: center; color: var(--green-dim); }
  .bar i { position: absolute; left: 0; top: 0; bottom: 0; background: linear-gradient(90deg, #003810, #00a030); }
  .bar span { position: relative; line-height: 10px; }
  .list { flex: 1; margin: 3px 4px; }
  .ch { width: 30px; font-family: var(--mono); }
  .row.nosig { color: #2f4a3a; }
  .dim { color: var(--green-dim); }
</style>
