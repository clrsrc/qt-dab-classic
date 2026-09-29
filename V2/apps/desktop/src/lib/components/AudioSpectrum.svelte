<script lang="ts">
  // Grafik-Equalizer-ANZEIGE (nur Anzeige, keine Regelung; Vorbild Crossmixer
  // Source/GUI/SpectrumAnalyzer: 48 Baender 40 Hz..16 kHz logarithmisch,
  // Hann-FFT 1024). Die FFT rechnet der Kern auf dem PCM des Hoerdienstes
  // (AudioPipeline) und schickt ~20 Hz `audio_spectrum` (u8 je Band,
  // 0,5 dB ab -90 dBFS, latest-wins). Ballistik wie ein Pegelmesser:
  // sofortiger Anstieg, exponentieller Abfall. Bei verstecktem Fenster
  // (Infobereich, minimiert) wird das Ereignis abbestellt (set_audio_spectrum),
  // wie Crossmixer setActive(false) bei visibilityChanged.
  import { onMount } from "svelte";
  import { api } from "$lib/core";
  import { onScopeFrame } from "$lib/debug";

  const BARS = 48;
  const W = 480;
  const H = 48;
  const MIN_DB = -85;
  let canvas = $state<HTMLCanvasElement | null>(null);
  const levels = new Float32Array(BARS);
  let latest: Uint8Array | null = null;
  let lastData = 0;
  let raf = 0;

  function setEnabled(on: boolean) {
    api.send({ type: "set_audio_spectrum", enabled: on }).catch(() => {
      // Kern gerade nicht da: beim naechsten Sichtbarkeitswechsel erneut
    });
  }

  function draw() {
    raf = requestAnimationFrame(draw);
    const c = canvas;
    if (!c) return;
    const ctx = c.getContext("2d");
    if (!ctx) return;
    if (latest) {
      for (let b = 0; b < BARS && b < latest.length; b++) {
        const db = latest[b] / 2 - 90;
        const n = Math.min(1, Math.max(0, (db - MIN_DB) / -MIN_DB));
        if (n > levels[b]) levels[b] = n;
      }
      latest = null;
    }
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, W, H);
    const gap = 1;
    const bw = Math.max(1, (W - gap * (BARS + 1)) / BARS);
    // ohne frische Daten (Dienst weg, Stille) zuegig auf Null
    const decay = performance.now() - lastData > 1500 ? 0.8 : 0.94;
    const grad = ctx.createLinearGradient(0, H, 0, 0);
    grad.addColorStop(0, "#00c040");
    grad.addColorStop(0.6, "#d9d23a");
    grad.addColorStop(1, "#ff3030");
    ctx.fillStyle = grad;
    for (let b = 0; b < BARS; b++) {
      levels[b] *= decay;
      const l = levels[b];
      if (l <= 0.004) continue;
      const bh = Math.max(1, l * H);
      ctx.fillRect(gap + b * (bw + gap), H - bh, bw, bh);
    }
  }

  onMount(() => {
    const off = onScopeFrame((kind, data) => {
      if (kind !== "audio") return;
      latest = data as Uint8Array;
      lastData = performance.now();
    });
    const vis = () => setEnabled(!document.hidden);
    document.addEventListener("visibilitychange", vis);
    setEnabled(!document.hidden);
    raf = requestAnimationFrame(draw);
    return () => {
      off();
      document.removeEventListener("visibilitychange", vis);
      cancelAnimationFrame(raf);
      setEnabled(false);
    };
  });
</script>

<canvas bind:this={canvas} width={W} height={H} class="eq" title="40 Hz – 16 kHz"></canvas>

<style>
  .eq { display: block; width: 100%; height: 48px; background: #000; border: 1px solid #1f3a26; }
</style>
