<script lang="ts">
  // Panel "EWF-Historie" (Bugfixes.txt #10): abgeschlossene Alarme dieser
  // Sitzung, neueste zuerst. Nur im Speicher (App::state::ews_history), nicht
  // ueber Neustarts hinweg - im Unterschied zum EPG-Cache reicht das, um einen
  // Alarm kurz nach dem Ende nochmal ansehen zu koennen. Erreichbar ueber den
  // Button "EWF" in Transport.svelte.
  import type { EwsHistoryEntry, EwsLocationInfo } from "$lib/core";
  import { t } from "$lib/i18n.svelte";
  import { s } from "$lib/state.svelte";
  import { compass, fmtDistance } from "$lib/tii";

  function fmtWhen(unix: number): string {
    const d = new Date(unix * 1000);
    const pad = (n: number) => String(n).padStart(2, "0");
    return `${pad(d.getDate())}.${pad(d.getMonth() + 1)}. ${pad(d.getHours())}:${pad(d.getMinutes())}`;
  }

  function serviceName(subCh: number): string {
    return s.services.find((x) => x.sub_ch === subCh)?.name.trim() ?? `SubCh ${subCh}`;
  }

  /** Wie AlarmWindow.svelte::locationLabel. */
  function locationLabel(loc: EwsLocationInfo): string {
    if (loc.distance_km != null && loc.azimuth_deg != null) {
      return `${loc.code} – ${fmtDistance(loc.distance_km)} ${compass(loc.azimuth_deg)}`;
    }
    if (loc.lat != null && loc.lon != null) {
      const ns = loc.lat >= 0 ? "N" : "S";
      const ew = loc.lon >= 0 ? "O" : "W";
      return `${loc.code} – ≈ ${Math.abs(loc.lat).toFixed(1)}°${ns} ${Math.abs(loc.lon).toFixed(1)}°${ew}`;
    }
    return loc.code;
  }

  function locations(e: EwsHistoryEntry): EwsLocationInfo[] {
    if (e.location_info.length) return e.location_info;
    return e.locations.map((code) => ({ code, lat: null, lon: null, radius_km: null, distance_km: null, azimuth_deg: null }));
  }
</script>

<section class="panel ews-history">
  <div class="panel-head">
    <span>{t("panel.ews_history")}</span>
  </div>
  <div class="list">
    {#if !s.ews_history.length}
      <div class="row"><span class="grow dim">{t("ews_history.empty")}</span></div>
    {/if}
    {#each s.ews_history as e (`${e.iid}-${e.ended_at}`)}
      <div class="entry">
        <div class="head">
          <span class="when">{fmtWhen(e.ended_at)}</span>
          <span class="svc">{serviceName(e.sub_ch)}</span>
          <span class="stage">{t("alarm.stage", { stage: e.stage })}</span>
          {#if e.is_test}<span class="flag">{t("ews_history.test")}</span>{/if}
        </div>
        {#if locations(e).length}
          <div class="locs">
            {#each locations(e) as loc (loc.code)}<span class="loc">{locationLabel(loc)}</span>{/each}
          </div>
        {/if}
      </div>
    {/each}
  </div>
</section>

<style>
  .ews-history { display: flex; flex-direction: column; flex: 1 1 120px; min-height: 80px; }
  .list { flex: 1; margin: 3px 4px; overflow: auto; }
  .entry { padding: 3px 4px; font-size: 10px; border-bottom: 1px solid #14261a; }
  .entry:last-child { border-bottom: none; }
  .head { display: flex; align-items: center; gap: 6px; flex-wrap: wrap; }
  .when { font-family: var(--mono); color: var(--text-dim); }
  .svc { font-weight: bold; }
  .stage { color: var(--text-dim); }
  .flag { font-size: 8px; font-family: var(--mono); border: 1px solid currentColor; padding: 0 2px; color: var(--amber); }
  .locs { display: flex; flex-direction: column; gap: 1px; margin-top: 2px; font-family: var(--mono); color: var(--text-dim); word-break: break-all; }
  .dim { color: var(--green-dim); }
  .row { gap: 5px; padding: 1px 4px; font-size: 10px; }
</style>
