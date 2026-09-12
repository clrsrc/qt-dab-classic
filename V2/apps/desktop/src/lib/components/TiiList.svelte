<script lang="ts">
  // TII-Liste (Entscheidung 25): mainId/subId, Staerke (Balken relativ zum
  // staerksten), Sendername aus txdata.tii, Entfernung und Azimut zu den
  // Heimatkoordinaten. Daten: `s.tii` (dab-app::tii, sortiert).
  import { t } from "$lib/i18n.svelte";
  import { s, ui } from "$lib/state.svelte";
  import { fmtAzimuth, fmtDistance, hasDistance, hasHome, tiiId } from "$lib/tii";

  const max = $derived(s.tii.length ? Math.max(0.001, s.tii[0].strength) : 1);
  const enabled = $derived(ui.settings?.tii_enabled ?? true);
</script>

<div class="list tii">
  {#if !enabled}
    <div class="empty">{t("tii.disabled")}</div>
  {:else if !s.tii.length}
    <div class="empty">{s.synced ? t("tii.empty") : t("debug.no_sync")}</div>
  {:else}
    {#each s.tii as x, i (`${x.main_id}:${x.sub_id}`)}
      <div class="row" class:best={i === 0} title={x.transmitter ? `${x.transmitter.ensemble} · ${x.transmitter.power_kw} kW · ${x.transmitter.polarization} · ${x.transmitter.direction || "–"}` : ""}>
        <span class="id">{tiiId(x)}</span>
        <span class="bar" title={t("tii.strength")}><i style="width:{Math.round((x.strength / max) * 100)}%"></i></span>
        <span class="str">{x.strength.toFixed(2)}</span>
        <span class="grow name" class:nodb={!x.transmitter}>{x.transmitter?.name ?? t("tii.not_in_db")}</span>
        <span class="meta dist">{fmtDistance(x.distance_km)}</span>
        <span class="meta az">{fmtAzimuth(x.azimuth_deg)}</span>
      </div>
    {/each}
    {#if !hasHome()}
      <div class="hint">{t("tii.no_home")}</div>
    {:else if !hasDistance()}
      <div class="hint">{t("tii.no_distance")}</div>
    {/if}
  {/if}
</div>

<style>
  .tii { min-height: 40px; max-height: 96px; }
  .empty, .hint { padding: 4px 8px; color: var(--green-dim); font-size: 10px; }
  .hint { color: var(--amber); }
  .row { gap: 6px; padding: 1px 6px; font-size: 11px; }
  .row.best { color: var(--green-hi); }
  .id { font-family: var(--mono); min-width: 44px; }
  .bar { width: 60px; height: 7px; background: #0e1a12; border: 1px solid #1e3a26; display: inline-block; }
  .bar i { display: block; height: 100%; background: linear-gradient(90deg, #007a28, #00e050); }
  .str { font-family: var(--mono); font-size: 10px; min-width: 30px; color: #2f8a4a; }
  .name.nodb { color: #2f6a3a; font-style: italic; }
  .dist { min-width: 48px; text-align: right; }
  .az { min-width: 56px; text-align: right; }
</style>
