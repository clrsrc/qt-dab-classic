<script lang="ts">
  // Leiste fuer die Wiedergabe eines Mitschnitts (lib/playback.svelte.ts):
  // erscheint nur, solange eine Datei geladen ist; "x" schliesst und gibt
  // die Blob-URL frei. Laeuft unabhaengig vom Kern-Audio (Live-Programm
  // spielt weiter - bei Bedarf vorher stumm schalten).
  import { t } from "$lib/i18n.svelte";
  import { playback, stopPlayback } from "$lib/playback.svelte";
</script>

{#if playback.url}
  <section class="player">
    <span class="lbl" title={playback.path ?? ""}>{t("playback.title")}: {playback.label}</span>
    <!-- svelte-ignore a11y_media_has_caption -->
    <audio src={playback.url} controls autoplay onended={() => undefined}></audio>
    <button class="btn mini" title={t("playback.close")} onclick={stopPlayback}>✕</button>
  </section>
{/if}

<style>
  .player { display: flex; align-items: center; gap: 6px; padding: 2px 6px; border-top: 1px solid #14261a; border-bottom: 1px solid #14261a; font-size: 10px; }
  .lbl { flex: 0 1 auto; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: var(--text-dim); }
  audio { flex: 1 1 200px; height: 24px; min-width: 120px; }
</style>
