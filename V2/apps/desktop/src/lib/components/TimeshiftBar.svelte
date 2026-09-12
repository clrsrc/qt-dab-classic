<script lang="ts">
  // Timeshift-Leiste unter dem Transport (Plan M4 Abschnitt 3, Entscheidung 4):
  // Pufferbalken mit Lesezeiger-Marke, Versatzanzeige "−1:23" bzw. "LIVE",
  // Knoepfe ⏸/▶, −30 s, +30 s, LIVE, Klick/Drag auf den Balken = seek und
  // "letzte n min sichern" (Export aus dem Ring als WAV).
  //
  // Sichtbar, solange ein Audiodienst laeuft und die Quelle keine Datei ist
  // (bei Datei-Wiedergabe spult man die Datei selbst). Mit der Umgebungs-
  // variablen DABCLASSIC_TS_DEMO=1 zeigt die Rust-Seite einen Demo-Zustand,
  // dann ist die Leiste auch ohne Kern-Funktion sichtbar (Sichtpruefung).
  import { t, tError } from "$lib/i18n.svelte";
  import { notify, s } from "$lib/state.svelte";
  import { fmtOffset, fmtSpan, readerClock, timeshiftApi, SKIP_STEP_S } from "$lib/timeshift";

  const ts = $derived(s.timeshift);
  const visible = $derived(ts.demo || (!!s.current && s.device?.kind !== "file"));
  const paused = $derived(ts.mode === "paused");
  const isLive = $derived(ts.mode === "live" && ts.offset_s <= 0);
  // Massstab: eingestellte Kapazitaet, mindestens der bereits gefuellte Teil.
  const scale = $derived(Math.max(ts.capacity_s, ts.buffered_s, 60));
  const fillPct = $derived(Math.min(100, (ts.buffered_s / scale) * 100));
  // Balken laeuft von links (aeltester Rahmen) nach rechts (live).
  const headPct = $derived(Math.min(100, Math.max(0, (1 - ts.offset_s / scale) * 100)));
  const clock = $derived(readerClock(ts));

  let bar: HTMLDivElement | null = $state(null);
  let dragging = $state(false);
  let exportMin = $state(5);

  const run = (p: Promise<unknown>) => p.catch((e) => notify("warn", tError(e)));

  /** Mausposition auf dem Balken -> Versatz hinter live. */
  function offsetAt(clientX: number): number {
    if (!bar) return 0;
    const r = bar.getBoundingClientRect();
    if (r.width <= 0) return 0;
    const x = Math.min(1, Math.max(0, (clientX - r.left) / r.width));
    return Math.min(ts.buffered_s, (1 - x) * scale);
  }

  function seekTo(clientX: number) {
    void run(timeshiftApi.seek(offsetAt(clientX)));
  }

  function onPointerDown(ev: PointerEvent) {
    if (ts.buffered_s <= 0) return;
    dragging = true;
    (ev.currentTarget as HTMLElement).setPointerCapture(ev.pointerId);
    seekTo(ev.clientX);
  }

  function onPointerMove(ev: PointerEvent) {
    if (dragging) seekTo(ev.clientX);
  }

  function onPointerUp(ev: PointerEvent) {
    if (!dragging) return;
    dragging = false;
    (ev.currentTarget as HTMLElement).releasePointerCapture(ev.pointerId);
  }

  async function doExport() {
    const minutes = Math.max(1, Math.min(240, Math.round(exportMin)));
    try {
      const path = await timeshiftApi.exportRange(minutes * 60, 0);
      notify("info", t("ts.export_started", { min: minutes, file: path.split(/[\\/]/).pop() ?? path }), 8000);
    } catch (e) {
      notify("warn", tError(e));
    }
  }
</script>

{#if visible}
  <section class="timeshift">
    <button class="btn ts" class:on={paused} title={paused ? t("ts.play") : t("ts.pause")} onclick={() => run(timeshiftApi.pauseToggle())}>
      {paused ? "▶" : "⏸"}
    </button>
    <button class="btn ts" title={t("ts.back")} onclick={() => run(timeshiftApi.skip(-SKIP_STEP_S))}>−30</button>
    <button class="btn ts" title={t("ts.fwd")} onclick={() => run(timeshiftApi.skip(SKIP_STEP_S))}>+30</button>
    <button class="btn ts live" class:on={isLive} title={t("ts.live_tip")} onclick={() => run(timeshiftApi.live())}>LIVE</button>

    <div
      class="bar"
      bind:this={bar}
      role="slider"
      tabindex="-1"
      aria-label={t("ts.bar")}
      aria-valuemin={0}
      aria-valuemax={Math.round(ts.buffered_s)}
      aria-valuenow={Math.round(ts.buffered_s - ts.offset_s)}
      title={t("ts.bar_tip", { buffered: fmtSpan(ts.buffered_s), capacity: fmtSpan(ts.capacity_s) })}
      onpointerdown={onPointerDown}
      onpointermove={onPointerMove}
      onpointerup={onPointerUp}
      onpointercancel={onPointerUp}
    >
      <i class="fill" style="width:{fillPct}%; left:{100 - fillPct}%"></i>
      <i class="head" class:paused style="left:{headPct}%"></i>
    </div>

    <span class="off" class:live={isLive} class:paused>{fmtOffset(ts.offset_s)}</span>
    <span class="buf" title={t("ts.buffered")}>{fmtSpan(ts.buffered_s)}{clock ? ` · ${clock}` : ""}</span>

    <span class="sep"></span>
    <input class="min" type="number" min="1" max="240" bind:value={exportMin} title={t("ts.export_min")} />
    <button class="btn ts exp" title={t("ts.export_tip")} onclick={doExport} disabled={ts.buffered_s <= 0}>{t("ts.export")}</button>
  </section>
{/if}

<style>
  .timeshift { display: flex; align-items: center; gap: 3px; padding: 2px 6px 4px; flex: none; }
  .btn.ts { font-size: 9px; min-width: 26px; padding: 1px 4px; font-family: var(--mono); }
  .btn.ts.live { min-width: 32px; }
  .btn.ts.exp { min-width: 0; font-size: 8px; }
  .sep { width: 6px; }
  .bar {
    position: relative; flex: 1; min-width: 80px; height: 10px; margin: 0 4px;
    background: #0a1a0e; border: 1px solid #1f3a26; cursor: pointer; overflow: hidden;
  }
  .bar .fill { position: absolute; top: 0; bottom: 0; background: #12301b; }
  .bar .head { position: absolute; top: -1px; bottom: -1px; width: 3px; margin-left: -1px; background: var(--green-hi, #6f6); }
  .bar .head.paused { background: var(--amber, #e8b23a); animation: blink 1s steps(2, start) infinite; }
  .off { font-family: var(--mono); font-size: 10px; min-width: 46px; text-align: right; color: var(--green); }
  .off.live { color: #7080a0; }
  .off.paused { color: var(--amber, #e8b23a); }
  .buf { font-family: var(--mono); font-size: 9px; color: var(--text-dim, #7080a0); white-space: nowrap; }
  .min { width: 38px; font-size: 9px; }
  button:disabled { opacity: 0.45; }
</style>
