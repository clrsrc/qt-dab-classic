<script lang="ts">
  // Panel "Verkehr" (Auftrag 16.09.2026, TPEG 17.09.2026):
  // 1. Verkehrs- und sonstige Durchsagen (EN 300 401 8.1.6, FIG 0/18
  //    Ankuendigungs-Unterstuetzung + FIG 0/19 Umschaltung), protokolliert wie
  //    die EWF-Historie: laufende Durchsage oben, darunter die beendeten
  //    dieser Sitzung, neueste zuerst.
  // 2. TPEG-Verkehrsmeldungen (dab_app::tpeg): der Kern startet den TPEG-
  //    Datendienst des Ensembles ("ARD TPEG" auf WDR 11D/9A) im Hintergrund,
  //    die App dekodiert TEC-Ereignisse (Wirkung, Ursache, Zeiten, Laenge,
  //    Hinweise) und den Ort als OpenLR-Koordinaten. Strassennamen werden
  //    nicht gesendet - angezeigt werden Strassenart/-klasse, Laenge,
  //    Fahrtrichtung, Koordinaten und (mit Heimatkoordinaten) Entfernung.
  // Erreichbar ueber den Button "TA" neben EPG in Transport.svelte.
  import type { TrafficEntry, TpegEntry, TecCause } from "$lib/core";
  import { openUrl } from "@tauri-apps/plugin-opener";
  import { t, tError } from "$lib/i18n.svelte";
  import { notify, s, ui, patchSettings } from "$lib/state.svelte";
  import { playRecording, playback } from "$lib/playback.svelte";

  const KIND_KEYS = ["alarm", "road", "transport", "warning", "news", "weather", "event", "special", "programme", "sport", "financial"] as const;

  function kinds(flags: number): string[] {
    const out: string[] = [];
    KIND_KEYS.forEach((k, bit) => {
      if (flags & (1 << bit)) out.push(t(`traffic.kind.${k}`));
    });
    if (!out.length) out.push(t("traffic.kind.unknown"));
    return out;
  }

  const pad = (n: number) => String(n).padStart(2, "0");

  function fmtWhen(unix: number): string {
    const d = new Date(unix * 1000);
    return `${pad(d.getDate())}.${pad(d.getMonth() + 1)}. ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
  }

  /** Kurzform fuer TPEG-Zeiten: heute nur Uhrzeit, sonst Tag.Monat. Uhrzeit. */
  function fmtShort(unix: number): string {
    const d = new Date(unix * 1000);
    const now = new Date();
    const hm = `${pad(d.getHours())}:${pad(d.getMinutes())}`;
    if (d.toDateString() === now.toDateString()) return hm;
    return `${pad(d.getDate())}.${pad(d.getMonth() + 1)}. ${hm}`;
  }

  function fmtDuration(e: TrafficEntry): string {
    const end = e.ended_at ?? Math.floor(Date.now() / 1000);
    const sec = Math.max(0, end - e.started_at);
    return sec >= 60 ? `${Math.floor(sec / 60)}:${String(sec % 60).padStart(2, "0")} min` : `${sec} s`;
  }

  const entries = $derived<TrafficEntry[]>(s.traffic_active ? [s.traffic_active, ...s.traffic_history] : s.traffic_history);
  const supported = $derived(s.traffic_supported);

  // ---- TPEG ---------------------------------------------------------------
  const tpegOn = $derived(ui.settings?.tpeg_enabled ?? true);

  function tk(prefix: string, code: number | null | undefined, fallback = ""): string {
    if (code === null || code === undefined) return fallback;
    const key = `${prefix}.${code}`;
    const v = t(key);
    return v === key ? `${fallback}${fallback ? " " : ""}${code}` : v;
  }

  function causeText(c: TecCause): string {
    let out = tk("tpeg.cause", c.main, t("tpeg.cause_unknown"));
    if (c.sub !== null && c.sub !== undefined && c.sub !== 0) {
      const key = `tpeg.sub.${c.main}.${c.sub}`;
      const v = t(key);
      if (v !== key) out += `: ${v}`;
    }
    if (c.lane_restriction) {
      out += ` – ${tk("tpeg.lane", c.lane_restriction)}${c.lanes ? ` (${c.lanes})` : ""}`;
    }
    if (c.length_m) out += ` – ${fmtLength(c.length_m)}`;
    if (c.text.length) out += ` – ${c.text.join(" / ")}`;
    if (c.unverified) out += ` (${t("tpeg.unverified")})`;
    return out;
  }

  function fmtLength(m: number): string {
    return m >= 1000 ? `${(m / 1000).toFixed(m >= 10000 ? 0 : 1)} km` : `${m} m`;
  }

  function roadText(m: TpegEntry): string {
    const parts: string[] = [];
    if (m.fow !== null && m.fow !== undefined && m.fow !== 0) parts.push(tk("tpeg.fow", m.fow));
    if (m.frc !== null && m.frc !== undefined) parts.push(tk("tpeg.frc", m.frc));
    if (m.bearing_deg !== null && m.bearing_deg !== undefined) parts.push(`→ ${compass(m.bearing_deg)}`);
    return parts.join(" · ");
  }

  function compass(deg: number): string {
    return t(`tpeg.compass.${Math.round(deg / 45) % 8}`);
  }

  function coordText(m: TpegEntry): string {
    if (m.lat === null || m.lat === undefined || m.lon === null || m.lon === undefined) return "";
    return `${m.lat.toFixed(3)}° N ${m.lon.toFixed(3)}° O`;
  }

  function timeText(m: TpegEntry): string {
    if (m.start_unix && m.stop_unix) return `${fmtShort(m.start_unix)} – ${fmtShort(m.stop_unix)}`;
    if (m.start_unix) return t("tpeg.from", { time: fmtShort(m.start_unix) });
    if (m.stop_unix) return t("tpeg.until", { time: fmtShort(m.stop_unix) });
    return "";
  }

  function adviceText(m: TpegEntry): string {
    return m.advices
      .map((a) => {
        let s = tk("tpeg.advice", a.code, "");
        if (a.text.length) s += (s ? ": " : "") + a.text.join(" / ");
        return s;
      })
      .filter(Boolean)
      .join("; ");
  }

  function mapUrl(m: TpegEntry): string | null {
    if (m.lat === null || m.lat === undefined || m.lon === null || m.lon === undefined) return null;
    return `https://www.openstreetmap.org/?mlat=${m.lat.toFixed(5)}&mlon=${m.lon.toFixed(5)}#map=14/${m.lat.toFixed(5)}/${m.lon.toFixed(5)}`;
  }

  function tpegStatusText(): string {
    const st = s.tpeg;
    if (!tpegOn) return t("traffic.tpeg_off");
    if (!st.available) return t("traffic.tpeg_none");
    if (!st.last_unix) return t("traffic.tpeg_waiting", { name: st.service_name || "TPEG" });
    const d = new Date(st.last_unix * 1000);
    return t("traffic.tpeg_status", {
      name: st.service_name || "TPEG",
      version: st.tec_version || "TEC",
      count: st.messages.length,
      time: `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`,
    });
  }

  let filter = $state("");
  const tpegList = $derived.by<TpegEntry[]>(() => {
    const f = filter.trim().toLowerCase();
    const all = s.tpeg.messages;
    if (!f) return all;
    return all.filter((m) => {
      const hay = [tk("tpeg.effect", m.effect), ...m.causes.map(causeText), roadText(m), adviceText(m), coordText(m)].join(" ").toLowerCase();
      return hay.includes(f);
    });
  });
