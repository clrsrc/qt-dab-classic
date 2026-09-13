<script lang="ts">
  // Panel "Musik" (Entscheidungen 6, 7; Plan M4b Abschnitt 3): Vorschlagsliste
  // der Titel-Trennung aus dem Timeshift-Puffer. Pro Zeile "uebernehmen"
  // (schneidet den Titel als MP3 mit ID3-Tags aus dem Ring), oben "alle
  // uebernehmen" (nacheinander, weil der Kern nur einen Export gleichzeitig
  // schreibt) und der Schalter "automatisch speichern".
  //
  // Keine echte Wellenform: statt eines weiteren Rings im Kern zeigt die Zeile
  // nur einen Laengenbalken (Plan Abschnitt 3, erste Ausbaustufe).
  import { t, tError } from "$lib/i18n.svelte";
  import {
    ageS,
    durationS,
    effectivePostRoll,
    effectivePreRoll,
    fmtLen,
    label,
    MAX_ROLL_S,
    MIN_ROLL_S,
    musicApi,
    ROLL_STEP_S,
    type TrackCandidate,
  } from "$lib/music";
  import { notify, patchSettings, s, ui } from "$lib/state.svelte";

  let busy = $state(false);
  const enabled = $derived(ui.settings?.music_enabled ?? true);
  const autoSave = $derived(ui.settings?.music_auto_save ?? false);
  const frame = $derived(s.timeshift.frame_index);
  const open = $derived(s.music_candidates.filter((c) => !c.taken).length);
  // Laengenbalken: der laengste Titel der Liste ist die Bezugsgroesse.
  const longest = $derived(Math.max(60, ...s.music_candidates.map((c) => durationS(c) ?? 0)));

  function fileOf(path: string): string {
    return path.split(/[\\/]/).pop() ?? path;
  }

  async function take(index: number) {
    if (busy) return;
    busy = true;
    try {
      const path = await musicApi.exportCandidate(index);
      notify("info", t("music.exported", { file: fileOf(path) }));
    } catch (e) {
      notify("warn", tError(e));
    } finally {
      busy = false;
    }
  }

  /** Alle offenen Vorschlaege nacheinander; der Kern schreibt nur einen Export gleichzeitig. */
  async function takeAll() {
    if (busy) return;
    busy = true;
    try {
      for (let i = 0; i < s.music_candidates.length; i++) {
        if (s.music_candidates[i]?.taken) continue;
        try {
          await musicApi.exportCandidate(i);
        } catch (e) {
          notify("warn", tError(e));
          break;
        }
        await new Promise((r) => setTimeout(r, 1500));
      }
    } finally {
      busy = false;
    }
  }

  async function clear() {
    try {
      await musicApi.clear();
    } catch (e) {
      notify("warn", tError(e));
    }
  }

  function rowTitle(c: TrackCandidate): string {
    const d = durationS(c);
    return `${label(c)} · ${c.station ?? ""} · ${fmtLen(d)}${c.from_dls ? ` · ${t("music.from_dls")}` : ""}`;
  }

  /** +/- Sekunden-Stepper fuer Vor-/Nachlauf (verschiebbare Schnittmarken, Plan M4b Abschnitt 5). */
  async function adjust(index: number, c: TrackCandidate, dPre: number, dPost: number) {
    if (busy) return;
    const preRollS = Math.min(MAX_ROLL_S, Math.max(MIN_ROLL_S, effectivePreRoll(c) + dPre));
    const postRollS = Math.min(MAX_ROLL_S, Math.max(MIN_ROLL_S, effectivePostRoll(c) + dPost));
    try {
      await musicApi.adjust(index, preRollS, postRollS);
    } catch (e) {
      notify("warn", tError(e));
    }
  }
</script>

