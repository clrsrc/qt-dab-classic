<script lang="ts">
  // EPG-Panel (Entscheidung 9/21, Hotkey E): Tage heute..+5 (nur mit Daten),
  // Dienste mit Sendeplan (Standard: aktueller Dienst), Programmliste mit
  // Startzeit, Dauer, Titel, "laeuft"-Markierung und Fortschrittsbalken;
  // Klick oeffnet das Detail. Daten kommen aus dem Rust-Cache (epgApi).
  import { onMount, tick } from "svelte";
  import { dayOf, epgApi, fmtClock, fmtDayShort, fmtDuration, onEpgEvent, progressOf, upcomingDays, type EpgService, type Programme } from "$lib/epg";
  import { i18n, t } from "$lib/i18n.svelte";
  import { s, ui } from "$lib/state.svelte";
  import EpgProgrammeDetail from "./EpgProgrammeDetail.svelte";
  import Logo from "./Logo.svelte";

  const eid = $derived(s.ensemble?.eid ?? null);
  const offered = $derived.by(() => {
    void ui.now;
    return upcomingDays(6);
  });

  let available = $state<number[]>([]);
  let day = $state<number>(dayOf(new Date()));
  let services = $state<EpgService[]>([]);
  let sid = $state<number | null>(null);
  let programmes = $state<Programme[]>([]);
  let selected = $state<Programme | null>(null);
  let userPickedService = false;
  let listEl = $state<HTMLDivElement | null>(null);

  // Namen bevorzugt aus der aktuellen Senderliste (die Rust-Seite kennt sie u. U. noch nicht, wenn das Panel vor service_added laedt)
  const serviceOptions = $derived(services.map((x) => ({ sid: x.sid, name: s.services.find((y) => y.sid === x.sid)?.name.trim() || x.name })));
  const serviceName = $derived(serviceOptions.find((x) => x.sid === sid)?.name ?? (sid != null ? sid.toString(16).toUpperCase() : ""));
  const legacy = $derived(programmes.some((p) => p.legacy_time));

  async function loadDays() {
    if (eid == null) {
      available = [];
      services = [];
      programmes = [];
      return;
    }
    try {
      available = await epgApi.days(eid);
    } catch {
      available = [];
    }
    if (!available.includes(day)) {
      const first = offered.find((d) => available.includes(d));
      if (first != null) day = first;
    }
    await loadServices();
  }

  async function loadServices() {
    if (eid == null) return;
    try {
      services = await epgApi.services(eid, day);
    } catch {
      services = [];
    }
    const cur = s.current?.sid ?? null;
    if (!userPickedService && cur != null && services.some((x) => x.sid === cur)) sid = cur;
    else if (sid == null || !services.some((x) => x.sid === sid)) sid = services[0]?.sid ?? null;
    await loadProgrammes();
  }

  async function loadProgrammes() {
    if (eid == null || sid == null) {
      programmes = [];
      return;
    }
    try {
      programmes = await epgApi.programmes(eid, sid, day);
    } catch {
      programmes = [];
    }
    if (selected && !programmes.some((p) => p.start_unix === selected!.start_unix && p.medium_name === selected!.medium_name)) selected = null;
    await tick();
    scrollToRunning();
  }

  function scrollToRunning() {
    const el = listEl?.querySelector<HTMLElement>(".row.running");
    if (el) el.scrollIntoView({ block: "center" });
  }

  function pickDay(d: number) {
    day = d;
    selected = null;
    void loadServices();
  }
  function pickService(e: Event) {
    sid = Number((e.target as HTMLSelectElement).value);
    userPickedService = true;
    selected = null;
    void loadProgrammes();
  }
  function pick(p: Programme) {
    selected = selected && selected.start_unix === p.start_unix && selected.medium_name === p.medium_name ? null : p;
  }

  // Ensemble-Wechsel: alles neu; Dienstwechsel: Vorgabe folgt dem aktuellen Dienst
  $effect(() => {
    void eid;
    userPickedService = false;
    void loadDays();
  });
  $effect(() => {
    const cur = s.current?.sid;
    if (cur != null && !userPickedService && services.some((x) => x.sid === cur) && cur !== sid) {
      sid = cur;
      selected = null;
      void loadProgrammes();
    }
  });

  onMount(() =>
    onEpgEvent((ev) => {
      if (ev.type !== "epg_updated" || ev.eid !== eid) return;
      if (!available.includes(ev.day) || ev.day === day) void loadDays();
    }),
  );

  const past = (p: Programme) => p.start_unix * 1000 + p.duration_min * 60000 <= ui.now;
