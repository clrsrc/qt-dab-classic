<script lang="ts">
  // Minimale Shell (M2 beginnt hier): zeigt Kernstatus, Ensemble, Dienste,
  // DLS und Pegel. Layout folgt spaeter dem Classic-Compact-Vorbild.
  import { onMount } from "svelte";
  import { getCurrentWindow } from "@tauri-apps/api/window";
  import { core, app, type CoreEvent, type ServiceInfo } from "$lib/core";

  let alive = $state(false);
  let coreVersion = $state("–");
  let synced = $state(false);
  let ensemble = $state("");
  let channel = $state("5C");
  let services = $state<ServiceInfo[]>([]);
  let current = $state<number | null>(null);
  let dls = $state("");
  let snr = $state(0);
  let level = $state<[number, number]>([0, 0]);
  let dataDir = $state("");
  let log = $state<string[]>([]);
  let eventCount = $state(0);

  function push(line: string) {
    log = [line, ...log].slice(0, 12);
  }

  function handle(ev: CoreEvent) {
    eventCount++;
    switch (ev.type) {
      case "ready":
        coreVersion = String(ev.core_version);
        alive = true;
        push(`Kern bereit ${coreVersion}`);
        break;
      case "synced":
        synced = Boolean(ev.synced);
        break;
      case "ensemble_found":
        ensemble = `${ev.name} (${Number(ev.eid).toString(16).toUpperCase()})`;
        services = [];
        push(`Ensemble ${ensemble}`);
        break;
      case "service_added":
        services = [...services, ev.service as ServiceInfo].sort((a, b) => a.name.localeCompare(b.name));
        break;
      case "service_started":
        current = Number(ev.sid);
        break;
      case "dls":
        dls = String(ev.text);
        break;
      case "snr":
        snr = Number(ev.db);
        break;
      case "audio_level":
        level = [Number(ev.left), Number(ev.right)];
        break;
      case "device_error":
      case "log":
        push(`${ev.type}: ${ev.message ?? ev.text}`);
        break;
      case "exiting":
        alive = false;
        push(`Kern beendet: ${ev.reason}`);
        break;
    }
  }

  onMount(() => {
    let un: (() => void) | undefined;
    (async () => {
      un = await core.onEvent(handle);
      alive = await core.alive();
      const [dir] = await app.dataDir();
      dataDir = dir;
      await core.send({ type: "get_state" });
    })();
    return () => un?.();
  });

  async function select(s: ServiceInfo) {
    await core.send({ type: "select_service", sid: s.sid, scids: s.scids, slot: "primary" });
  }
  async function tune() {
    await core.send({ type: "open_device", source: { kind: "hack_rf", serial: null } });
    await core.send({ type: "set_channel", channel });
  }
  async function spike() {
    await core.send({ type: "open_device", source: { kind: "file", path: "spike", loop: true } });
  }
  const win = getCurrentWindow();
</script>

<div class="shell">
  <header data-tauri-drag-region>
    <span class="title" data-tauri-drag-region>DAB Classic</span>
    <span class="status" class:ok={alive}>KERN</span>
    <span class="status" class:ok={synced}>SYNC</span>
    <button class="win" onclick={() => win.minimize()} aria-label="Minimieren">–</button>
    <button class="win" onclick={() => win.close()} aria-label="Schließen">×</button>
  </header>

  <section class="display">
    <div class="line big">{ensemble || "kein Ensemble"}</div>
    <div class="line">{services.find((s) => s.sid === current)?.name ?? "–"}</div>
    <div class="line dls">{dls || " "}</div>
    <div class="meters">
      <span>SNR {snr.toFixed(1)} dB</span>
      <span class="vu"><i style="width:{Math.min(100, level[0] * 100)}%"></i></span>
      <span class="vu"><i style="width:{Math.min(100, level[1] * 100)}%"></i></span>
    </div>
  </section>

  <section class="controls">
    <input bind:value={channel} size="4" />
    <button onclick={tune}>HackRF</button>
    <button onclick={spike}>Spike</button>
    <span class="dim">{eventCount} Ereignisse · {dataDir}</span>
  </section>

  <section class="playlist">
    {#each services as s (s.sid)}
      <button class="svc" class:active={s.sid === current} onclick={() => select(s)}>
        <span>{s.name}</span><span class="dim">{s.bitrate_kbps} kbit/s</span>
      </button>
    {/each}
  </section>

  <section class="log">
    {#each log as l}<div>{l}</div>{/each}
  </section>
</div>

<style>
  :global(body) { margin: 0; background: #0b0d10; color: #d6dbe1; font: 13px/1.35 "Segoe UI", system-ui, sans-serif; user-select: none; }
  .shell { display: flex; flex-direction: column; height: 100vh; border: 1px solid #2a3038; box-sizing: border-box; }
  header { display: flex; align-items: center; gap: 8px; padding: 4px 8px; background: linear-gradient(#1e242c, #141920); border-bottom: 1px solid #2a3038; }
  .title { flex: 1; font-weight: 600; letter-spacing: 0.08em; color: #9fd3ff; }
  .status { font-size: 10px; padding: 1px 6px; border-radius: 3px; background: #2a3038; color: #7a8592; }
  .status.ok { background: #1f5a2c; color: #9df0a8; }
  .win { background: none; border: 1px solid #3a424c; color: #aab; width: 20px; height: 18px; line-height: 14px; padding: 0; cursor: default; }
  .display { padding: 10px 12px; background: #0f1a12; border-bottom: 1px solid #2a3038; font-family: Consolas, monospace; }
  .line { color: #7fe08a; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .line.big { font-size: 16px; }
  .dls { color: #c8f0cc; min-height: 1.35em; }
  .meters { display: flex; gap: 10px; align-items: center; margin-top: 6px; font-size: 11px; color: #8fb; }
  .vu { flex: 1; height: 6px; background: #14261a; border: 1px solid #1f3a26; }
  .vu i { display: block; height: 100%; background: linear-gradient(90deg, #3ad25a, #d9d23a 70%, #e0403a); }
  .controls { display: flex; gap: 6px; align-items: center; padding: 6px 8px; border-bottom: 1px solid #2a3038; }
  .controls input, .controls button { background: #1a2028; color: #d6dbe1; border: 1px solid #3a424c; padding: 2px 8px; }
  .playlist { flex: 1; overflow: auto; padding: 4px; }
  .svc { display: flex; justify-content: space-between; width: 100%; text-align: left; background: none; border: 0; color: #d6dbe1; padding: 3px 8px; cursor: default; }
  .svc:hover { background: #1a2028; }
  .svc.active { color: #fff; background: #1f3d5a; }
  .dim { color: #7a8592; font-size: 11px; }
  .log { max-height: 90px; overflow: auto; padding: 4px 8px; border-top: 1px solid #2a3038; font-size: 11px; color: #8a95a2; font-family: Consolas, monospace; }
</style>