<section class="panel music">
  <div class="panel-head">
    <span>{t("panel.music")}</span>
    <span class="grow"></span>
    <label class="inl strong" title={t("music.enabled_hint")}>
      <input type="checkbox" checked={enabled} onchange={(e) => patchSettings({ music_enabled: (e.currentTarget as HTMLInputElement).checked })} />
      {t("music.enabled")}
    </label>
    {#if enabled}
      <label class="inl" title={t("music.auto_save_hint")}>
        <input type="checkbox" checked={autoSave} onchange={(e) => patchSettings({ music_auto_save: (e.currentTarget as HTMLInputElement).checked })} />
        {t("music.auto_save")}
      </label>
      <button class="btn mini" disabled={busy || !open} onclick={takeAll}>{t("music.take_all")}</button>
      <button class="btn mini" disabled={!s.music_candidates.length} onclick={clear}>{t("music.clear")}</button>
    {/if}
  </div>
  <div class="list">
    {#if !enabled}
      <div class="row"><span class="grow dim">{t("music.disabled")}</span></div>
    {:else if !s.music_candidates.length}
      <div class="row"><span class="grow dim">{autoSave ? t("music.empty_auto") : t("music.empty")}</span></div>
    {/if}
    {#each enabled ? s.music_candidates : [] as c, i (`${c.start_frame}-${i}`)}
      {@const dur = durationS(c)}
      <div class="row" class:taken={c.taken} title={rowTitle(c)}>
        <span class="bar" aria-hidden="true"><i style="width:{Math.round(Math.min(1, (dur ?? 0) / longest) * 100)}%"></i></span>
        <span class="grow">
          <span class="ttl">{c.title ?? t("music.unknown")}</span>{#if c.artist}<span class="dim"> · </span><span class="art">{c.artist}</span>{/if}
        </span>
        {#if c.from_dls}<span class="flag" title={t("music.from_dls")}>DLS</span>{/if}
        <span class="svc">{c.station ?? ""}</span>
        <span class="len">{fmtLen(dur)}</span>
        <span class="ago">{t("music.ago", { time: fmtLen(ageS(c, frame)) })}</span>
        {#if !c.taken}
          <span class="roll" title={t("music.pre_roll_hint")}>
            <button class="btn mini step" disabled={busy} onclick={() => adjust(i, c, -ROLL_STEP_S, 0)}>−</button>
            <span class="roll-val">{t("music.pre_roll", { value: effectivePreRoll(c) })}</span>
            <button class="btn mini step" disabled={busy} onclick={() => adjust(i, c, ROLL_STEP_S, 0)}>+</button>
          </span>
          <span class="roll" title={t("music.post_roll_hint")}>
            <button class="btn mini step" disabled={busy} onclick={() => adjust(i, c, 0, -ROLL_STEP_S)}>−</button>
            <span class="roll-val">{t("music.post_roll", { value: effectivePostRoll(c) })}</span>
            <button class="btn mini step" disabled={busy} onclick={() => adjust(i, c, 0, ROLL_STEP_S)}>+</button>
          </span>
        {/if}
        {#if c.taken}
          <span class="done" title={t("music.taken")}>✓</span>
        {:else}
          <button class="btn mini" disabled={busy} onclick={() => take(i)}>{t("music.take")}</button>
        {/if}
      </div>
    {/each}
  </div>
  {#if enabled}<div class="foot">{t("music.hint")}</div>{/if}
</section>

<style>
  .music { display: flex; flex-direction: column; flex: 1 1 120px; min-height: 80px; }
  .list { flex: 1; margin: 3px 4px; }
  .row { gap: 5px; padding: 1px 4px; font-size: 10px; }
  .row.taken { color: #2f6a3a; }
  .bar { width: 42px; height: 6px; background: #0a0e14; border: 1px inset #2a2e36; flex: none; }
  .bar i { display: block; height: 100%; background: linear-gradient(90deg, #003810, #00a030); }
  .ttl { font-weight: bold; }
  .art { color: var(--green); }
  .flag { font-size: 8px; font-family: var(--mono); border: 1px solid currentColor; padding: 0 2px; color: var(--amber); flex: none; }
  .svc { width: 74px; flex: none; color: var(--text-dim); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .len { width: 38px; font-family: var(--mono); text-align: right; flex: none; }
  .ago { width: 62px; font-family: var(--mono); text-align: right; flex: none; color: var(--text-dim); }
  .done { width: 40px; text-align: center; flex: none; color: var(--green); }
  .roll { display: inline-flex; align-items: center; gap: 2px; flex: none; }
  .roll-val { width: 52px; font-family: var(--mono); text-align: center; color: var(--text-dim); }
  .step { width: 14px; padding: 0; line-height: 1; }
  .dim { color: var(--green-dim); }
  .mini { font-size: 9px; min-height: 14px; padding: 0 4px; }
  .inl { display: inline-flex; align-items: center; gap: 3px; font-weight: normal; letter-spacing: 0; text-transform: none; }
  .inl.strong { color: var(--text, #e8e8e8); font-weight: bold; }
  .foot { font-size: 9px; color: var(--text-dim); padding: 1px 8px 3px; }
  input[type="checkbox"] { margin: 0; }
</style>
