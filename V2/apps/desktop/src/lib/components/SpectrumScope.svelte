<script lang="ts">
  // Spektrum (Entscheidung 25): 2048 Bins (u8, 0,5 dB/Stufe, fftshift) aus
  // `spectrum`-Ereignissen, gezeichnet per Canvas + requestAnimationFrame,
  // ohne Svelte-Reaktivitaet je Bin. Skala folgt dem Signal (glatt), Mitte
  // und DAB-Kanalgrenzen (+-768 kHz) markiert, Peak-Hold optional.
  import { onMount } from "svelte";
  import { binToDbfs, makeFramePump, onScopeFrame, SPECTRUM_BINS } from "$lib/debug";
  import { t } from "$lib/i18n.svelte";

  let { peakHold = true }: { peakHold?: boolean } = $props();

  const W = 512;
  const H = 96;
  const LEFT = 26; // Platz fuer dB-Beschriftung
  const BINS_PER_COL = SPECTRUM_BINS / (W - LEFT) ; // ~4,2

  let canvas = $state<HTMLCanvasElement | null>(null);
  let latest: Uint8Array | null = null;
  let frames = 0;
  const cols = W - LEFT;
  const colDb = new Float32Array(cols);
  const peak = new Float32Array(cols).fill(-200);
  let lo = -110;
  let hi = -30;

  function draw() {
    const c = canvas;
    if (!c) return;
    const ctx = c.getContext("2d");
    if (!ctx) return;
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, W, H);
    const data = latest;
    if (!data) {
      ctx.fillStyle = "#3a4050";
      ctx.font = "10px Consolas, monospace";
      ctx.fillText(t("debug.no_data"), LEFT + 6, H / 2 + 4);
      return;
    }
    // Spalten: Maximum der zugehoerigen Bins
    let fmin = 1e9;
    let fmax = -1e9;
    for (let x = 0; x < cols; x++) {
      const b0 = Math.floor(x * BINS_PER_COL);
      const b1 = Math.min(SPECTRUM_BINS, Math.floor((x + 1) * BINS_PER_COL) || b0 + 1);
      let m = 0;
      for (let b = b0; b < b1; b++) if (data[b] > m) m = data[b];
      const db = binToDbfs(m);
      colDb[x] = db;
      if (db < fmin) fmin = db;
      if (db > fmax) fmax = db;
      if (db > peak[x]) peak[x] = db;
      else peak[x] -= 0.25;
    }
    // Skala glatt nachfuehren (mind. 40 dB Spanne)
    let tlo = Math.floor((fmin - 4) / 10) * 10;
    let thi = Math.ceil((fmax + 4) / 10) * 10;
    if (thi - tlo < 40) thi = tlo + 40;
    lo += (tlo - lo) * 0.2;
    hi += (thi - hi) * 0.2;
    const y = (db: number) => H - 10 - ((db - lo) / (hi - lo)) * (H - 14);

    // Raster + Beschriftung
    ctx.strokeStyle = "#14261a";
    ctx.fillStyle = "#3a8a4a";
    ctx.font = "8px Consolas, monospace";
    ctx.lineWidth = 1;
    const step = hi - lo > 80 ? 20 : 10;
    for (let d = Math.ceil(lo / step) * step; d <= hi; d += step) {
      const yy = Math.round(y(d)) + 0.5;
      ctx.beginPath();
      ctx.moveTo(LEFT, yy);
      ctx.lineTo(W, yy);
      ctx.stroke();
      ctx.fillText(String(Math.round(d)), 1, yy + 3);
    }
    // Kanalgrenzen +-768 kHz und Mitte
    const xOf = (bin: number) => LEFT + (bin / SPECTRUM_BINS) * cols;
    ctx.strokeStyle = "#1e3a26";
    for (const bin of [1024 - 768, 1024 + 768]) {
      const xx = Math.round(xOf(bin)) + 0.5;
      ctx.beginPath();
      ctx.moveTo(xx, 0);
      ctx.lineTo(xx, H - 10);
      ctx.stroke();
    }
    ctx.strokeStyle = "#e0b030";
    ctx.setLineDash([2, 3]);
    const xm = Math.round(xOf(1024)) + 0.5;
    ctx.beginPath();
    ctx.moveTo(xm, 0);
    ctx.lineTo(xm, H - 10);
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.fillStyle = "#3a8a4a";
    ctx.fillText("-1", LEFT + 1, H - 1);
    ctx.fillText("-768k", xOf(256) - 12, H - 1);
    ctx.fillText("0", xm - 2, H - 1);
    ctx.fillText("+768k", xOf(1792) - 12, H - 1);
    ctx.fillText("+1 MHz", W - 30, H - 1);

    // Peak-Hold
    if (peakHold) {
      ctx.strokeStyle = "#7a6020";
      ctx.beginPath();
      for (let x = 0; x < cols; x++) {
        const yy = y(peak[x]);
        if (x === 0) ctx.moveTo(LEFT + x, yy);
        else ctx.lineTo(LEFT + x, yy);
      }
      ctx.stroke();
    }
    // Spektrum mit Fuellung
    ctx.beginPath();
    ctx.moveTo(LEFT, H - 10);
    for (let x = 0; x < cols; x++) ctx.lineTo(LEFT + x, y(colDb[x]));
    ctx.lineTo(W - 1, H - 10);
    ctx.closePath();
    ctx.fillStyle = "rgba(0, 160, 48, 0.25)";
    ctx.fill();
    ctx.strokeStyle = "#00e050";
    ctx.beginPath();
    for (let x = 0; x < cols; x++) {
      const yy = y(colDb[x]);
      if (x === 0) ctx.moveTo(LEFT + x, yy);
      else ctx.lineTo(LEFT + x, yy);
    }
    ctx.stroke();
    ctx.fillStyle = "#3a8a4a";
    ctx.fillText(`dBFS  #${frames}`, W - 62, 9);
  }

  onMount(() => {
    const pump = makeFramePump(draw);
    pump.request();
    const off = onScopeFrame((kind, data) => {
      if (kind !== "spectrum") return;
      latest = data as Uint8Array;
      frames++;
      pump.request();
    });
    return () => {
      off();
      pump.stop();
    };
  });
  $effect(() => {
    void peakHold;
    if (!peakHold) peak.fill(-200);
  });
</script>

<canvas bind:this={canvas} width={W} height={H} class="scope"></canvas>

<style>
  .scope { display: block; width: 100%; height: 96px; background: #000; border: 1px inset #2a2e36; }
</style>
