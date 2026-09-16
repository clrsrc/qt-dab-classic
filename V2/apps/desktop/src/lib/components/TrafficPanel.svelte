<script lang="ts">
  // Panel "Verkehr" (Auftrag 16.09.2026): Verkehrs- und sonstige Durchsagen
  // (EN 300 401 8.1.6, FIG 0/18 Ankuendigungs-Unterstuetzung + FIG 0/19
  // Umschaltung), protokolliert wie die EWF-Historie: laufende Durchsage oben,
  // darunter die beendeten dieser Sitzung, neueste zuerst. Kein TPEG-Decoder:
  // der Datendienst "ARD TPEG" (App-Typ 4) bleibt undekodiert, hier geht es
  // um die Audio-Durchsagen, auf die ein Radio mit "TA" umschaltet.
  // Erreichbar ueber den Button "TA" neben EPG in Transport.svelte.
  import type { TrafficEntry } from "$lib/core";
  import { t } from "$lib/i18n.svelte";
  import { s, ui, patchSettings } from "$lib/state.svelte";
  import { playRecording, playback } from "$lib/playback.svelte";

  const KIND_KEYS = ["alarm", "road", "transport", "warning", "news", "weather", "event", "special", "programme", "sport", "financial"] as const;

  function kinds(flags: number): string[] {
    const out: string[] = [];
    KIND_KEYS.forEach((k, bit) => {
      if (flags & (1 << bit)) out.push(t(`traffic.kind.${k}`));
    });
    if (!out.length) out.push(t("traffic.kind.unknown"));
    return out;
  }

  function fmtWhen(unix: number): string {
    const d = new Date(unix * 1000);
    const pad = (n: number) => String(n).padStart(2, "0");
    return `${pad(d.getDate())}.${pad(d.getMonth() + 1)}. ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
  }

  function fmtDuration(e: TrafficEntry): string {
    const end = e.ended_at ?? Math.floor(Date.now() / 1000);
    const sec = Math.max(0, end - e.started_at);
    return sec >= 60 ? `${Math.floor(sec / 60)}:${String(sec % 60).padStart(2, "0")} min` : `${sec} s`;
  }

  const entries = $derived<TrafficEntry[]>(s.traffic_active ? [s.traffic_active, ...s.traffic_history] : s.traffic_history);
  const supported = $derived(s.traffic_supported);
</script>

<section class="panel traffic">
  <div class="panel-head">
    <span>{t("panel.traffic")}</span>
    <span class="grow"></span>
    <span class="k" title={t("traffic.support_hint")}>{supported ? t("traffic.supported") : t("traffic.unsupported")}</span>
    <label class="inl" title={t("traffic.record_hint")}>
      <input type="checkbox" checked={ui.settings?.announcement_record ?? true} onchange={(e) => void patchSettings({ announcement_record: (e.currentTarget as HTMLInputElement).checked })} />
      {t("traffic.record")}
    </label>
    <label class="inl" title={t("traffic.autoswitch_hint")}>
      <input type="checkbox" checked={ui.settings?.traffic_autoswitch ?? false} onchange={(e) => void patchSettings({ traffic_autoswitch: (e.currentTarget as HTMLInputElement).checked })} />
      {t("traffic.autoswitch")}
    </label>
  </div>
  <div class="list">
    {#if !entries.length}
      <div class="row"><span class="grow dim">{t("traffic.empty")}</span></div>
    {/if}
    {#each entries as e (e.id)}
      <div class="entry" class:active={e.ended_at === null}>
        <div class="head">
          <span class="when">{fmtWhen(e.started_at)}</span>
          <span class="svc">{e.announcing_service ?? `SubCh ${e.sub_ch}`}</span>
          {#each kinds(e.flags) as k (k)}<span class="flag">{k}</span>{/each}
          {#if e.ended_at === null}<span class="flag live">{t("traffic.running")}</span>{/if}
          {#if e.switched}<span class="flag sw" title={t("traffic.switched_hint")}>{t("traffic.switched")}</span>{/if}
          <span class="dur">{fmtDuration(e)}</span>
          {#if e.file && e.ended_at !== null}
            <button class="btn mini play" class:on={playback.path === e.file} title={e.file} onclick={() => void playRecording(e.file!, `${e.announcing_service ?? ""} ${fmtWhen(e.started_at)}`)}>▶ {t("traffic.play")}</button>
          {:else if e.file}
            <span class="flag rec">{t("traffic.recording")}</span>
          {/if}
        </div>
        <div class="meta">
          {e.channel} · {e.ensemble} · {t("traffic.cluster", { cluster: e.cluster })}
          {#if e.services.length}· {t("traffic.for")} {e.services.join(", ")}{/if}
        </div>
      </div>
    {/each}
  </div>
  <div class="foot dim">{t("traffic.tpeg_hint")}</div>
</section>

<style>
  .traffic { display: flex; flex-direction: column; flex: 1 1 120px; min-height: 80px; }
  .panel-head { display: flex; align-items: center; gap: 8px; }
  .grow { flex: 1; }
  .k { color: var(--text-dim); font-size: 9px; }
  .inl { display: inline-flex; align-items: center; gap: 3px; font-size: 9px; color: var(--text-dim); }
  .list { flex: 1; margin: 3px 4px; overflow: auto; }
  .entry { padding: 3px 4px; font-size: 10px; border-bottom: 1px solid #14261a; }
  .entry:last-child { border-bottom: none; }
  .entry.active { background: #0e2a12; }
  .head { display: flex; align-items: center; gap: 6px; flex-wrap: wrap; }
  .when { font-family: var(--mono); color: var(--text-dim); }
  .svc { font-weight: bold; }
  .dur { margin-left: auto; font-family: var(--mono); color: var(--text-dim); }
  .flag { font-size: 8px; font-family: var(--mono); border: 1px solid currentColor; padding: 0 2px; color: var(--amber); }
  .flag.live { color: var(--green); }
  .flag.sw { color: var(--text-dim); }
  .flag.rec { color: var(--red, #e04040); }
  .play { margin-left: 4px; }
  .meta { margin-top: 2px; font-size: 9px; color: var(--text-dim); }
  .dim { color: var(--green-dim); }
  .row { gap: 5px; padding: 1px 4px; font-size: 10px; }
  .foot { font-size: 9px; padding: 2px 6px; border-top: 1px solid #14261a; }
</style>
