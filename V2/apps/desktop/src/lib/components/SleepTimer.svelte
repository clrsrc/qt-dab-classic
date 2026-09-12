<script lang="ts">
  // Sleep-Timer (Entscheidung 18): Minuten frei, Aktion stumm oder beenden,
  // Restzeit, Abbrechen. Sitzt im Einstellungs-Panel.
  import { api } from "$lib/core";
  import { t, tError } from "$lib/i18n.svelte";
  import { notify, ui } from "$lib/state.svelte";
  import { fmtClock, type SleepAction } from "$lib/timers";
  import { sleepRemaining, tm } from "$lib/timers.svelte";

  let minutes = $state(30);
  let action = $state<SleepAction>("mute");
  const remaining = $derived(sleepRemaining(ui.now));

  async function start() {
    try {
      await api.sleepSet(Math.round(minutes), action);
      notify("info", t("sleep.set", { n: Math.round(minutes), action: t(`sleep.${action}`) }));
    } catch (e) {
      notify("warn", tError(e));
    }
  }
  async function cancel() {
    try {
      await api.sleepCancel();
    } catch (e) {
      notify("warn", tError(e));
    }
  }
</script>

<span class="sleep">
  {#if tm.sleep}
    <span class="rem">{fmtClock(remaining)}</span>
    <span class="k">→ {t(`sleep.${tm.sleep.action}`)}</span>
    <button class="btn mini" onclick={cancel}>{t("sleep.cancel")}</button>
  {:else}
    <input type="number" class="min" bind:value={minutes} min="1" max="1440" onkeydown={(e) => e.key === "Enter" && start()} />
    <span class="k">{t("sleep.minutes")}</span>
    <select bind:value={action}>
      <option value="mute">{t("sleep.mute")}</option>
      <option value="quit">{t("sleep.quit")}</option>
    </select>
    <button class="btn mini" onclick={start} disabled={!(minutes >= 1)}>{t("sleep.start")}</button>
  {/if}
</span>

<style>
  .sleep { display: inline-flex; align-items: center; gap: 4px; flex-wrap: wrap; }
  .min { width: 52px; }
  .rem { font-family: var(--mono); color: var(--amber); font-weight: bold; }
  .k { color: var(--text-dim); font-size: 9px; }
  .mini { font-size: 9px; min-height: 14px; padding: 0 5px; }
</style>
