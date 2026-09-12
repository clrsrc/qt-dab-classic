<script lang="ts">
  import { api } from "$lib/core";
  import { t, tError } from "$lib/i18n.svelte";
  import { notify, patchSettings, s, ui } from "$lib/state.svelte";
  import SleepTimer from "./SleepTimer.svelte";
  import { debugApi } from "$lib/debug";

  // TII / Debug-Panel (lib/debug.ts): Heimatkoordinaten, Detektor, DX, Scope-Rate
  const numOrNull = (e: Event) => {
    const v = (e.target as HTMLInputElement).value.trim().replace(",", ".");
    return v === "" || isNaN(Number(v)) ? null : Number(v);
  };
  const setHome = (e: Event, which: "lat" | "lon") => {
    const v = numOrNull(e);
    const lat = which === "lat" ? v : (ui.settings?.home_lat ?? null);
    const lon = which === "lon" ? v : (ui.settings?.home_lon ?? null);
    run(debugApi.homeSet(lat, lon));
  };
  const setTii = (patch: { enabled?: boolean; threshold?: number; dx_mode?: boolean }) => {
    const st = ui.settings;
    if (!st) return;
    run(debugApi.tiiSet(patch.enabled ?? st.tii_enabled, patch.threshold ?? st.tii_threshold, patch.dx_mode ?? st.tii_dx_mode));
  };

  let lna = $state(40);
  let vga = $state(40);
  let amp = $state(false);
  $effect(() => {
    lna = s.gain.lna;
    vga = s.gain.vga;
    amp = s.gain.amp;
  });
  const run = (p: Promise<unknown>) => p.catch((e) => notify("warn", tError(e)));
  const sel = (e: Event) => (e.target as HTMLSelectElement).value;
  const chk = (e: Event) => (e.target as HTMLInputElement).checked;
</script>

