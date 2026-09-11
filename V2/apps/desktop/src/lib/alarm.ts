// Warnton bei Notfallwarnung (Web Audio, kein Kern-Umweg). Ein kurzer
// 880-Hz-Ton je Sekunde, solange der Alarm laeuft und nicht quittiert ist.

let ctx: AudioContext | null = null;
let timer: ReturnType<typeof setInterval> | undefined;

function beep() {
  try {
    ctx ??= new AudioContext();
    if (ctx.state === "suspended") void ctx.resume();
    const osc = ctx.createOscillator();
    const gain = ctx.createGain();
    osc.type = "square";
    osc.frequency.value = 880;
    gain.gain.value = 0.08;
    osc.connect(gain).connect(ctx.destination);
    const t0 = ctx.currentTime;
    osc.start(t0);
    osc.stop(t0 + 0.3);
  } catch {
    // Audio nicht verfuegbar: Banner reicht.
  }
}

export function startBeep() {
  if (timer) return;
  beep();
  timer = setInterval(beep, 1000);
}

export function stopBeep() {
  if (timer) clearInterval(timer);
  timer = undefined;
}
