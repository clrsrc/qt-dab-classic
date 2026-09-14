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
    zoomable = false,
  }: {
    eid?: number | null;
    sid?: number | null;
    size?: LogoSize;
    src?: string | null;
    name?: string;
    px?: number;
    /** Klick oeffnet eine vergroesserte Ansicht (Bugfixes.txt #12). */
    zoomable?: boolean;
  } = $props();

  let fetched = $state<string | null>(null);
  let version = $state(0);
  let zoomed = $state(false);
  let large = $state<string | null>(null);

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

  function onKey(e: KeyboardEvent) {
    if (zoomed && e.key === "Escape") zoomed = false;
  }

  function openZoom() {
    if (!zoomable || !url) return;
    large = null;
    if (eid != null && sid != null && sid) {
      getLogo(eid, sid, "large").then((u) => (large = u ?? url));
    } else {
      large = url;
    }
    zoomed = true;
  }
</script>

{#if url && zoomable}
  <button type="button" class="logo-btn" onclick={openZoom} aria-label={name || t("logo.alt")}>
    <img class="logo" src={url} alt="" style="width:{px}px;height:{px}px" draggable="false" />
  </button>
{:else if url}
  <img class="logo" src={url} alt={name || t("logo.alt")} style="width:{px}px;height:{px}px" draggable="false" />
{:else}
  <span class="logo ph" title={t("logo.none")} style="width:{px}px;height:{px}px;font-size:{Math.max(8, Math.round(px / 3.2))}px">{initials(name)}</span>
{/if}

<svelte:window onkeydown={onKey} />

{#if zoomed}
  <div class="overlay" onmousedown={(e) => e.target === e.currentTarget && (zoomed = false)} role="presentation">
    <div class="dialog zoom" role="dialog" aria-modal="true" aria-label={name || t("logo.alt")}>
      <img src={large ?? url} alt={name || t("logo.alt")} draggable="false" />
    </div>
  </div>
{/if}

<style>
  .logo { flex: none; object-fit: contain; display: inline-flex; align-items: center; justify-content: center; }
  .logo-btn { all: unset; cursor: zoom-in; display: inline-flex; line-height: 0; }
  .ph { border: 1px solid #14261a; color: var(--green-dim); font-family: var(--mono); font-weight: bold; overflow: hidden; }
  .dialog.zoom { padding: 8px; display: flex; }
  .dialog.zoom img { max-width: min(80vw, 480px); max-height: min(80vh, 480px); object-fit: contain; }
</style>
