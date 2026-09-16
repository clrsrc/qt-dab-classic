<script lang="ts">
  // Einstellungs-Panel: alle Felder aus dab-app::settings (settings.json) in
  // Gruppen. Jede Aenderung laeuft ueber patchSettings -> update_settings; die
  // Rust-Seite schickt die Kern-Kommandos (set_agc, set_ppm, set_audio_device,
  // set_volume, set_ews, set_epg, set_tii, set_scopes) selbst nach.
  import { untrack } from "svelte";
  import { api, type Gain } from "$lib/core";
  import { t, tError } from "$lib/i18n.svelte";
  import { notify, patchSettings, s, ui } from "$lib/state.svelte";
  import SleepTimer from "./SleepTimer.svelte";
  import { debugApi } from "$lib/debug";
  import { CAPACITY_MAX_MIN, CAPACITY_MIN_MIN } from "$lib/timeshift";

  const run = (p: Promise<unknown>) => p.catch((e) => notify("warn", tError(e)));
  const sel = (e: Event) => (e.target as HTMLSelectElement).value;
  const chk = (e: Event) => (e.target as HTMLInputElement).checked;
  const num = (e: Event) => Number((e.target as HTMLInputElement).value);

  // TII / Debug-Panel (lib/debug.ts): Heimatkoordinaten, Detektor, DX, Scope-Rate
  // Befund 9: `min`/`max` am number-Input halten getippte Werte nicht auf, und
  // Rust (tii.rs) speichert ungeprueft. Leeres Feld = Koordinate loeschen;
  // alles andere muss eine Zahl im gueltigen Bereich sein, sonst Meldung und
  // nichts speichern (Feld auf den gespeicherten Wert zurueck).
  const HOME_RANGE = { lat: 90, lon: 180 } as const;
  const setHome = (e: Event, which: "lat" | "lon") => {
    const el = e.target as HTMLInputElement;
    const raw = el.value.trim().replace(",", ".");
    const prev = (which === "lat" ? ui.settings?.home_lat : ui.settings?.home_lon) ?? null;
    let v: number | null = null;
    if (raw !== "") {
      v = Number(raw);
      if (!isFinite(v) || Math.abs(v) > HOME_RANGE[which]) {
        notify("warn", t(which === "lat" ? "tii.home_lat_invalid" : "tii.home_lon_invalid"));
        el.value = prev === null ? "" : String(prev);
        return;
      }
    }
    const lat = which === "lat" ? v : (ui.settings?.home_lat ?? null);
    const lon = which === "lon" ? v : (ui.settings?.home_lon ?? null);
    run(debugApi.homeSet(lat, lon));
  };
  // ASA-"Standort-Code" (ETSI TS 104 089 Annex A, z. B. von asa.radio) als
  // Alternative zur direkten Grad-Eingabe; wird einmalig in home_lat/lon
  // umgerechnet, nicht selbst gespeichert.
  let homeCode = $state("");
  const applyHomeCode = () => {
    const code = homeCode.trim();
    if (!code) return;
    debugApi
      .homeCodeDecode(code)
      .then(([lat, lon]) => run(debugApi.homeSet(lat, lon)))
      .catch((e) => notify("warn", tError(e)));
  };
  const setTii = (patch: { enabled?: boolean; threshold?: number; dx_mode?: boolean }) => {
    const st = ui.settings;
    if (!st) return;
    run(debugApi.tiiSet(patch.enabled ?? st.tii_enabled, patch.threshold ?? st.tii_threshold, patch.dx_mode ?? st.tii_dx_mode));
  };

  // Geraet / Gain (Entscheidung 26): Gain-Satz je Geraet und Kanal, RTL-SDR
  // hat nur den Tuner-Gain (lna in 0,1 dB), Datei-Quelle keinen Gain.
  const deviceKind = $derived(s.device?.kind ?? ui.settings?.device ?? "hackrf");
  const isFile = $derived(deviceKind === "file");
  const isRtl = $derived(deviceKind === "rtlsdr");
  const gainLocked = $derived(isFile || !s.device || (ui.settings?.agc ?? true));
  const deviceName = (kind: string) => (kind === "rtlsdr" ? t("device.rtlsdr") : kind === "file" ? t("device.file") : t("device.hackrf"));
  const fmtGain = (g: Gain, rtl: boolean) => (rtl ? `${(g.lna / 10).toFixed(1)} dB` : `${g.lna}/${g.vga}/${g.amp ? "A" : "-"}`);
  const memory = $derived.by(() => {
    const table = ui.settings?.gain_by_channel?.[deviceKind] ?? {};
    const ch = s.channel ?? ui.settings?.last_channel ?? "";
    const entry = ch ? table[ch] : undefined;
    return { count: Object.keys(table).length, channel: ch || "–", entry };
  });

  let lna = $state(40);
  let vga = $state(40);
  let amp = $state(false);
  let tunerDb = $state(30);
  // Befund 10: solange der Nutzer die Felder bearbeitet (und noch nicht
  // "Setzen" gedrueckt hat), darf ein gain_changed des Kerns (z. B. der
  // Gain-Speicher beim Kanalwechsel) die Eingabe nicht ueberschreiben.
  let gainDirty = $state(false);
  $effect(() => {
    const g = s.gain;
    if (untrack(() => gainDirty)) return;
    lna = g.lna;
    vga = g.vga;
    amp = g.amp;
    tunerDb = Math.round(g.lna) / 10;
  });
  function applyGain() {
    const g: Gain = isRtl ? { lna: Math.round(tunerDb * 10), vga: 0, amp: false } : { lna, vga, amp };
    gainDirty = false;
    run(api.setGain(g));
  }
  /** Eingabe verwerfen und wieder den Kern-Stand zeigen. */
  function resetGain() {
    gainDirty = false;
    lna = s.gain.lna;
    vga = s.gain.vga;
    amp = s.gain.amp;
    tunerDb = Math.round(s.gain.lna) / 10;
  }
  function clearMemory() {
    const st = ui.settings;
    if (!st) return;
    const next = { ...st.gain_by_channel };
    delete next[deviceKind];
    void patchSettings({ gain_by_channel: next });
  }

  async function pickRecordingDir() {
    const p = await api.pickDirectory(ui.settings?.recording_dir ?? null).catch((e) => (notify("warn", tError(e)), null));
    if (p) void patchSettings({ recording_dir: p });
  }

  const panelNames = ["presets", "services", "stations", "scan", "settings", "epg", "traffic", "timer", "music", "debug"] as const;