</script>

<section class="panel traffic">
  <div class="panel-head">
    <span>{t("panel.traffic")}</span>
    <span class="grow"></span>
    <span class="k" title={t("traffic.support_hint")}>{supported ? t("traffic.supported") : t("traffic.unsupported")}</span>
    <label class="inl" title={t("traffic.record_hint")}>
      <input type="checkbox" checked={ui.settings?.announcement_record ?? true} onchange={(e) => void patchSettings({ announcement_record: (e.currentTarget as HTMLInputElement).checked })} />
      {t("traffic.record")}
    </label>
    <label class="inl" title={t("traffic.autoswitch_hint")}>
      <input type="checkbox" checked={ui.settings?.traffic_autoswitch ?? false} onchange={(e) => void patchSettings({ traffic_autoswitch: (e.currentTarget as HTMLInputElement).checked })} />
      {t("traffic.autoswitch")}
    </label>
  </div>
  <div class="list ann">
    {#if !entries.length}
      <div class="row"><span class="grow dim">{t("traffic.empty")}</span></div>
    {/if}
    {#each entries as e (e.id)}
      <div class="entry" class:active={e.ended_at === null}>
        <div class="head">
          <span class="when">{fmtWhen(e.started_at)}</span>
          <span class="svc">{e.announcing_service ?? `SubCh ${e.sub_ch}`}</span>
          {#each kinds(e.flags) as k (k)}<span class="flag">{k}</span>{/each}
          {#if e.ended_at === null}<span class="flag live">{t("traffic.running")}</span>{/if}
          {#if e.switched}<span class="flag sw" title={t("traffic.switched_hint")}>{t("traffic.switched")}</span>{/if}
          <span class="dur">{fmtDuration(e)}</span>
          {#if e.file && e.ended_at !== null}
            <button class="btn mini play" class:on={playback.path === e.file} title={e.file} onclick={() => void playRecording(e.file!, `${e.announcing_service ?? ""} ${fmtWhen(e.started_at)}`)}>▶ {t("traffic.play")}</button>
          {:else if e.file}
            <span class="flag rec">{t("traffic.recording")}</span>
          {/if}
        </div>
        <div class="meta">
          {e.channel} · {e.ensemble} · {t("traffic.cluster", { cluster: e.cluster })}
          {#if e.services.length}· {t("traffic.for")} {e.services.join(", ")}{/if}
        </div>
      </div>
    {/each}
  </div>

  <!-- TPEG-Verkehrsmeldungen (dab_app::tpeg) -->
  <div class="panel-head sub">
    <span>{t("traffic.tpeg_title")}</span>
    <span class="k grow" title={s.tpeg.provider ? `${s.tpeg.description} · ${s.tpeg.provider}` : t("traffic.tpeg_hint")}>{tpegStatusText()}</span>
    {#if tpegOn && s.tpeg.messages.length}
      <input class="filter" type="search" placeholder={t("traffic.tpeg_filter")} bind:value={filter} />
    {/if}
    <label class="inl" title={t("settings.tpeg_hint")}>
      <input type="checkbox" checked={tpegOn} onchange={(e) => void patchSettings({ tpeg_enabled: (e.currentTarget as HTMLInputElement).checked })} />
      TPEG
    </label>
  </div>
  <div class="list tpeg">
    {#if tpegOn && s.tpeg.available && s.tpeg.last_unix && !s.tpeg.messages.length}
      <div class="row"><span class="grow dim">{t("traffic.tpeg_empty")}</span></div>
    {/if}
    {#each tpegList as m (m.id)}
      <div class="entry" class:severe={m.effect >= 6} class:closed={m.effect === 7} class:expired={m.expiry_unix * 1000 < Date.now()}>
        <div class="head">
          <span class="eff">{tk("tpeg.effect", m.effect, t("tpeg.effect_unknown"))}</span>
          {#each m.causes as c, i (i)}<span class="cause">{causeText(c)}</span>{/each}
          {#if m.length_m}<span class="flag">{fmtLength(m.length_m)}</span>{/if}
          {#if m.delay_min}<span class="flag warn">+{m.delay_min} min</span>{/if}
          {#if m.speed_kmh}<span class="flag">{m.speed_kmh} km/h</span>{/if}
          {#if m.tendency}<span class="flag">{tk("tpeg.tendency", m.tendency)}</span>{/if}
          {#if m.distance_km !== null && m.distance_km !== undefined && m.direction_deg !== null && m.direction_deg !== undefined}
            <span class="dist">{m.distance_km} km {compass(m.direction_deg)}</span>
          {/if}
        </div>
        <div class="meta">
          {#if roadText(m)}{roadText(m)} · {/if}{#if timeText(m)}{timeText(m)} · {/if}{#if adviceText(m)}{adviceText(m)} · {/if}
          {#if m.location_text.length}{m.location_text.join(" / ")} · {/if}
          {#if mapUrl(m)}
            <button class="lnk" title={t("traffic.tpeg_map_hint")} onclick={() => openUrl(mapUrl(m)!).catch((x: unknown) => notify("warn", tError(x)))}>{coordText(m)}</button>
          {:else if m.tmc_code}
            TMC {m.tmc_code}
          {/if}
          <span class="id" title="messageID/version · {t('tpeg.expires')} {fmtShort(m.expiry_unix)}">#{m.id}/{m.version}</span>
        </div>
      </div>
    {/each}
  </div>
  <div class="foot dim">{t("traffic.tpeg_hint")}{#if tpegOn && s.tpeg.available && !s.tpeg.home_known} {t("traffic.tpeg_home_hint")}{/if}</div>
</section>

<style>
  .traffic { display: flex; flex-direction: column; flex: 1 1 120px; min-height: 80px; }
  .panel-head { display: flex; align-items: center; gap: 8px; }
  .panel-head.sub { border-top: 1px solid #14261a; margin-top: 2px; padding-top: 2px; }
  .grow { flex: 1; }
  .k { color: var(--text-dim); font-size: 9px; }
  .k.grow { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .inl { display: inline-flex; align-items: center; gap: 3px; font-size: 9px; color: var(--text-dim); }
  .filter { width: 90px; font-size: 9px; background: #06110a; color: var(--text); border: 1px solid #14261a; padding: 0 3px; }
  .list { margin: 3px 4px; overflow: auto; }
  .list.ann { flex: 0 1 auto; max-height: 40%; }
  .list.tpeg { flex: 1 1 60px; }
  .entry { padding: 3px 4px; font-size: 10px; border-bottom: 1px solid #14261a; }
  .entry:last-child { border-bottom: none; }
  .entry.active { background: #0e2a12; }
  .entry.severe .eff { color: var(--amber); }
  .entry.closed .eff { color: var(--red, #e04040); }
  .entry.expired { opacity: 0.55; }
  .head { display: flex; align-items: center; gap: 6px; flex-wrap: wrap; }
  .when { font-family: var(--mono); color: var(--text-dim); }
  .svc { font-weight: bold; }
  .eff { font-weight: bold; }
  .dur, .dist { margin-left: auto; font-family: var(--mono); color: var(--text-dim); white-space: nowrap; }
  .flag { font-size: 8px; font-family: var(--mono); border: 1px solid currentColor; padding: 0 2px; color: var(--amber); }
  .flag.live { color: var(--green); }
  .flag.sw { color: var(--text-dim); }
  .flag.rec { color: var(--red, #e04040); }
  .flag.warn { color: var(--red, #e04040); }
  .play { margin-left: 4px; }
  .meta { margin-top: 2px; font-size: 9px; color: var(--text-dim); }
  .lnk { background: none; border: none; padding: 0; font: inherit; color: var(--text-dim); text-decoration: underline dotted; cursor: pointer; }
  .lnk:hover { color: var(--text); }
  .id { float: right; font-family: var(--mono); opacity: 0.6; }
  .dim { color: var(--green-dim); }
  .row { gap: 5px; padding: 1px 4px; font-size: 10px; }
  .foot { font-size: 9px; padding: 2px 6px; border-top: 1px solid #14261a; }
</style>
