<script lang="ts">
  import { api } from "$lib/core";
  import { t, tError } from "$lib/i18n.svelte";
  import { notify, patchSettings, s, ui } from "$lib/state.svelte";

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
  .k { color: var(--text-dim); font-size: 9px; }
  .inl { display: inline-flex; align-items: center; margin-left: 6px; }
  .mono { font-family: var(--mono); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .mini { font-size: 9px; min-height: 14px; padding: 0 5px; }
</style>
