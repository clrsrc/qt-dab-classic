<script lang="ts">
  import { openMenu } from "$lib/dialogs.svelte";
  import { t } from "$lib/i18n.svelte";
  import { clearPreset, importFavorites, parseDragPayload, recallPreset, storePreset } from "$lib/presets";
  import { activePresetSlot, s, ui } from "$lib/state.svelte";
  import Logo from "./Logo.svelte";

  const LONG_PRESS_MS = 500;
  let pressTimer: ReturnType<typeof setTimeout> | undefined;
  let longPressed = false;
  let dragOver = $state<number | null>(null);
  const active = $derived(activePresetSlot());

  function down(slot: number, e: PointerEvent) {
    if (e.button !== 0) return;
    longPressed = false;
    pressTimer = setTimeout(() => {
      longPressed = true;
      void storePreset(slot);
    }, LONG_PRESS_MS);
  }
  function up() {
    clearTimeout(pressTimer);
  }
  function click(slot: number) {
    if (longPressed) {
      longPressed = false;
      return;
    }
    void recallPreset(slot);
  }
  function menu(slot: number, e: MouseEvent) {
    e.preventDefault();
    clearTimeout(pressTimer);
    openMenu(e.clientX, e.clientY, [
      { label: t("preset.store"), action: () => storePreset(slot), disabled: !s.current },
      { label: t("preset.clear"), action: () => clearPreset(slot), disabled: !ui.presets.slots[slot] },
    ]);
  }
  function drop(slot: number, e: DragEvent) {
    e.preventDefault();
    dragOver = null;
    const svc = parseDragPayload(e.dataTransfer?.getData("text/plain") ?? "");
    if (svc) void storePreset(slot, svc);
  }
  function tip(slot: number) {
    const p = ui.presets.slots[slot];
    return p ? t("preset.tooltip", { n: slot + 1, name: p.name, channel: p.channel }) : t("preset.tooltip_empty", { n: slot + 1 });
  }
</script>

<section class="presets">
  <div class="panel-head">
    <span>{t("panel.presets")}</span>
    <span class="grow"></span>
    <button class="btn mini" onclick={importFavorites}>{t("preset.import")}</button>
  </div>
  <div class="grid">
    {#each ui.presets.slots as p, i (i)}
      <button
        class="btn slot"
        class:on={active === i}
        class:tuning={s.pending?.slot === i}
        class:over={dragOver === i}
        class:empty={!p}
        title={tip(i)}
        onpointerdown={(e) => down(i, e)}
        onpointerup={up}
        onpointerleave={up}
        onclick={() => click(i)}
        oncontextmenu={(e) => menu(i, e)}
        ondragover={(e) => { e.preventDefault(); dragOver = i; }}
        ondragleave={() => (dragOver = null)}
        ondrop={(e) => drop(i, e)}
      >
        <span class="num">{i === 9 ? 0 : i + 1}</span>
        <span class="name">
          {#if p}<Logo src={p.logo_data_url ?? null} eid={p.eid} sid={p.sid} size="small" name={p.name} px={14} />{/if}
          {p ? p.name : t("preset.empty")}
        </span>
        <span class="ch">{p ? p.channel : ""}</span>
      </button>
    {/each}
  </div>
</section>

<style>
  .presets { flex: none; }
  .grid { display: grid; grid-template-columns: repeat(5, 1fr); gap: 2px; padding: 3px 4px; }
  .slot { display: flex; flex-direction: column; align-items: stretch; padding: 2px 4px; min-height: 30px; gap: 0; position: relative; }
  .slot .num { position: absolute; left: 3px; top: 1px; font-size: 8px; color: #6a7280; }
  .slot .name { font-size: 10px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; text-align: center; padding: 0 8px; display: flex; align-items: center; justify-content: center; gap: 3px; }
  .slot .ch { font-size: 8px; color: #6a7280; text-align: center; font-family: var(--mono); min-height: 10px; }
  .slot.empty .name { color: #4a5060; font-weight: normal; }
  .slot.on { color: var(--green-hi); border-style: inset; background: #0a1a10; }
  .slot.on .ch { color: var(--green); }
  .slot.tuning { color: var(--amber); }
  .slot.over { outline: 1px dashed var(--green-hi); }
</style>
