<script lang="ts">
  // Debug-Panel (Entscheidung 25, Hotkey D): Spektrum, Konstellation,
  // SNR-Verlauf, TII-Liste mit Sendestandort, Fehlerzaehler FIC/RS/AAC,
  // Frequenzversatz und Gain. Der Kern liefert Spektrum/IQ nur, solange das
  // Panel offen ist (settings.panels.debug -> set_scopes); Zaehler kommen
  // 1 Hz per `debug_stats`. Sichtbarkeit wie die anderen Panels (+page).
  import { onMount } from "svelte";
  import { debugApi, scopeCounters, scopeIdle } from "$lib/debug";
  import { t, tError } from "$lib/i18n.svelte";
  import { notify, s, ui } from "$lib/state.svelte";
  import IqScope from "./IqScope.svelte";
  import SnrHistory from "./SnrHistory.svelte";
  import SpectrumScope from "./SpectrumScope.svelte";
  import TiiList from "./TiiList.svelte";

  let peakHold = $state(true);
  /** Frames, die zwischen Schliessen und erneutem Oeffnen ankamen (Beleg: 0). */
  let idleFrames = $state<number | null>(null);
  const frames = $derived.by(() => {
    void ui.now;
    return { spectrum: scopeCounters.spectrum, iq: scopeCounters.iq };
  });
  const ficPct = $derived(s.fic_total ? Math.round((100 * s.fic_ok) / s.fic_total) : 0);
  const p = $derived(s.debug.stats_primary);
  const rate = $derived(ui.settings?.scope_rate_hz ?? 5);
  const run = (pr: Promise<unknown>) => pr.catch((e) => notify("warn", tError(e)));
  const cnt = (now: number | undefined, total: number | undefined) => (now == null ? "–" : `${now}/s · Σ ${total ?? 0}`);

  onMount(() => {
    if (scopeIdle.closedAt) idleFrames = scopeCounters.spectrum - scopeIdle.closedAt.spectrum + (scopeCounters.iq - scopeIdle.closedAt.iq);
    // Zaehler/Verlauf sofort holen (das naechste debug_stats kommt erst nach <= 1 s)
    debugApi
      .state()
      .then((d) => {
        s.debug = d;
      })
      .catch(() => {});
    return () => {
      scopeIdle.closedAt = { spectrum: scopeCounters.spectrum, iq: scopeCounters.iq };
    };
  });
</script>

<section class="panel debug">
  <div class="panel-head">
    <span>{t("panel.debug")}</span>
    <span class="grow"></span>
    <label class="k">{t("debug.rate")}
      <select value={rate} onchange={(e) => run(debugApi.setRate(Number((e.target as HTMLSelectElement).value)))}>
        {#each [1, 2, 3, 5, 8, 10] as r (r)}<option value={r}>{r} Hz</option>{/each}
      </select>
    </label>
    <label class="k"><input type="checkbox" bind:checked={peakHold} />{t("debug.peak_hold")}</label>
    <span class="k mono" title={t("debug.frames_hint")}>{t("debug.frames", { s: frames.spectrum, iq: frames.iq })}</span>
    {#if idleFrames != null}<span class="k mono" class:warn={idleFrames > 2}>{t("debug.idle_frames", { n: idleFrames })}</span>{/if}
  </div>
  <div class="body">
    <div class="rowx">
      <div class="box grow">
        <span class="cap">{t("debug.spectrum")}</span>
        <SpectrumScope {peakHold} />
      </div>
      <div class="box">
        <span class="cap">{t("debug.iq")}</span>
        <IqScope />
      </div>
    </div>
    <div class="rowx">
      <div class="box grow">
        <span class="cap">{t("debug.snr")}</span>
        <SnrHistory />
      </div>
      <div class="box counters">
        <span class="cap">{t("debug.counters")}</span>
        <div class="grid">
          <span class="lbl">{t("debug.fic")}</span><span class="mono">{s.fic_ok}/{s.fic_total} · {ficPct} %</span>
          <span class="lbl">{t("debug.frame_errors")}</span><span class="mono">{cnt(p?.frame_errors, p?.total_frame_errors)}</span>
          <span class="lbl">{t("debug.rs_errors")}</span><span class="mono">{cnt(p?.rs_errors, p?.total_rs_errors)}</span>
          <span class="lbl">{t("debug.aac_errors")}</span><span class="mono">{cnt(p?.aac_errors, p?.total_aac_errors)}</span>
          <span class="lbl">{t("debug.rs_corrections")}</span><span class="mono">{cnt(p?.rs_corrections, p?.total_rs_corrections)}</span>
          <span class="lbl">{t("debug.freq_offset")}</span><span class="mono">{s.debug.freq_offset_hz} Hz</span>
          <span class="lbl">{t("debug.gain")}</span>
          <span class="mono">LNA {s.gain.lna} · VGA {s.gain.vga} · AMP {s.gain.amp ? t("debug.on") : t("debug.off")} · {s.agc ? "AGC" : t("debug.manual")}</span>
          <span class="lbl">SNR</span><span class="mono">{s.synced ? `${s.snr.toFixed(1)} dB` : t("debug.no_sync")}</span>
        </div>
      </div>
    </div>
    <div class="box">
      <span class="cap">
        {t("tii.title")}
        {#if s.debug.tii_db_entries}<span class="k">· {t("tii.db", { n: s.debug.tii_db_entries })}</span>{:else}<span class="k warn">· {t("tii.db_missing")}</span>{/if}
        {#if ui.settings?.tii_dx_mode}<span class="k amber">· DX</span>{/if}
      </span>
      <TiiList />
    </div>
  </div>
</section>

<style>
  .debug { flex: none; display: flex; flex-direction: column; }
  .body { display: flex; flex-direction: column; gap: 3px; padding: 3px 4px 4px; }
  .rowx { display: flex; gap: 4px; align-items: stretch; }
  .box { display: flex; flex-direction: column; gap: 1px; min-width: 0; }
  .box.grow { flex: 1; }
  .cap { font-size: 9px; color: #7080a0; font-weight: bold; letter-spacing: 0.05em; text-transform: uppercase; }
  .counters { width: 250px; }
  .grid { display: grid; grid-template-columns: max-content 1fr; gap: 1px 8px; font-size: 10px; background: var(--list); border: 1px inset #2a2e36; padding: 2px 4px; flex: 1; }
  .grid .lbl { color: #2f8a4a; }
  .mono { font-family: var(--mono); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .k { color: var(--text-dim); font-size: 9px; font-weight: normal; letter-spacing: 0; text-transform: none; display: inline-flex; align-items: center; gap: 3px; }
  .k select { min-height: 14px; font-size: 9px; padding: 0 2px; }
  .k input[type="checkbox"] { margin: 0; }
  .warn { color: var(--red); }
  .amber { color: var(--amber); }
</style>
