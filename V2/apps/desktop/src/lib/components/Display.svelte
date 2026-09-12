<script lang="ts">
  import { channelMhz } from "$lib/core";
  import { t } from "$lib/i18n.svelte";
  import { currentService, dlPlusTitle, s, slideUrl, ui } from "$lib/state.svelte";
  import { fmtClock, remainingMin } from "$lib/epg";
  import Logo from "./Logo.svelte";

  const svc = $derived(currentService());
  const codec = $derived(s.current?.codec);
  const codecText = $derived(
    !codec ? "" : codec.codec === "he_aac" ? `AAC${codec.sbr ? "+" : ""}${codec.ps ? " PS" : ""}` : codec.codec === "mp2" ? "MP2" : "DATA",
  );
  const eid = $derived(s.ensemble ? s.ensemble.eid.toString(16).toUpperCase().padStart(4, "0") : "");
  const dlp = $derived(dlPlusTitle());
  const slide = $derived(slideUrl());
  const headline = $derived.by(() => {
    if (s.pending) return s.pending.name ? t("display.searching_service", { name: s.pending.name, channel: s.pending.channel }) : t("display.searching", { channel: s.pending.channel });
    if (svc) return svc.name.trim();
    if (!s.device) return t("display.no_device");
    if (s.channel && !s.synced && !s.scan.active) return `${s.channel}: ${t("display.no_signal")}`;
    return t("display.no_service");
  });
  const fileText = $derived.by(() => {
    const f = s.file;
    if (!f) return "";
    const fmt = (x: number) => `${Math.floor(x / 60)}:${String(Math.floor(x % 60)).padStart(2, "0")}`;
    return `${fmt(f.position_s)} / ${fmt(f.length_s)}${f.loop ? " ↻" : ""}${f.ended ? ` ${t("display.file_ended")}` : ""}`;
  });
  const vu = (x: number) => Math.min(100, Math.max(0, Math.sqrt(Math.max(0, x)) * 100));
</script>

<section class="lcd display">
  <div class="row1">
    <span class="hi ens">{s.ensemble ? `${s.ensemble.name.trim()} (${eid})` : t("display.no_ensemble")}</span>
    <span class="dim">{s.channel ?? "--"} · {channelMhz(s.channel)} MHz</span>
    <span class="dim">{s.snr.toFixed(1)} dB</span>
  </div>
  <div class="name" class:pending={!!s.pending}>{headline}</div>
  <div class="row1 tech">
    <span>{svc ? `${svc.bitrate_kbps} kbps` : "--- kbps"}</span>
    <span>{codecText || "---"}</span>
    <span>{s.current ? (s.current.stereo ? t("display.stereo") : t("display.mono")) : "----"}</span>
    <span class="dim">{s.fic_total ? `FIC ${s.fic_ok}/${s.fic_total}` : ""}</span>
    <span class="dim">{fileText}</span>
  </div>
  {#if slide || s.logo_data_url}
    <div class="media">
      <Logo src={s.logo_data_url} name={svc?.name ?? ""} size="medium" px={slide ? 84 : 48} />
      {#if slide}<img src={slide} alt={s.slide?.name ?? "slide"} />{/if}
    </div>
  {/if}
  {#if s.now_next && (s.now_next.now || s.now_next.next)}
    <div class="epgline" title={s.now_next.now?.legacy_time || s.now_next.next?.legacy_time ? t("epg.legacy_hint") : ""}>
      {#if s.now_next.now}
        <span class="dim">{t("epg.now")}</span> <span class="hi">{fmtClock(s.now_next.now.start_unix)} {s.now_next.now.title}</span>
        <span class="amber">· {t("epg.remaining", { min: remainingMin(s.now_next.now.start_unix, s.now_next.now.duration_min, ui.now) })}</span>
      {/if}
      {#if s.now_next.next}
        <span class="dim"> {t("epg.next")}</span> <span>{fmtClock(s.now_next.next.start_unix)} {s.now_next.next.title}</span>
      {/if}
    </div>
  {/if}
  <div class="dlp">
    {#if dlp}
      <span class="hi">{dlp.title}</span>
      {#if dlp.artist}<span class="dim"> · </span><span>{dlp.artist}</span>{/if}
      {#if s.dl_plus && !s.dl_plus.item_running}<span class="dim"> (pause)</span>{/if}
    {:else}
      <span class="dim">{s.current ? "DL+ ---" : ""}</span>
    {/if}
  </div>
  <div class="ticker"><span class:scroll={s.dls.length > 70}>{s.dls || " "}</span></div>
  <div class="vu">
    <span class="dim">L</span><span class="bar"><i style="width:{vu(s.level[0])}%"></i></span>
    <span class="dim">R</span><span class="bar"><i style="width:{vu(s.level[1])}%"></i></span>
    {#if s.muted}<span class="dim">MUTE</span>{/if}
  </div>
</section>

<style>
  .display { padding: 4px 8px; display: flex; flex-direction: column; gap: 2px; flex: none; }
  .row1 { display: flex; gap: 12px; font-size: 10px; white-space: nowrap; }
  .row1 .ens { flex: 1; overflow: hidden; text-overflow: ellipsis; }
  .tech span { min-width: 56px; }
  .name { font-size: 16px; font-weight: bold; color: var(--green-hi); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; padding: 1px 0; }
  .name.pending { color: var(--amber); animation: blink 1s steps(2, start) infinite; }
  .media { display: flex; gap: 6px; align-items: center; }
  .media img { height: 84px; max-width: calc(100% - 90px); object-fit: contain; }
  .epgline { font-size: 11px; min-height: 15px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .epgline .amber { color: var(--amber); }
  .dlp { font-size: 11px; min-height: 15px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .ticker { font-size: 11px; height: 16px; overflow: hidden; white-space: nowrap; position: relative; border-top: 1px solid #0f2a16; }
  .ticker span { display: inline-block; }
  .ticker span.scroll { animation: ticker 24s linear infinite; padding-left: 100%; }
  .vu { display: flex; align-items: center; gap: 4px; font-size: 9px; }
  .bar { flex: 1; height: 6px; background: #0a1a0e; border: 1px solid #1f3a26; }
  .bar i { display: block; height: 100%; background: linear-gradient(90deg, #00c040 60%, #d9d23a 85%, #ff3030); transition: width 60ms linear; }
</style>