</script>

{#if ui.settings}
  {@const st = ui.settings}
  <section class="panel settings">
    <div class="panel-head"><span>{t("panel.settings")}</span></div>
    <div class="panel-body">
      <!-- Geraet / Empfang -->
      <div class="group">{t("settings.group_device")}</div>
      <div class="grid">
        <span class="lbl">{t("settings.device")}</span>
        <span class="row">
          <select value={st.device} onchange={(e) => patchSettings({ device: sel(e) })}>
            <option value="hackrf">{t("device.hackrf")}</option>
            <option value="rtlsdr">{t("device.rtlsdr")}</option>
            <option value="file">{t("device.file")}</option>
          </select>
          {#if st.device === "rtlsdr"}
            <span class="k">{t("settings.rtlsdr_index")}</span>
            <input type="number" class="short" min="0" max="9" value={st.rtlsdr_index} onchange={(e) => patchSettings({ rtlsdr_index: Math.max(0, num(e)) })} />
          {/if}
          {#if s.device}<span class="k">({deviceName(s.device.kind)} {s.device.name})</span>{/if}
        </span>

        <span class="lbl">{t("settings.ppm")}</span>
        <span class="row">
          <input type="number" class="ppm" value={st.ppm} min="-200" max="200" disabled={isFile} onchange={(e) => patchSettings({ ppm: num(e) })} />
          {#if isFile}<span class="k">{t("settings.gain_file_hint")}</span>{/if}
        </span>

        <span class="lbl">{t("settings.agc")}</span>
        <span class="row">
          <input type="checkbox" checked={st.agc} disabled={isFile} onchange={(e) => patchSettings({ agc: chk(e) })} />
          {#if isFile}<span class="k">{t("settings.gain_file_hint")}</span>{:else if st.agc}<span class="k">{t("settings.gain_agc_hint")}</span>{/if}
        </span>

        <span class="lbl">{t("settings.gain")}</span>
        <span class="row">
          {#if isRtl}
            <span class="k">{t("settings.tuner_gain")}</span>
            <input type="number" class="ppm" bind:value={tunerDb} min="0" max="50" step="0.1" disabled={gainLocked} oninput={() => (gainDirty = true)} />
          {:else}
            <span class="k">{t("settings.lna")}</span><input type="number" class="short" bind:value={lna} min="0" max="40" step="8" disabled={gainLocked} oninput={() => (gainDirty = true)} />
            <span class="k">{t("settings.vga")}</span><input type="number" class="short" bind:value={vga} min="0" max="62" step="2" disabled={gainLocked} oninput={() => (gainDirty = true)} />
            <label class="inl"><input type="checkbox" bind:checked={amp} disabled={gainLocked} oninput={() => (gainDirty = true)} />{t("settings.amp")}</label>
          {/if}
          <button class="btn" onclick={applyGain} disabled={gainLocked}>{t("settings.gain_apply")}</button>
          {#if gainDirty}<button class="btn mini" onclick={resetGain} title={t("settings.gain_reset_hint")}>{t("settings.gain_reset")}</button>{/if}
          <span class="k">{t("settings.gain_current", { lna: s.gain.lna, vga: s.gain.vga, amp: s.gain.amp ? "A" : "-" })}{s.agc ? " AGC" : ""}</span>
        </span>

        <span class="lbl">{t("settings.gain_memory")}</span>
        <span class="row">
          {#if isFile}
            <span class="k">{t("settings.gain_file_hint")}</span>
          {:else}
            <span class="k">
              {#if memory.entry}
                {t("settings.gain_memory_entry", { device: deviceName(deviceKind), channel: memory.channel, value: fmtGain(memory.entry, isRtl) })}
              {:else}
                {t("settings.gain_memory_none", { device: deviceName(deviceKind), channel: memory.channel })}
              {/if}
              · {t("settings.gain_memory_count", { n: memory.count })}
            </span>
            <button class="btn mini" onclick={clearMemory} disabled={memory.count === 0}>{t("settings.gain_memory_clear", { device: deviceName(deviceKind) })}</button>
          {/if}
        </span>
      </div>

      <!-- Audio -->
      <div class="group">{t("settings.group_audio")}</div>
      <div class="grid">
        <span class="lbl">{t("settings.audio_device")}</span>
        <select value={st.audio_device ?? ""} onchange={(e) => patchSettings({ audio_device: sel(e) === "" ? null : Number(sel(e)) })}>
          <option value="">{t("settings.audio_default")}</option>
          {#each s.audio_devices as name, i (i)}
            <option value={i}>{name}</option>
          {/each}
        </select>

        <span class="lbl">{t("settings.volume")}</span>
        <span class="row">
          <input type="number" class="short" min="0" max="100" step="5" value={st.volume_percent} onchange={(e) => patchSettings({ volume_percent: Math.max(0, Math.min(100, num(e))) })} />
          <span class="k">% · {t("settings.volume_hint")}</span>
        </span>
      </div>

      <!-- Notfallwarnung (Entscheidung 5) -->
      <div class="group">{t("settings.group_ews")}</div>
      <div class="grid">
        <span class="lbl">{t("settings.ews")}</span>
        <span><input type="checkbox" checked={st.ews_enabled} onchange={(e) => patchSettings({ ews_enabled: chk(e) })} /></span>
        <span class="lbl">{t("settings.ews_autoswitch")}</span>
        <span><input type="checkbox" checked={st.ews_autoswitch} disabled={!st.ews_enabled} onchange={(e) => patchSettings({ ews_autoswitch: chk(e) })} /></span>
        <span class="lbl">{t("alarm.beep")}</span>
        <span><input type="checkbox" checked={st.alarm_beep} disabled={!st.ews_enabled} onchange={(e) => patchSettings({ alarm_beep: chk(e) })} /></span>
      </div>

      <!-- EPG -->
      <div class="group">{t("settings.group_epg")}</div>
      <div class="grid">
        <span class="lbl">{t("settings.epg")}</span>
        <span class="row">
          <input type="checkbox" checked={st.epg_enabled} onchange={(e) => patchSettings({ epg_enabled: chk(e) })} />
          <span class="k">{t("settings.epg_hint")}</span>
        </span>
      </div>

      <!-- Aufnahme / Timer / Sleep (lib/timers.ts) -->
      <div class="group">{t("settings.group_recording")}</div>
      <div class="grid">
        <span class="lbl">{t("rec.dir")}</span>
        <span class="row">
          <input type="text" class="dir" value={st.recording_dir ?? ""} placeholder={t("rec.dir_default")} onchange={(e) => patchSettings({ recording_dir: (e.target as HTMLInputElement).value.trim() || null })} />
          <button class="btn mini" onclick={pickRecordingDir}>{t("settings.pick_dir")}</button>
          {#if st.recording_dir}<button class="btn mini" onclick={() => patchSettings({ recording_dir: null })}>{t("settings.dir_clear")}</button>{/if}
        </span>
        <span class="lbl">{t("settings.pre_post")}</span>
        <span class="row">
          <span class="k">{t("rec.pre")}</span><input type="number" class="ppm" value={Math.round(st.record_pre_s / 60)} min="0" max="60" onchange={(e) => patchSettings({ record_pre_s: Math.max(0, num(e)) * 60 })} />
          <span class="k">{t("rec.post")}</span><input type="number" class="ppm" value={Math.round(st.record_post_s / 60)} min="0" max="120" onchange={(e) => patchSettings({ record_post_s: Math.max(0, num(e)) * 60 })} />
        </span>
        <span class="lbl">{t("sleep.label")}</span>
        <span><SleepTimer /></span>
      </div>

      <!-- Timeshift (lib/timeshift.ts, Entscheidung 4) -->
      <div class="group">{t("settings.group_timeshift")}</div>
      <div class="grid">
        <span class="lbl">{t("ts.capacity")}</span>
        <span class="row">
          <input
            type="number"
            class="ppm"
            min={CAPACITY_MIN_MIN}
            max={CAPACITY_MAX_MIN}
            value={Math.round(st.timeshift_capacity_s / 60)}
            onchange={(e) => patchSettings({ timeshift_capacity_s: Math.max(CAPACITY_MIN_MIN, Math.min(CAPACITY_MAX_MIN, num(e))) * 60 })}
          />
          <span class="k">{t("ts.capacity_hint", { mb: Math.round((st.timeshift_capacity_s / 60) * 0.78) })}</span>
        </span>
      </div>

      <!-- Allgemein -->
      <div class="group">{t("settings.group_general")}</div>
      <div class="grid">
        <span class="lbl">{t("settings.language")}</span>
        <select value={st.language ?? ""} onchange={(e) => patchSettings({ language: sel(e) || null })}>
          <option value="">{t("settings.language_system")}</option>
          <option value="de">Deutsch</option>
          <option value="en">English</option>
        </select>

        <span class="lbl">{t("settings.autostart")}</span>
        <span class="row">
          <input type="checkbox" checked={st.autostart} onchange={(e) => patchSettings({ autostart: chk(e) })} />
          <span class="k">{t("settings.autostart_hint")}</span>
        </span>

        <span class="lbl">{t("settings.panels")}</span>
        <span class="row">
          {#each panelNames as name (name)}
            <label class="inl"><input type="checkbox" checked={st.panels[name]} onchange={(e) => patchSettings({ panels: { ...st.panels, [name]: chk(e) } })} />{t(`panel.${name}`)}</label>
          {/each}
        </span>

        <span class="lbl">{t("settings.data_dir")}</span>
        <span class="mono" title={ui.dataDir}>{ui.dataDir} <span class="k">({ui.portable ? t("settings.portable") : t("settings.profile")})</span></span>

        <span class="lbl">{t("core.version")}</span>
        <span class="mono">{s.core_version || "–"} <button class="btn mini" onclick={() => run(api.restartCore())}>{t("core.restart")}</button></span>
      </div>

      <!-- TII / Debug-Panel (lib/debug.ts, lib/tii.ts, Entscheidung 25) -->
      <div class="group">{t("settings.group_tii")}</div>
      <div class="grid">
        <span class="lbl">{t("tii.home")}</span>
        <span class="row">
          <span class="k">{t("tii.home_lat")}</span><input type="number" class="ppm" style="width:78px" step="0.0001" min="-90" max="90" value={st.home_lat ?? ""} placeholder="51.2180" onchange={(e) => setHome(e, "lat")} />
          <span class="k">{t("tii.home_lon")}</span><input type="number" class="ppm" style="width:78px" step="0.0001" min="-180" max="180" value={st.home_lon ?? ""} placeholder="6.7617" onchange={(e) => setHome(e, "lon")} />
          <span class="k">{t("tii.home_hint")}</span>
        </span>
        <span class="lbl"></span>
        <span class="row">
          <!-- Sichtbarer Zustand des Ortsabgleichs: ohne beide Koordinaten
               gilt jeder EWF-Alarm als relevant (Kern: relevant = null). -->
          {#if st.home_lat != null && st.home_lon != null}
            <span class="k homeok">{t("tii.home_status_set", { lat: st.home_lat.toFixed(4), lon: st.home_lon.toFixed(4) })}</span>
          {:else}
            <span class="k homewarn">{t("tii.home_status_unset")}</span>
          {/if}
        </span>

        <span class="lbl">{t("tii.home_code")}</span>
        <span class="row">
          <input
            type="text"
            class="ppm"
            style="width:150px"
            placeholder="1253-3513-3668"
            bind:value={homeCode}
            onkeydown={(e) => e.key === "Enter" && applyHomeCode()}
          />
          <button class="btn mini" onclick={applyHomeCode}>{t("tii.home_code_apply")}</button>
          <span class="k">{t("tii.home_code_hint")}</span>
        </span>

        <span class="lbl">{t("tii.enabled")}</span>
        <span class="row">
          <input type="checkbox" checked={st.tii_enabled} onchange={(e) => setTii({ enabled: chk(e) })} />
          <span class="k">{t("tii.threshold")}</span><input type="number" class="ppm" min="1" max="30" value={st.tii_threshold} onchange={(e) => setTii({ threshold: Math.max(1, Math.min(30, num(e))) })} />
          <label class="inl"><input type="checkbox" checked={st.tii_dx_mode} onchange={(e) => setTii({ dx_mode: chk(e) })} />{t("tii.dx_mode")}</label>
        </span>

        <span class="lbl">{t("debug.scope_rate")}</span>
        <span class="row">
          <select value={st.scope_rate_hz} onchange={(e) => run(debugApi.setRate(Number(sel(e))))}>
            {#each [1, 2, 3, 5, 8, 10] as r (r)}<option value={r}>{r} Hz</option>{/each}
          </select>
          <span class="k">{t("debug.scope_rate_hint")}</span>
        </span>
      </div>
    </div>
  </section>
{/if}

<style>
  .settings { flex: none; }
  .panel-body { display: flex; flex-direction: column; gap: 2px; }
  .group { margin-top: 6px; padding: 1px 0; border-bottom: 1px solid var(--chrome-lo); color: var(--amber, #e8b23a); font-size: 9px; font-weight: bold; letter-spacing: 0.08em; text-transform: uppercase; }
  .group:first-child { margin-top: 0; }
  .grid { display: grid; grid-template-columns: max-content 1fr; gap: 4px 10px; align-items: center; font-size: 10px; }
  .grid > .lbl { color: #7080a0; font-weight: bold; }
  .row { display: flex; align-items: center; gap: 4px; flex-wrap: wrap; }
  .short { width: 48px; }
  .ppm { width: 60px; }
  .dir { flex: 1 1 160px; min-width: 120px; }
  .k { color: var(--text-dim); font-size: 9px; }
  .k.homeok { color: var(--green); }
  .k.homewarn { color: var(--amber); }
  .inl { display: inline-flex; align-items: center; margin-left: 6px; }
  .mono { font-family: var(--mono); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .mini { font-size: 9px; min-height: 14px; padding: 0 5px; }
  input:disabled, select:disabled, button:disabled { opacity: 0.45; }
</style>
