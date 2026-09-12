<script lang="ts">
  // Timer-Panel (Entscheidung 9/10): Liste wie v1 (Typ, Dienst, Datum/Zeit,
  // Dauer, Titel, Status), aktiv-Schalter, Bearbeiten, Loeschen, Neu.
  import { t } from "$lib/i18n.svelte";
  import { ui } from "$lib/state.svelte";
  import { deleteTimer, fmtDateTime, fmtDuration, newTimer, timerLabel, timerStatusKey, toggleTimer, type Timer } from "$lib/timers";
  import { nextTimer, tm } from "$lib/timers.svelte";
  import TimerEditor from "./TimerEditor.svelte";

  let editing = $state<{ timer: Timer; isNew: boolean } | null>(null);
  const next = $derived(nextTimer(ui.now));
  const pre = $derived(Math.round((ui.settings?.record_pre_s ?? 120) / 60));
  const post = $derived(Math.round((ui.settings?.record_post_s ?? 300) / 60));

  function add() {
    editing = { timer: newTimer(), isNew: true };
  }
  function edit(x: Timer) {
    editing = { timer: { ...x }, isNew: false };
  }
</script>

<section class="panel timers">
  <div class="panel-head">
    <span>{t("panel.timer")}</span>
    <span class="grow"></span>
    <span class="info" title={t("timer.pre_post")}>{tm.timers.length ? t("timer.count", { n: tm.timers.length }) : ""} · {pre}/{post} min</span>
    <button class="btn mini" onclick={add}>{t("timer.new")}</button>
  </div>
  {#if editing}
    <TimerEditor timer={editing.timer} isNew={editing.isNew} onclose={() => (editing = null)} />
  {/if}
  <div class="list">
    {#if !tm.timers.length}
      <div class="row"><span class="grow dim">{t("timer.empty")}</span></div>
    {/if}
    {#each tm.timers as x (x.id)}
      <div class="row" class:active={x.fired && x.active} class:off={!x.active} class:next={next?.id === x.id} title={`${t(`timer.kind.${x.type}`)}: ${x.service} @ ${fmtDateTime(x.start_unix)} (${x.channel || "–"})`}>
        <span class="kind k-{x.type}">{t(`timer.kind_short.${x.type}`)}</span>
        <span class="when">{fmtDateTime(x.start_unix)}</span>
        <span class="dur">{fmtDuration(x.duration_s)}</span>
        <span class="grow">
          <span class="svc">{x.service}</span>{#if x.title}<span class="dim"> · </span>{x.title}{/if}
        </span>
        <span class="st" class:run={x.fired && x.active}>{t(timerStatusKey(x))}</span>
        <input type="checkbox" checked={x.active} title={t("timer.status.waiting")} onchange={() => toggleTimer(x)} />
        <button class="btn mini" title={t("timer.edit")} onclick={() => edit(x)}>✎</button>
        <button class="btn mini del" title={t("timer.delete")} onclick={() => deleteTimer(x)}>✕</button>
      </div>
    {/each}
  </div>
  {#if next}
    <div class="foot">{t("timer.next", { name: timerLabel(next), time: fmtDateTime(next.start_unix) })}</div>
  {/if}
</section>

<style>
  .timers { display: flex; flex-direction: column; flex: 1 1 120px; min-height: 80px; }
  .info { font-weight: normal; letter-spacing: 0; text-transform: none; color: var(--text-dim); }
  .list { flex: 1; margin: 3px 4px; }
  .row { gap: 5px; padding: 1px 4px; font-size: 10px; }
  .row.off { color: #2f6a3a; }
  .row.next .when { color: var(--amber); }
  .kind { width: 44px; font-size: 8px; font-weight: bold; font-family: var(--mono); text-align: center; border: 1px solid currentColor; padding: 0 2px; flex: none; }
  .k-manual_switch { color: #00cc00; }
  .k-manual_record { color: #ff4040; }
  .k-epg_switch { color: #33aaee; }
  .k-epg_record { color: #ff9933; }
  .when { width: 96px; font-family: var(--mono); flex: none; }
  .dur { width: 42px; font-family: var(--mono); text-align: right; flex: none; }
  .svc { font-weight: bold; }
  .st { width: 48px; font-size: 9px; text-align: right; flex: none; color: var(--text-dim); }
  .st.run { color: var(--red); font-weight: bold; }
  .dim { color: var(--green-dim); }
  .mini { font-size: 9px; min-height: 14px; padding: 0 4px; }
  .del:hover { color: var(--red); }
  .foot { font-size: 9px; color: var(--text-dim); padding: 1px 8px 3px; }
  input[type="checkbox"] { margin: 0; }
</style>
