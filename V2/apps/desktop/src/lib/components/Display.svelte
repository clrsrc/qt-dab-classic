<script lang="ts">
  import { untrack } from "svelte";
  import { openUrl } from "@tauri-apps/plugin-opener";
  import { channelMhz } from "$lib/core";
  import { dialogs } from "$lib/dialogs.svelte";
  import { t, tError } from "$lib/i18n.svelte";
  import { currentService, dlPlusTitle, notify, s, slideUrl, ui } from "$lib/state.svelte";
  import { fmtClock, remainingMin } from "$lib/epg";
  import { fmtOffset } from "$lib/timeshift";
  import { linkify } from "$lib/linkify";
  import { languageName, ptyName } from "$lib/metadata";
  import Logo from "./Logo.svelte";

  const svc = $derived(currentService());
  const codec = $derived(s.current?.codec);
  const codecText = $derived(
    !codec ? "" : codec.codec === "he_aac" ? `AAC${codec.sbr ? "+" : ""}${codec.ps ? " PS" : ""}` : codec.codec === "mp2" ? "MP2" : "DATA",
  );
  const eid = $derived(s.ensemble ? s.ensemble.eid.toString(16).toUpperCase().padStart(4, "0") : "");
  const dlp = $derived(dlPlusTitle());
  const dlsSegments = $derived(linkify(s.dls));
  const titleSegments = $derived(linkify(dlp?.title ?? ""));
  const artistSegments = $derived(linkify(dlp?.artist ?? ""));
  const openLink = (e: MouseEvent, url: string) => {
    e.preventDefault();
    // Opener-Plugin kann ablehnen (Scope, fehlender Browser): melden statt stumm (Befund 12).
    openUrl(url).catch((x) => notify("warn", tError(x)));
  };
  const slide = $derived(slideUrl());
  const slideSrc = (x: { mime: string; data_b64: string }) => `data:${x.mime || "image/jpeg"};base64,${x.data_b64}`;
  // Vergroesserung: Index im Streifen (-1 = zu).
  let zoomIdx = $state(-1);
  const zoomSrc = $derived(zoomIdx >= 0 && zoomIdx < s.slides.length ? slideSrc(s.slides[zoomIdx]) : null);
  // Bugfixes.txt #12 galt nur dem Logo (Logo.svelte); das meist groessere und
  // zuerst ins Auge fallende MOT-SlideShow-Bild daneben hatte gar keine
  // Vergroesserung - hier nachgezogen (Fund aus Stefans Live-Test 15.09.2026).
  const slideZoomed = $derived(zoomSrc !== null);
  function onSlideKey(e: KeyboardEvent) {
    if (!slideZoomed) return;
    if (e.key === "Escape") zoomIdx = -1;
    else if (e.key === "ArrowLeft" && zoomIdx > 0) zoomIdx--;
    else if (e.key === "ArrowRight" && zoomIdx < s.slides.length - 1) zoomIdx++;
  }
  // Offene Vergroesserung anmelden, damit Escape nicht zusaetzlich Timeshift
  // auf live springen laesst (lib/hotkeys.ts, Befund 5).
  $effect(() => {
    if (!slideZoomed) return;
    untrack(() => dialogs.zoom++);
    return () => {
      dialogs.zoom--;
    };
  });
  const headline = $derived.by(() => {
    if (s.pending) return s.pending.name ? t("display.searching_service", { name: s.pending.name, channel: s.pending.channel }) : t("display.searching", { channel: s.pending.channel });
    if (svc) return svc.name.trim();
    if (!s.device) return t("display.no_device");
    // "kein Signal" erst nach einer no_signal-Meldung des Kerns; waehrend der
    // Sync-Suche nach einem Kanalwechsel wechselt `synced` mehrmals pro Sekunde.
    if (s.channel && ui.noSignal && !s.synced && !s.scan.active) return `${s.channel}: ${t("display.no_signal")}`;
    if (s.channel && !s.ensemble && !s.scan.active) return t("display.searching", { channel: s.channel });
    return t("display.no_service");
  });
  const fileText = $derived.by(() => {
    const f = s.file;
    if (!f) return "";
    const fmt = (x: number) => `${Math.floor(x / 60)}:${String(Math.floor(x % 60)).padStart(2, "0")}`;
    return `${fmt(f.position_s)} / ${fmt(f.length_s)}${f.loop ? " ↻" : ""}${f.ended ? ` ${t("display.file_ended")}` : ""}`;
  });
  const vu = (x: number) => Math.min(100, Math.max(0, Math.sqrt(Math.max(0, x)) * 100));
  // Timeshift (lib/timeshift.ts): bei Versatz > 0 statt des Live-Hinweises
  const tsOffset = $derived(s.timeshift.offset_s > 0 ? fmtOffset(s.timeshift.offset_s) : "");
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
    {#if svc && ptyName(svc.pty)}<span class="pty" title={svc.language ? `${t("display.language")}: ${languageName(svc.language)}` : ""}>{ptyName(svc.pty)}</span>{/if}
    <span class="dim">{s.fic_total ? `FIC ${s.fic_ok}/${s.fic_total}` : ""}</span>
    {#if tsOffset}
      <span class="ts" class:paused={s.timeshift.mode === "paused"} title={t("ts.display_tip")}>TIMESHIFT {tsOffset}</span>
    {:else if s.current && s.device?.kind !== "file"}
      <span class="dim">LIVE</span>
    {/if}
    <span class="dim">{fileText}</span>
  </div>
  {#if slide || s.logo_data_url || s.slides.length}
    <div class="media">
      <Logo
        eid={s.ensemble?.eid ?? null}
        sid={s.current?.sid ?? null}
        src={s.logo_data_url}
        name={svc?.name ?? ""}
        size="medium"
        px={s.slides.length ? 96 : 64}
        zoomable
      />
      <!-- Bilderstreifen: bis zu 5 verschiedene Slideshow-Bilder, neuestes rechts (Stefan 16.09.2026). -->
      <div class="strip">
        {#each s.slides as sl, i (sl.data_b64.slice(0, 64) + sl.name)}
          <button type="button" class="slide-btn" class:newest={i === s.slides.length - 1} onclick={() => (zoomIdx = i)} aria-label={sl.name || t("logo.alt")}>
            <img src={slideSrc(sl)} alt={sl.name ?? "slide"} draggable="false" />
          </button>
        {/each}
      </div>
    </div>
  {/if}
  {#if zoomSrc}
    <div class="overlay" onmousedown={(e) => e.target === e.currentTarget && (zoomIdx = -1)} role="presentation">
      <div class="dialog zoom" role="dialog" aria-modal="true" aria-label={s.slides[zoomIdx]?.name || t("logo.alt")}>
        <img src={zoomSrc} alt={s.slides[zoomIdx]?.name ?? "slide"} draggable="false" />
        <div class="zoomnav">
          <button class="btn mini" disabled={zoomIdx <= 0} onclick={() => zoomIdx--}>◀</button>
          <span>{zoomIdx + 1} / {s.slides.length}</span>
          <button class="btn mini" disabled={zoomIdx >= s.slides.length - 1} onclick={() => zoomIdx++}>▶</button>
        </div>
      </div>
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
      <span class="hi">{#each titleSegments as seg, i (i)}{#if seg.url}<a href={seg.url} onclick={(e) => openLink(e, seg.url ?? "")}>{seg.text}</a>{:else}{seg.text}{/if}{/each}</span>
      {#if dlp.artist}<span class="dim"> · </span><span>{#each artistSegments as seg, i (i)}{#if seg.url}<a href={seg.url} onclick={(e) => openLink(e, seg.url ?? "")}>{seg.text}</a>{:else}{seg.text}{/if}{/each}</span>{/if}
      {#if s.dl_plus && !s.dl_plus.item_running}<span class="dim"> (pause)</span>{/if}
    {:else}
      <span class="dim">{s.current ? "DL+ ---" : ""}</span>
    {/if}
  </div>
  <div class="ticker">
    <span class:scroll={s.dls.length > 70}
      >{#if dlsSegments.length}{#each dlsSegments as seg, i (i)}{#if seg.url}<a href={seg.url} onclick={(e) => openLink(e, seg.url ?? "")}>{seg.text}</a>{:else}{seg.text}{/if}{/each}{:else}&nbsp;{/if}</span
    >
  </div>
  <div class="vu">
    <span class="dim">L</span><span class="bar"><i style="width:{vu(s.level[0])}%"></i></span>
    <span class="dim">R</span><span class="bar"><i style="width:{vu(s.level[1])}%"></i></span>
    {#if s.muted}<span class="dim">MUTE</span>{/if}
  </div>
</section>
<svelte:window onkeydown={onSlideKey} />

<style>
  .display { padding: 4px 8px; display: flex; flex-direction: column; gap: 2px; flex: none; }
  .row1 { display: flex; gap: 12px; font-size: 10px; white-space: nowrap; }
  .row1 .ens { flex: 1; overflow: hidden; text-overflow: ellipsis; }
  .tech span { min-width: 56px; }
  .tech .pty { color: #7fd6a0; font-size: 9px; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 110px; }
  .tech .ts { color: var(--green-hi); font-weight: bold; min-width: 120px; }
  .tech .ts.paused { color: var(--amber, #e8b23a); animation: blink 1s steps(2, start) infinite; }
  .name { font-size: 16px; font-weight: bold; color: var(--green-hi); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; padding: 1px 0; }
  .name.pending { color: var(--amber); animation: blink 1s steps(2, start) infinite; }
  .media { display: flex; gap: 6px; align-items: center; min-width: 0; }
  .strip { display: flex; gap: 4px; align-items: center; flex: 1 1 auto; min-width: 0; overflow-x: auto; overflow-y: hidden; justify-content: flex-end; }
  .strip img { height: 128px; max-width: 220px; object-fit: contain; border: 1px solid transparent; }
  .slide-btn { all: unset; cursor: zoom-in; display: inline-flex; line-height: 0; min-width: 0; flex: 0 0 auto; }
  .slide-btn.newest img { border-color: var(--green-dim); }
  .dialog.zoom { padding: 8px; display: flex; flex-direction: column; align-items: center; gap: 6px; }
  .dialog.zoom img { max-width: min(90vw, 640px); max-height: min(80vh, 640px); object-fit: contain; }
  .zoomnav { display: flex; align-items: center; gap: 8px; font-size: 10px; color: var(--text-dim); }
  .dlp a, .ticker a { color: inherit; text-decoration: underline; text-decoration-style: dotted; cursor: pointer; }
  .dlp a:hover, .ticker a:hover { color: var(--green-hi); }
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
