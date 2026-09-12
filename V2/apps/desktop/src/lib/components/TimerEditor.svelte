<script lang="ts">
  // Timer anlegen/aendern (v1 unified-timer-widget): Typ, Dienst aus dem
  // aktuellen Ensemble oder den Speichern (oder frei), Start, Dauer, Titel.
  import { t } from "$lib/i18n.svelte";
  import { s, ui } from "$lib/state.svelte";
  import { fmtDateTime, fromLocalInput, isRecordKind, saveTimer, toLocalInput, type Timer, type TimerKind } from "$lib/timers";

  let { timer, isNew, onclose }: { timer: Timer; isNew: boolean; onclose: () => void } = $props();

  interface Choice {
    key: string;
    label: string;
    channel: string;
    eid: number;
    sid: number;
    scids: number;
    name: string;
  }
  const choices = $derived.by((): Choice[] => {
    const out: Choice[] = [];
    const ch = s.channel ?? "";
    const eid = s.ensemble?.eid ?? 0;
    for (const svc of s.services.filter((x) => x.is_audio)) {
      out.push({ key: `e:${svc.sid}:${svc.scids}`, label: `${svc.name.trim()} (${ch})`, channel: ch, eid, sid: svc.sid, scids: svc.scids, name: svc.name.trim() });
    }
    ui.presets.slots.forEach((p, i) => {
      if (p && !out.some((c) => c.sid === p.sid && c.channel.toUpperCase() === p.channel.toUpperCase() && p.sid !== 0)) {
        out.push({ key: `p:${i}`, label: `${i === 9 ? 0 : i + 1}: ${p.name} (${p.channel})`, channel: p.channel, eid: p.eid, sid: p.sid, scids: p.scids, name: p.name });
      }
    });
    return out;
  });

  // Der Editor arbeitet auf einer Momentaufnahme; das Panel erzeugt ihn je Timer neu.
  // svelte-ignore state_referenced_locally
  const init = { timer: { ...timer }, isNew };
  const kinds: TimerKind[] = init.isNew || init.timer.type.startsWith("manual") ? ["manual_switch", "manual_record"] : [init.timer.type];
  let kind = $state<TimerKind>(init.timer.type);
  let choice = $state("");
  let customName = $state(init.timer.service);
  let customChannel = $state(init.timer.channel);
  let start = $state(toLocalInput(init.timer.start_unix));
  let minutes = $state(Math.round(init.timer.duration_s / 60));
  let title = $state(init.timer.title);
  let busy = $state(false);

  // Vorbelegung: passender Eintrag der Liste, sonst frei; neu: aktueller Dienst.
  $effect(() => {
    if (choice) return;
    const list = choices;
    if (!isNew || timer.service) {
      const hit = list.find((c) => (timer.sid && c.sid === timer.sid && c.channel.toUpperCase() === timer.channel.toUpperCase()) || (!timer.sid && c.name.toLowerCase() === timer.service.toLowerCase()));
      choice = hit ? hit.key : "custom";
    } else if (s.current) {
      const hit = list.find((c) => c.sid === s.current!.sid && c.scids === s.current!.scids);
      choice = hit ? hit.key : list[0]?.key ?? "custom";
    } else {
      choice = list[0]?.key ?? "custom";
    }
  });

  const startUnix = $derived(fromLocalInput(start));
  const endText = $derived(minutes > 0 && Number.isFinite(startUnix) ? fmtDateTime(startUnix + minutes * 60) : "-");

  async function save() {
    const c = choices.find((x) => x.key === choice);
    const out: Timer = {
      ...timer,
      type: kind,
      channel: c ? c.channel : customChannel.trim().toUpperCase(),
      eid: c ? c.eid : 0,
      sid: c ? c.sid : 0,
      scids: c ? c.scids : 0,
      service: c ? c.name : customName.trim(),
      title: title.trim(),
      start_unix: Number.isFinite(startUnix) ? startUnix : 0,
      duration_s: Math.max(0, Math.round(minutes)) * 60,
      active: true,
      fired: isNew ? false : timer.fired,
    };
    busy = true;
    const ok = await saveTimer(out, isNew);
    busy = false;
    if (ok) onclose();
  }
  function onKey(e: KeyboardEvent) {
    if (e.key === "Escape") onclose();
    if (e.key === "Enter" && (e.target as HTMLElement)?.tagName !== "BUTTON") void save();
    e.stopPropagation();
  }
</script>

<!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
<div class="editor" role="form" onkeydown={onKey}>
  <span class="lbl">{t("timer.field.type")}</span>
  <select bind:value={kind}>
    {#each kinds as k (k)}<option value={k}>{t(`timer.kind.${k}`)}</option>{/each}
  </select>

  <span class="lbl">{t("timer.field.service")}</span>
  <span class="svc">
    <select bind:value={choice}>
      {#if choices.some((c) => c.key.startsWith("e:"))}
        <optgroup label={t("timer.service_current")}>
          {#each choices.filter((c) => c.key.startsWith("e:")) as c (c.key)}<option value={c.key}>{c.label}</option>{/each}
        </optgroup>
      {/if}
      {#if choices.some((c) => c.key.startsWith("p:"))}
        <optgroup label={t("timer.service_presets")}>
          {#each choices.filter((c) => c.key.startsWith("p:")) as c (c.key)}<option value={c.key}>{c.label}</option>{/each}
        </optgroup>
      {/if}
      <option value="custom">{t("timer.service_custom")}</option>
    </select>
    {#if choice === "custom"}
      <input type="text" class="name" bind:value={customName} placeholder={t("timer.service_name")} />
      <input type="text" class="ch" bind:value={customChannel} placeholder={t("timer.field.channel")} maxlength="3" />
    {/if}
  </span>

  <span class="lbl">{t("timer.field.start")}</span>
  <span class="svc"><input type="datetime-local" bind:value={start} /><span class="k">{t("timer.field.end")}: {endText}</span></span>

  <span class="lbl">{t("timer.field.duration")}</span>
  <span class="svc"><input type="number" class="min" bind:value={minutes} min="0" max="1440" /><span class="k">{isRecordKind(kind) ? t("timer.duration_hint_record") : t("timer.duration_hint_switch")}</span></span>

  <span class="lbl">{t("timer.field.title")}</span>
  <input type="text" bind:value={title} placeholder={t("timer.title_placeholder")} />

  <span></span>
  <span class="actions">
    <button class="btn" onclick={onclose}>{t("modal.cancel")}</button>
    <button class="btn on" onclick={save} disabled={busy || (choice === "custom" && !customName.trim())}>{t("timer.save")}</button>
  </span>
</div>

<style>
  .editor { display: grid; grid-template-columns: max-content 1fr; gap: 3px 8px; align-items: center; padding: 4px 6px; font-size: 10px; background: #1a1e28; border-bottom: 1px solid var(--chrome-lo); }
  .lbl { color: #7080a0; font-weight: bold; }
  .svc { display: flex; align-items: center; gap: 4px; flex-wrap: wrap; }
  .svc select { max-width: 220px; }
  .name { width: 120px; }
  .ch { width: 40px; }
  .min { width: 56px; }
  .k { color: var(--text-dim); font-size: 9px; }
  .actions { display: flex; justify-content: flex-end; gap: 6px; }
  input[type="datetime-local"] { background: var(--list); color: var(--green); border: 1px inset #2a2e36; font-size: 10px; padding: 1px 3px; color-scheme: dark; }
</style>
