<script lang="ts">
  // REC-Knopf im Transport: rot mit Laufzeit waehrend der Aufnahme, Tooltip
  // mit Pfad; Taste R macht dasselbe (hotkeys.ts -> recording.ts).
  import { t } from "$lib/i18n.svelte";
  import { toggleRecording } from "$lib/recording";
  import { s } from "$lib/state.svelte";
  import { fmtClock } from "$lib/timers";
  import { tm } from "$lib/timers.svelte";

  const on = $derived(s.recording || !!tm.recording?.active);
  const tip = $derived(on ? t("rec.tip_active", { path: tm.recording?.path ?? "" }) : t("rec.start"));
</script>

<button class="btn feat rec" class:on class:blink={on} title={tip} onclick={toggleRecording} disabled={!on && !s.current}>
  {#if on}<span class="dot">●</span> {fmtClock(tm.recording?.seconds ?? 0)}{:else}REC{/if}
</button>

<style>
  .feat { font-size: 8px; min-width: 30px; padding: 1px 3px; font-family: var(--mono); }
  .rec.on { min-width: 52px; }
  .dot { animation: blink 1s steps(2, start) infinite; }
</style>
