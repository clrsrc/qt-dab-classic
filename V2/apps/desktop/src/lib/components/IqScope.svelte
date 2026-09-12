<script lang="ts">
  // Konstellation (Entscheidung 25): 1536 Traeger von OFDM-Symbol 2 nach
  // der Differenzdemodulation (int8, 127 = 1,0) aus `iq_samples`; nach Sync
  // vier Punktwolken (D-QPSK). Canvas + requestAnimationFrame.
  import { onMount } from "svelte";
  import { IQ_CARRIERS, makeFramePump, onScopeFrame } from "$lib/debug";

  const S = 96;
  const R = 40;
  const C = S / 2;
  let canvas = $state<HTMLCanvasElement | null>(null);
  let latest: Int8Array | null = null;
  let stale = 0;

  function draw() {
    const c = canvas;
    if (!c) return;
    const ctx = c.getContext("2d");
    if (!ctx) return;
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, S, S);
    // Achsen, Einheitskreis, Diagonalen (Sollpunkte bei +-45 Grad)
    ctx.strokeStyle = "#1e3a26";
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(C + 0.5, 0);
    ctx.lineTo(C + 0.5, S);
    ctx.moveTo(0, C + 0.5);
    ctx.lineTo(S, C + 0.5);
    ctx.stroke();
    ctx.strokeStyle = "#14261a";
    ctx.setLineDash([2, 3]);
    ctx.beginPath();
    ctx.moveTo(C - R, C - R);
    ctx.lineTo(C + R, C + R);
    ctx.moveTo(C - R, C + R);
    ctx.lineTo(C + R, C - R);
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.strokeStyle = "#2a5a36";
    ctx.beginPath();
    ctx.arc(C, C, R, 0, Math.PI * 2);
    ctx.stroke();
    const d = latest;
    if (!d) return;
    // Halbtransparente 2x2-Punkte: dichte Bereiche (die vier D-QPSK-Wolken)
    // werden heller, die duenne Ring-Streuung bei niedrigem SNR bleibt dunkel.
    ctx.fillStyle = stale ? "rgba(47, 106, 58, 0.35)" : "rgba(0, 224, 80, 0.3)";
    const n = Math.min(IQ_CARRIERS, d.length >> 1);
    for (let i = 0; i < n; i++) {
      const x = C + (d[2 * i] / 127) * R;
      const y = C - (d[2 * i + 1] / 127) * R;
      ctx.fillRect(x - 1, y - 1, 2, 2);
    }
  }

  onMount(() => {
    const pump = makeFramePump(draw);
    pump.request();
    const off = onScopeFrame((kind, data) => {
      if (kind !== "iq") return;
      latest = data as Int8Array;
      stale = 0;
      pump.request();
    });
    // Ohne Sync kommen keine IQ-Frames: alte Wolke abdunkeln
    const timer = setInterval(() => {
      if (latest && ++stale === 3) pump.request();
    }, 1000);
    return () => {
      off();
      pump.stop();
      clearInterval(timer);
    };
  });
</script>

<canvas bind:this={canvas} width={S} height={S} class="scope"></canvas>

<style>
  .scope { display: block; width: 96px; height: 96px; background: #000; border: 1px inset #2a2e36; }
</style>
