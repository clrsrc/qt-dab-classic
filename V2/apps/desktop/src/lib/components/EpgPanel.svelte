<script lang="ts">
  // EPG-Panel (Entscheidung 9/21, Hotkey E): Uebersicht ueber ALLE Sender mit
  // Sendeplan (Stefan 29.09.2026). Zeilen = Sender mit Logo, Spalten =
  // Zeitstrahl des gewaehlten Tages (heute..+5, nur Tage mit Daten). Zuerst
  // die Favoriten in der Reihenfolge der Speicher, dann die uebrigen Sender;
  // wer keinen Sendeplan sendet, erscheint nicht. Reihenfolge und Daten
  // kommen fertig aus dem Rust-Cache (epgApi.grid, alle Ensembles); Klick auf
  // eine Sendung oeffnet das Detail.
  import { onMount, tick, untrack } from "svelte";
  import { dateOfDay, dayOf, epgApi, fmtClock, fmtDayShort, fmtDuration, onEpgEvent, upcomingDays, type EpgGridRow, type Programme } from "$lib/epg";
  import { i18n, t } from "$lib/i18n.svelte";
  import { s, ui } from "$lib/state.svelte";
  import EpgProgrammeDetail from "./EpgProgrammeDetail.svelte";
  import Logo from "./Logo.svelte";

  /** Breite der Senderspalte und Massstab des Zeitstrahls (150 px je Stunde). */
  const LEFT = 124;
  const PPM = 2.5;

  const offered = $derived.by(() => {
    void ui.now;
    return upcomingDays(6);
  });
  const today = $derived(offered[0]);

  let available = $state<number[]>([]);
  let day = $state<number>(dayOf(new Date()));
  let rows = $state<EpgGridRow[]>([]);
  let loaded = $state(false);
  let selected = $state<{ row: EpgGridRow; p: Programme } | null>(null);
  let gridEl = $state<HTMLDivElement | null>(null);
  let wantNow = true;
  let seq = 0;

  // Tagesgrenzen in Ortszeit; an Umstellungstagen ist der Tag 23 bzw. 25 h lang
  const dayStart = $derived(dateOfDay(day).getTime());
  const dayEnd = $derived.by(() => {
    const d = dateOfDay(day);
    return new Date(d.getFullYear(), d.getMonth(), d.getDate() + 1).getTime();
  });
  const dayMin = $derived((dayEnd - dayStart) / 60000);
  const hours = $derived(Array.from({ length: Math.ceil(dayMin / 60) }, (_, h) => dayStart / 1000 + h * 3600));
  const nowMin = $derived(ui.now >= dayStart && ui.now < dayEnd ? (ui.now - dayStart) / 60000 : null);
  const legacy = $derived(rows.some((r) => r.programmes.some((p) => p.legacy_time)));

  const keyOf = (p: Programme) => `${p.start_unix}:${p.medium_name}`;
  const endMs = (p: Programme) => p.start_unix * 1000 + p.duration_min * 60000;

  /** Lage einer Sendung auf dem Zeitstrahl, auf den Tag beschnitten. */
  function place(p: Programme): { left: number; width: number } {
    const a = Math.max(0, (p.start_unix * 1000 - dayStart) / 60000);
    const b = Math.min(dayMin, (endMs(p) - dayStart) / 60000);
    return { left: a * PPM, width: Math.max(2, (b - a) * PPM) };
  }

  async function load() {
    const my = ++seq;
    let days: number[] = [];
    let list: EpgGridRow[] = [];
    try {
      days = await epgApi.gridDays();
      if (!days.includes(day)) {
        const first = offered.find((d) => days.includes(d));
        if (first != null) day = first;
      }
      list = await epgApi.grid(day);
    } catch {
      list = [];
    }
    if (my !== seq) return;
    available = days;
    rows = list;
    loaded = true;
    if (selected) {
      const sel = selected;
      const row = list.find((r) => r.eid === sel.row.eid && r.sid === sel.row.sid);
      const p = row?.programmes.find((x) => keyOf(x) === keyOf(sel.p));
      selected = row && p ? { row, p } : null;
    }
    if (wantNow && list.length) {
      await tick();
      scrollToNow();
    }
  }

  // Das Karussell liefert die Sendeplaene in Schueben: Nachladen buendeln
  let timer: ReturnType<typeof setTimeout> | null = null;
  function reloadSoon() {
    if (timer) return;
    timer = setTimeout(() => {
      timer = null;
      void load();
    }, 400);
  }

  function scrollToNow() {
    const el = gridEl;
    if (!el || nowMin == null) return;
    wantNow = false;
    // aktuelle Uhrzeit im linken Viertel des sichtbaren Zeitstrahls
    el.scrollLeft = Math.max(0, nowMin * PPM - (el.clientWidth - LEFT) / 4);
  }

  function pickDay(d: number) {
    day = d;
    selected = null;
  }
  function jumpNow() {
    selected = null;
    wantNow = true;
    if (day !== today) day = today;
    else void tick().then(scrollToNow);
  }
  function pick(row: EpgGridRow, p: Programme) {
    selected = { row, p };
  }

  // Tag, Favoriten oder Senderliste geaendert: Zeilen und Reihenfolge neu
  $effect(() => {
    void day;
    void ui.presets;
    void s.stations;
    untrack(() => void load());
  });

  // Mausrad ohne senkrechten Ueberlauf blaettert im Zeitstrahl
  $effect(() => {
    const el = gridEl;
    if (!el) return;
    const onWheel = (e: WheelEvent) => {
      if (e.deltaX !== 0 || e.shiftKey || e.ctrlKey || el.scrollHeight > el.clientHeight) return;
      e.preventDefault();
      el.scrollLeft += e.deltaY;
    };
    el.addEventListener("wheel", onWheel, { passive: false });
    return () => el.removeEventListener("wheel", onWheel);
  });

  onMount(() => {
    const off = onEpgEvent((ev) => {
      if (ev.type === "epg_updated") reloadSoon();
    });
    return () => {
      off();
      if (timer) clearTimeout(timer);
    };
  });

  const isCurrent = (r: EpgGridRow) => !!s.current && s.current.sid === r.sid && s.ensemble?.eid === r.eid;
  const tip = (p: Programme) =>
    `${fmtClock(p.start_unix)}–${fmtClock(p.start_unix + p.duration_min * 60)} (${fmtDuration(p.duration_min)})\n${p.long_name || p.medium_name}${p.short_desc ? `\n${p.short_desc}` : ""}`;
