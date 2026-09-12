<script lang="ts">
  // Detail einer Sendung: langer Name, Zeit, Beschreibung, Genres und die
  // Aktionen "Umschalten" (sofort), "Aufnehmen" / "Umschalten (Timer)" ueber
  // das Timer-Modul (Kommando `timer_add_from_epg`, Vertrag in epg.ts).
  import { api } from "$lib/core";
  import { epgApi, fmtClock, fmtDuration, progressOf, remainingMin, type Programme, type TimerFromEpgRequest } from "$lib/epg";
  import { t, tError } from "$lib/i18n.svelte";
  import { notify, s, ui } from "$lib/state.svelte";

  let { programme, serviceName, sid, onclose }: { programme: Programme; serviceName: string; sid: number; onclose: () => void } = $props();

  let busy = $state(false);
  let error = $state<string | null>(null);

  const p = $derived(programme);
  const running = $derived(progressOf(p.start_unix, p.duration_min, ui.now) !== null);
  const past = $derived(p.start_unix * 1000 + p.duration_min * 60000 <= ui.now);
  const isCurrent = $derived(!!s.current && s.current.sid === sid);

  function request(kind: TimerFromEpgRequest["kind"]): TimerFromEpgRequest {
    return {
      channel: s.channel ?? s.ensemble?.channel ?? "",
      eid: s.ensemble?.eid ?? 0,
      sid,
      service: serviceName,
      title: p.long_name || p.medium_name,
      start_unix: p.start_unix,
      duration_s: p.duration_min * 60,
      kind,
    };
  }

  async function addTimer(kind: TimerFromEpgRequest["kind"]) {
    busy = true;
    error = null;
    try {
      const id = await epgApi.timerAddFromEpg(request(kind));
      notify("info", t("epg.timer_added", { id, title: p.long_name || p.medium_name }));
    } catch (e) {
      const msg = typeof e === "string" ? e : e instanceof Error ? e.message : String(e);
      error = /not found|unknown|kein.*(Handler|command)|Command .* not found/i.test(msg) ? t("epg.timer_unavailable") : tError(msg);
    } finally {
      busy = false;
    }
  }

  async function switchNow() {
    try {
      await api.selectService(sid, 0);
    } catch (e) {
      error = tError(e);
    }
  }
</script>

<div class="detail lcd">
  <div class="head">
    <span class="hi title">{p.long_name || p.medium_name}</span>
    <button class="btn mini" onclick={onclose} title={t("epg.close")}>×</button>
  </div>
  <div class="meta">
    <span>{fmtClock(p.start_unix)}–{fmtClock(p.start_unix + p.duration_min * 60)}</span>
    <span class="dim">· {fmtDuration(p.duration_min)}</span>
    {#if running}<span class="run">· {t("epg.running")} · {t("epg.remaining", { min: remainingMin(p.start_unix, p.duration_min, ui.now) })}</span>{/if}
    {#if p.legacy_time}<span class="dim" title={t("epg.legacy_hint")}> · UTC+2′</span>{/if}
  </div>
  {#if p.long_name && p.medium_name && p.long_name !== p.medium_name}<div class="dim">{p.medium_name}</div>{/if}
  {#if p.long_desc || p.short_desc}
    <div class="desc">{p.long_desc || p.short_desc}</div>
  {/if}
  {#if p.genres.length}
    <div class="dim genres">{t("epg.genres")}: {p.genres.join(", ")}</div>
  {/if}
  <div class="actions">
    <button class="btn" disabled={isCurrent || s.recording} onclick={switchNow}>{t("epg.switch")}</button>
    <button class="btn" disabled={busy || past} onclick={() => addTimer("record")}>{t("epg.record")}</button>
    <button class="btn" disabled={busy || past || running} onclick={() => addTimer("switch")}>{t("epg.switch_timer")}</button>
  </div>
</div>

{#if error}
  <div class="overlay" role="presentation" onmousedown={(e) => e.target === e.currentTarget && (error = null)}>
    <div class="dialog" role="dialog" aria-modal="true">
      <h3>{t("epg.error_title")}</h3>
      <p>{error}</p>
      <div class="actions">
        <button class="btn on" onclick={() => (error = null)}>{t("modal.ok")}</button>
      </div>
    </div>
  </div>
{/if}

<style>
  .detail { padding: 4px 6px; margin: 2px 4px 3px; font-size: 10px; display: flex; flex-direction: column; gap: 3px; flex: none; }
  .head { display: flex; align-items: flex-start; gap: 6px; }
  .title { flex: 1; font-size: 12px; font-weight: bold; }
  .meta { display: flex; gap: 4px; flex-wrap: wrap; }
  .run { color: var(--amber); }
  .desc { white-space: pre-wrap; max-height: 96px; overflow: auto; color: var(--green); line-height: 1.35; user-select: text; }
  .genres { font-size: 9px; }
  .actions { display: flex; gap: 4px; justify-content: flex-end; padding-top: 2px; }
  .mini { font-size: 10px; min-height: 14px; padding: 0 5px; }
</style>
