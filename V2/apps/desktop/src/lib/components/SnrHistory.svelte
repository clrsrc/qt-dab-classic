<script lang="ts">
  // SNR-Verlauf (Entscheidung 25): 120 Werte (1 Hz) aus `s.debug.snr_history`
  // (Rust-Ring, `debug_stats` 1 Hz bei offenem Panel), Skala 0..20 dB,
  // Linie per Canvas. Reaktiv nur auf das Array als Ganzes.
  import { s } from "$lib/state.svelte";

  const W = 360;
  const H = 60;
  const LEFT = 18;
  const N = 120;
  const DB_MAX = 20;
  let canvas = $state<HTMLCanvasElement | null>(null);

  function draw(hist: number[], now: number) {
    const c = canvas;
    if (!c) return;
    const ctx = c.getContext("2d");
    if (!ctx) return;
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, W, H);
    const y = (db: number) => H - 8 - (Math.max(0, Math.min(DB_MAX, db)) / DB_MAX) * (H - 12);
    ctx.font = "8px Consolas, monospace";
    ctx.lineWidth = 1;
    for (const d of [0, 5, 10, 15, 20]) {
      const yy = Math.round(y(d)) + 0.5;
      ctx.strokeStyle = d === 10 ? "#1e3a26" : "#14261a";
      ctx.beginPath();
      ctx.moveTo(LEFT, yy);
      ctx.lineTo(W, yy);
      ctx.stroke();
      ctx.fillStyle = "#3a8a4a";
      ctx.fillText(String(d), 1, yy + 3);
    }
    // Zeitmarken alle 30 s
    ctx.strokeStyle = "#14261a";
    for (let i = 30; i < N; i += 30) {
      const xx = Math.round(LEFT + ((N - i) / N) * (W - LEFT)) + 0.5;
      ctx.beginPath();
      ctx.moveTo(xx, 0);
      ctx.lineTo(xx, H - 8);
      ctx.stroke();
      ctx.fillStyle = "#3a8a4a";
      ctx.fillText(`-${i}s`, xx - 8, H - 1);
    }
    if (!hist.length) return;
    // Rechtsbuendig: der neueste Wert ganz rechts
    const dx = (W - LEFT) / (N - 1);
    const x0 = LEFT + (N - hist.length) * dx;
    ctx.beginPath();
    ctx.moveTo(x0, H - 8);
    hist.forEach((v, i) => ctx.lineTo(x0 + i * dx, y(v)));
    ctx.lineTo(x0 + (hist.length - 1) * dx, H - 8);
    ctx.closePath();
    ctx.fillStyle = "rgba(0, 160, 48, 0.2)";
    ctx.fill();
    ctx.strokeStyle = "#00e050";
    ctx.beginPath();
    hist.forEach((v, i) => (i ? ctx.lineTo(x0 + i * dx, y(v)) : ctx.moveTo(x0, y(v))));
    ctx.stroke();
    ctx.fillStyle = "#00e050";
    ctx.font = "bold 10px Consolas, monospace";
    ctx.fillText(`${now.toFixed(1)} dB`, W - 50, 10);
  }

  $effect(() => {
    draw(s.debug.snr_history, s.snr);
  });
</script>

<canvas bind:this={canvas} width={W} height={H} class="scope"></canvas>

<style>
  .scope { display: block; width: 100%; height: 60px; background: #000; border: 1px inset #2a2e36; }
</style>