</script>

<section class="panel epg">
  <div class="panel-head">
    <span>{t("panel.epg")}</span>
    <span class="grow"></span>
    {#if legacy}<span class="legacy" title={t("epg.legacy_hint")}>{t("epg.legacy")}</span>{/if}
    {#if serviceName}<span>{serviceName}</span>{/if}
  </div>
  <div class="bar">
    <div class="days">
      {#each offered as d (d)}
        <button class="btn day" class:on={d === day} disabled={!available.includes(d)} onclick={() => pickDay(d)}>
          {d === offered[0] ? t("epg.today") : d === offered[1] ? t("epg.tomorrow") : fmtDayShort(d, i18n.lang)}
        </button>
      {/each}
    </div>
    <select class="svc" value={sid ?? ""} onchange={pickService} disabled={!services.length} title={t("epg.service")}>
      {#if !services.length}<option value="">{t("epg.no_services")}</option>{/if}
      {#each serviceOptions as svc (svc.sid)}
        <option value={svc.sid}>{svc.name}</option>
      {/each}
    </select>
  </div>
  <!-- Kompakte Shell: das Detail ersetzt die Liste, solange es offen ist (× oder erneuter Klick schliesst). -->
  {#if selected && sid != null}
    <div class="detailwrap">
      <Logo {eid} {sid} size="medium" name={serviceName} px={48} />
      <EpgProgrammeDetail programme={selected} {serviceName} {sid} onclose={() => (selected = null)} />
    </div>
  {/if}
  <div class="list" bind:this={listEl} hidden={!!selected}>
    {#if eid == null}
      <div class="empty">{t("epg.no_ensemble")}</div>
    {:else if !programmes.length}
      <div class="empty">{t("epg.no_data")}</div>
    {:else}
      {#each programmes as p (p.start_unix + ":" + p.medium_name)}
        {@const prog = progressOf(p.start_unix, p.duration_min, ui.now)}
        <button class="row" class:running={prog !== null} class:past={past(p)} class:active={selected === p} onclick={() => pick(p)}>
          <span class="time">{fmtClock(p.start_unix)}</span>
          <span class="dur">{fmtDuration(p.duration_min)}</span>
          <span class="grow title">
            {p.long_name || p.medium_name}
            {#if prog !== null}<span class="runmark">▶ {t("epg.running")}</span>{/if}
            {#if prog !== null}<span class="prog"><i style="width:{Math.round(prog * 100)}%"></i></span>{/if}
          </span>
          {#if p.genres.length}<span class="meta genre">{p.genres[0]}</span>{/if}
        </button>
      {/each}
    {/if}
  </div>
</section>

<style>
  .epg { display: flex; flex-direction: column; flex: 1 1 160px; min-height: 120px; }
  .bar { display: flex; gap: 4px; align-items: center; padding: 3px 4px 0; }
  .days { display: flex; gap: 2px; flex: 1; flex-wrap: wrap; }
  .day { font-size: 9px; padding: 1px 4px; min-height: 16px; }
  .svc { max-width: 140px; }
  .list { flex: 1; margin: 3px 4px; min-height: 60px; }
  .empty { padding: 8px; color: var(--green-dim); font-size: 10px; }
  .row { gap: 6px; padding: 1px 6px; font-size: 11px; }
  .row.past { color: #2f6a3a; }
  .row.past .meta { color: #24502e; }
  .row.running { color: var(--green-hi); background: #071a0c; }
  .time { font-family: var(--mono); min-width: 36px; }
  .dur { font-family: var(--mono); font-size: 9px; color: #2f8a4a; min-width: 30px; text-align: right; }
  .title { position: relative; padding-bottom: 1px; }
  .runmark { color: var(--amber); font-size: 9px; margin-left: 6px; }
  .prog { position: absolute; left: 0; right: 0; bottom: -1px; height: 2px; background: #10301a; display: block; }
  .prog i { display: block; height: 100%; background: var(--amber); }
  .genre { max-width: 90px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .legacy { color: var(--amber); font-weight: normal; letter-spacing: 0; text-transform: none; }
  .detailwrap { display: flex; align-items: flex-start; gap: 4px; padding-left: 4px; flex: 1; min-height: 0; overflow: auto; }
  .list[hidden] { display: none; }
  .detailwrap :global(.detail) { flex: 1; margin-left: 0; }
</style>
