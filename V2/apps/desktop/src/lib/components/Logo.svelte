<script lang="ts">
  // Senderlogo als data:-URL aus dem Rust-Cache; Platzhalter mit Kuerzel,
  // wenn (noch) keins vorhanden ist. `src` uebersteuert den Abruf (Display:
  // Logo aus dem AppState; Slots: im Preset gespeichertes Logo).
  import { onMount } from "svelte";
  import { t } from "$lib/i18n.svelte";
  import { getLogo, initials, onLogoUpdated, type LogoSize } from "$lib/logos";

  let {
    eid = null,
    sid = null,
    size = "small",
    src = null,
    name = "",
    px = 32,
  }: { eid?: number | null; sid?: number | null; size?: LogoSize; src?: string | null; name?: string; px?: number } = $props();

  let fetched = $state<string | null>(null);
  let version = $state(0);

  onMount(() =>
    onLogoUpdated((e, s) => {
      if (e === eid && s === sid) version++;
    }),
  );

  $effect(() => {
    const e = eid;
    const s = sid;
    void version;
    if (src || e == null || s == null || !s) {
      fetched = null;
      return;
    }
    let alive = true;
    getLogo(e, s, size).then((url) => {
      if (alive) fetched = url;
    });
    return () => {
      alive = false;
    };
  });

  const url = $derived(src ?? fetched);
</script>

{#if url}
  <img class="logo" src={url} alt={name || t("logo.alt")} style="width:{px}px;height:{px}px" draggable="false" />
{:else}
  <span class="logo ph" title={t("logo.none")} style="width:{px}px;height:{px}px;font-size:{Math.max(8, Math.round(px / 3.2))}px">{initials(name)}</span>
{/if}

<style>
  .logo { flex: none; object-fit: contain; display: inline-flex; align-items: center; justify-content: center; }
  .ph { border: 1px solid #14261a; color: var(--green-dim); font-family: var(--mono); font-weight: bold; overflow: hidden; }
</style>