</script>

<section class="panel epg">
  <div class="panel-head">
    <span>{t("panel.epg")}</span>
    <span class="grow"></span>
    {#if legacy}<span class="legacy" title={t("epg.legacy_hint")}>{t("epg.legacy")}</span>{/if}
    {#if rows.length}<span>{t("epg.stations", { n: rows.length })}</span>{/if}
  </div>
  <div class="bar">
    <div class="days">
      {#each offered as d (d)}
        <button class="btn day" class:on={d === day} disabled={!available.includes(d)} onclick={() => pickDay(d)}>
          {d === offered[0] ? t("epg.today") : d === offered[1] ? t("epg.tomorrow") : fmtDayShort(d, i18n.lang)}
        </button>
      {/each}
    </div>
    <button class="btn day" disabled={!available.includes(today)} title={t("epg.to_now")} onclick={jumpNow}>▸ {t("epg.now")}</button>
  </div>
  <div class="body">
    <div class="list grid" bind:this={gridEl}>
      {#if !rows.length}
        <div class="empty">{loaded ? t("epg.no_data") : ""}</div>
      {:else}
        <div class="sheet" style="width:{LEFT + dayMin * PPM}px">
          <div class="axis">
            <div class="corner"></div>
            {#each hours as h (h)}
              <span class="hour" style="left:{LEFT + ((h * 1000 - dayStart) / 60000) * PPM}px">{fmtClock(h)}</span>
            {/each}
          </div>
          {#each rows as r (r.eid + ":" + r.sid)}
            <div class="line" class:current={isCurrent(r)}>
              <div class="station" title={r.channel ? `${r.name} · ${r.channel}` : r.name}>
                <Logo eid={r.eid} sid={r.sid} size="small" name={r.name} px={28} />
                <span class="sname">{r.name}</span>
                {#if r.preset != null}<span class="slot" title={t("epg.favorite", { n: r.preset + 1 })}>{r.preset + 1}</span>{/if}
              </div>
              <div class="track" style="width:{dayMin * PPM}px;background-size:{60 * PPM}px 100%">
                {#each r.programmes as p (keyOf(p))}
                  {@const pos = place(p)}
                  <div
                    class="prog"
                    class:running={p.start_unix * 1000 <= ui.now && ui.now < endMs(p)}
                    class:past={endMs(p) <= ui.now}
                    style="left:{pos.left}px;width:{pos.width}px"
                    title={tip(p)}
                    role="button"
                    tabindex="0"
                    onclick={() => pick(r, p)}
                    onkeydown={(e) => e.key === "Enter" && pick(r, p)}
                  >
                    <span class="in"><span class="tm">{fmtClock(p.start_unix)}</span>{p.long_name || p.medium_name}</span>
                  </div>
                {/each}
              </div>
            </div>
          {/each}
          {#if nowMin != null}<div class="nowline" style="left:{LEFT + nowMin * PPM}px"></div>{/if}
        </div>
      {/if}
    </div>
    <!-- Kompakte Shell: das Detail liegt ueber dem Raster, solange es offen ist (× schliesst); das Raster behaelt seine Position. -->
    {#if selected}
      <div class="detailwrap">
        <Logo eid={selected.row.eid} sid={selected.row.sid} size="medium" name={selected.row.name} px={48} />
        <EpgProgrammeDetail
          programme={selected.p}
          serviceName={selected.row.name}
          sid={selected.row.sid}
          eid={selected.row.eid}
          channel={selected.row.channel}
          scids={selected.row.scids}
          onclose={() => (selected = null)}
        />
      </div>
    {/if}
  </div>
</section>

<style>
  .epg { display: flex; flex-direction: column; flex: 1 1 220px; min-height: 170px; }
  .bar { display: flex; gap: 4px; align-items: flex-start; padding: 3px 4px 0; }
  .days { display: flex; gap: 2px; flex: 1; flex-wrap: wrap; }
  .day { font-size: 9px; padding: 1px 4px; min-height: 16px; }
  .body { position: relative; flex: 1; min-height: 0; display: flex; margin: 3px 4px; }
  .grid { flex: 1; min-width: 0; min-height: 60px; }
  .empty { padding: 8px; color: var(--green-dim); font-size: 10px; }
  .legacy { color: var(--amber); font-weight: normal; letter-spacing: 0; text-transform: none; }

  .sheet { position: relative; }
  .axis { position: sticky; top: 0; z-index: 3; height: 15px; background: #10151c; border-bottom: 1px solid #1f3a26; }
  .corner { position: sticky; left: 0; z-index: 4; width: 124px; height: 100%; background: #10151c; border-right: 1px solid #1f3a26; }
  .hour { position: absolute; top: 0; padding-left: 3px; font-family: var(--mono); font-size: 9px; line-height: 14px; color: #2f8a4a; border-left: 1px solid #1f3a26; height: 100%; }

  .line { display: flex; height: 34px; border-bottom: 1px solid #0f2a16; }
  .station {
    position: sticky; left: 0; z-index: 2; flex: none; width: 124px; display: flex; align-items: center; gap: 4px; padding: 0 3px;
    background: var(--list); border-right: 1px solid #1f3a26; font-size: 10px; min-width: 0;
  }
  .line.current .station { background: var(--green-bg); color: #00ff50; }
  .sname { flex: 1; min-width: 0; overflow: hidden; display: -webkit-box; -webkit-line-clamp: 2; line-clamp: 2; -webkit-box-orient: vertical; line-height: 1.15; overflow-wrap: anywhere; }
  .slot { flex: none; font-family: var(--mono); font-size: 8px; color: var(--amber); align-self: flex-start; padding-top: 1px; }

  /* Stundenraster als Hintergrund der Spur */
  .track { position: relative; flex: none; background-image: linear-gradient(90deg, #0f2a16 1px, transparent 1px); }
  /* overflow: clip schneidet den Text ab, ohne den haftenden Titel vom Raster zu loesen */
  .prog {
    position: absolute; top: 1px; bottom: 1px; overflow: clip; cursor: pointer;
    background: #06140a; border-left: 1px solid #1f5a30; border-right: 1px solid #000; font-size: 10px; color: var(--green);
  }
  .prog:hover, .prog:focus-visible { background: #0e2a14; outline: none; }
  .prog.past { color: #2f6a3a; background: #040c06; border-left-color: #143a20; }
  .prog.running { color: var(--green-hi); background: #0a2a12; border-left-color: var(--amber); }
  /* Titel langer Sendungen bleibt beim Blaettern am linken Rand sichtbar */
  .in { position: sticky; left: 127px; display: inline-block; padding: 1px 3px; white-space: nowrap; line-height: 1.25; }
  .tm { display: block; font-family: var(--mono); font-size: 9px; color: #2f8a4a; }
  .prog.running .tm { color: var(--amber); }
  .nowline { position: absolute; top: 0; bottom: 0; width: 1px; background: var(--amber); z-index: 1; pointer-events: none; }

  .detailwrap { position: absolute; inset: 0; z-index: 5; display: flex; align-items: flex-start; gap: 4px; padding: 2px 0 0 2px; background: var(--chrome); overflow: auto; }
  .detailwrap :global(.detail) { flex: 1; margin-left: 0; }
</style>
