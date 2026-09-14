<script lang="ts">
  // Hauptfenster "Classic Compact": Titelleiste, LCD-Anzeige, Transport,
  // Speicherleiste, Kanal/Geraet, ein-/ausklappbare Panels, Statusleiste.
  import { onMount } from "svelte";
  import { makeKeyHandler } from "$lib/hotkeys";
  import { t, tError } from "$lib/i18n.svelte";
  import { recallPreset, storePreset } from "$lib/presets";
  import { dispose, init, notify, togglePanel, ui } from "$lib/state.svelte";
  import AlarmBanner from "$lib/components/AlarmBanner.svelte";
  import ChannelBar from "$lib/components/ChannelBar.svelte";
  import Dialogs from "$lib/components/Dialogs.svelte";
  import Display from "$lib/components/Display.svelte";
  import EpgPanel from "$lib/components/EpgPanel.svelte";
  import EwsHistoryPanel from "$lib/components/EwsHistoryPanel.svelte";
  import PresetBar from "$lib/components/PresetBar.svelte";
  import TimerPanel from "$lib/components/TimerPanel.svelte";
  import MusicPanel from "$lib/components/MusicPanel.svelte";
  import DebugPanel from "$lib/components/DebugPanel.svelte";
  import ScanPanel from "$lib/components/ScanPanel.svelte";
  import ServiceList from "$lib/components/ServiceList.svelte";
  import StationList from "$lib/components/StationList.svelte";
  import SettingsPanel from "$lib/components/SettingsPanel.svelte";
  import StatusBar from "$lib/components/StatusBar.svelte";
  import TitleBar from "$lib/components/TitleBar.svelte";
  import Transport from "$lib/components/Transport.svelte";
  import TimeshiftBar from "$lib/components/TimeshiftBar.svelte";

  const onKey = makeKeyHandler({
    recallPreset: (slot) => void recallPreset(slot),
    storePreset: (slot) => void storePreset(slot),
    error: (e) => notify("warn", tError(e)),
  });

  onMount(() => {
    init().catch((e) => notify("error", String(e)));
    return dispose;
  });
  const panels = $derived(ui.settings?.panels);
  // EPG/Timer haben eigene Buttons in Transport.svelte (oben links neben Vol),
  // hier nicht nochmal doppelt (Bugfixes.txt #11).
  const tabs = ["presets", "services", "stations", "scan", "settings", "music", "debug"] as const;
</script>

<!-- Nach einer Auswahl den Fokus abgeben, damit die Tastenkuerzel (Ziffern, M, +/-) wieder greifen. -->
<svelte:window
  onkeydown={onKey}
  oncontextmenu={(e) => e.preventDefault()}
  onchange={(e) => { const el = e.target as HTMLElement; if (el?.tagName === "SELECT" || el?.tagName === "INPUT") el.blur(); }}
/>

<div class="shell">
  <TitleBar />
  <AlarmBanner />
  <Display />
  <Transport />
  <TimeshiftBar />
  {#if panels?.presets}<PresetBar />{/if}
  <ChannelBar />
  <nav class="tabs">
    {#each tabs as name (name)}
      <button class="btn" class:on={panels?.[name]} onclick={() => togglePanel(name)}>{t(`panel.${name}`)}</button>
    {/each}
  </nav>
  <div class="panels">
    {#if panels?.services}<ServiceList />{/if}
    {#if panels?.stations}<StationList />{/if}
    {#if panels?.scan}<ScanPanel />{/if}
    {#if panels?.settings}<SettingsPanel />{/if}
    {#if panels?.epg}<EpgPanel />{/if}
    {#if panels?.ews_history}<EwsHistoryPanel />{/if}
    {#if panels?.timer}<TimerPanel />{/if}
    {#if panels?.music}<MusicPanel />{/if}
    {#if panels?.debug}<DebugPanel />{/if}
  </div>
  <StatusBar />
</div>
<Dialogs />

<style>
  .panels { flex: 1; display: flex; flex-direction: column; overflow: auto; min-height: 0; }
</style>