{#if ui.settings}
  <section class="panel settings">
    <div class="panel-head"><span>{t("panel.settings")}</span></div>
    <div class="panel-body grid">
      <span class="lbl">{t("settings.language")}</span>
      <select value={ui.settings.language ?? ""} onchange={(e) => patchSettings({ language: sel(e) || null })}>
        <option value="">{t("settings.language_system")}</option>
        <option value="de">Deutsch</option>
        <option value="en">English</option>
      </select>

      <span class="lbl">{t("settings.audio_device")}</span>
      <select value={ui.settings.audio_device ?? ""} onchange={(e) => patchSettings({ audio_device: sel(e) === "" ? null : Number(sel(e)) })}>
        <option value="">{t("settings.audio_default")}</option>
        {#each s.audio_devices as name, i (i)}
          <option value={i}>{name}</option>
        {/each}
      </select>

      <span class="lbl">{t("settings.agc")}</span>
      <span><input type="checkbox" checked={ui.settings.agc} onchange={(e) => patchSettings({ agc: chk(e) })} /></span>

      <span class="lbl">{t("settings.gain")}</span>
      <span class="gain">
        <span class="k">{t("settings.lna")}</span><input type="number" bind:value={lna} min="0" max="40" step="8" />
        <span class="k">{t("settings.vga")}</span><input type="number" bind:value={vga} min="0" max="62" step="2" />
        <label class="inl"><input type="checkbox" bind:checked={amp} />{t("settings.amp")}</label>
        <button class="btn" onclick={() => run(api.setGain({ lna, vga, amp }))}>{t("settings.gain_apply")}</button>
        <span class="k">({s.gain.lna}/{s.gain.vga}/{s.gain.amp ? "A" : "-"}{s.agc ? " AGC" : ""})</span>
      </span>

      <span class="lbl">{t("settings.ppm")}</span>
      <span><input type="number" class="ppm" value={ui.settings.ppm} min="-200" max="200" onchange={(e) => patchSettings({ ppm: Number((e.target as HTMLInputElement).value) })} /></span>

      <span class="lbl">{t("settings.ews")}</span>
      <span>
        <input type="checkbox" checked={ui.settings.ews_enabled} onchange={(e) => patchSettings({ ews_enabled: chk(e) })} />
        <label class="inl"><input type="checkbox" checked={ui.settings.ews_autoswitch} onchange={(e) => patchSettings({ ews_autoswitch: chk(e) })} />{t("settings.ews_autoswitch")}</label>
      </span>

      <span class="lbl">{t("settings.autostart")}</span>
      <span><input type="checkbox" checked={ui.settings.autostart} onchange={(e) => patchSettings({ autostart: chk(e) })} /></span>

      <!-- Timer/Aufnahme/Sleep/Alarm (lib/timers.ts) -->
      <span class="lbl">{t("sleep.label")}</span>
      <span><SleepTimer /></span>

      <span class="lbl">{t("rec.dir")}</span>
      <span class="gain">
        <input type="text" class="dir" value={ui.settings.recording_dir ?? ""} placeholder={t("rec.dir_default")} onchange={(e) => patchSettings({ recording_dir: (e.target as HTMLInputElement).value.trim() || null })} />
        <span class="k">{t("rec.pre")}</span><input type="number" class="ppm" value={Math.round(ui.settings.record_pre_s / 60)} min="0" max="60" onchange={(e) => patchSettings({ record_pre_s: Math.max(0, Number((e.target as HTMLInputElement).value)) * 60 })} />
        <span class="k">{t("rec.post")}</span><input type="number" class="ppm" value={Math.round(ui.settings.record_post_s / 60)} min="0" max="120" onchange={(e) => patchSettings({ record_post_s: Math.max(0, Number((e.target as HTMLInputElement).value)) * 60 })} />
      </span>

      <span class="lbl">{t("alarm.beep")}</span>
      <span><input type="checkbox" checked={ui.settings.alarm_beep} onchange={(e) => patchSettings({ alarm_beep: chk(e) })} /></span>

      <!-- TII / Debug-Panel (lib/debug.ts, lib/tii.ts) -->
      <span class="lbl">{t("tii.home")}</span>
      <span class="gain">
        <span class="k">{t("tii.home_lat")}</span><input type="number" class="ppm" style="width:78px" step="0.0001" min="-90" max="90" value={ui.settings.home_lat ?? ""} placeholder="51.2180" onchange={(e) => setHome(e, "lat")} />
        <span class="k">{t("tii.home_lon")}</span><input type="number" class="ppm" style="width:78px" step="0.0001" min="-180" max="180" value={ui.settings.home_lon ?? ""} placeholder="6.7617" onchange={(e) => setHome(e, "lon")} />
        <span class="k">{t("tii.home_hint")}</span>
      </span>

      <span class="lbl">{t("tii.enabled")}</span>
      <span class="gain">
        <input type="checkbox" checked={ui.settings.tii_enabled} onchange={(e) => setTii({ enabled: chk(e) })} />
        <span class="k">{t("tii.threshold")}</span><input type="number" class="ppm" min="1" max="30" value={ui.settings.tii_threshold} onchange={(e) => setTii({ threshold: Math.max(1, Math.min(30, Number((e.target as HTMLInputElement).value))) })} />
        <label class="inl"><input type="checkbox" checked={ui.settings.tii_dx_mode} onchange={(e) => setTii({ dx_mode: chk(e) })} />{t("tii.dx_mode")}</label>
      </span>

      <span class="lbl">{t("debug.scope_rate")}</span>
      <span class="gain">
        <select value={ui.settings.scope_rate_hz} onchange={(e) => run(debugApi.setRate(Number(sel(e))))}>
          {#each [1, 2, 3, 5, 8, 10] as r (r)}<option value={r}>{r} Hz</option>{/each}
        </select>
        <span class="k">{t("debug.scope_rate_hint")}</span>
      </span>

      <span class="lbl">{t("settings.data_dir")}</span>
      <span class="mono" title={ui.dataDir}>{ui.dataDir} <span class="k">({ui.portable ? t("settings.portable") : t("settings.profile")})</span></span>

      <span class="lbl">{t("core.version")}</span>
      <span class="mono">{s.core_version || "–"} <button class="btn mini" onclick={() => run(api.restartCore())}>{t("core.restart")}</button></span>
    </div>
  </section>
{/if}

<style>
  .settings { flex: none; }
  .grid { display: grid; grid-template-columns: max-content 1fr; gap: 4px 10px; align-items: center; font-size: 10px; }
  .grid > .lbl { color: #7080a0; font-weight: bold; }
  .gain { display: flex; align-items: center; gap: 4px; flex-wrap: wrap; }
  .gain input[type="number"] { width: 48px; }
  .ppm { width: 60px; }
  .dir { flex: 1 1 160px; min-width: 120px; }
  .k { color: var(--text-dim); font-size: 9px; }
  .inl { display: inline-flex; align-items: center; margin-left: 6px; }
  .mono { font-family: var(--mono); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .mini { font-size: 9px; min-height: 14px; padding: 0 5px; }
</style>
