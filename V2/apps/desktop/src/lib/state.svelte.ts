// Zentraler Store: Spiegel des Rust-Zustands ("Truth" in dab-app::state).
// Initial per Snapshot (`get_state`), danach werden dieselben Delta-Ereignisse
// angewendet, die auch die Rust-Seite verarbeitet. Komponenten lesen nur
// hieraus und rufen `api` fuer Aktionen.

import { api, type AppEvent, type AppState, type CoreEvent, type Presets, type ServiceInfo, type Settings } from "./core";
import { applyDebugAppEvent, emptyDebugState, feedScopeEvent } from "./debug";
import { emitEpgEvent } from "./epg";
import { setLang, t, tError } from "./i18n.svelte";
import { applyRecordingState, applyTimerAppEvent, initTimers } from "./timers.svelte";
import { emptyTimeshift } from "./timeshift";

export function emptyState(): AppState {
  return {
    core_alive: false,
    core_version: "",
    core_restarts: 0,
    device: null,
    device_error: null,
    channel: null,
    synced: false,
    snr: 0,
    fic_ok: 0,
    fic_total: 0,
    ensemble: null,
    services: [],
    current: null,
    dls: "",
    dl_plus: null,
    slide: null,
    slides: [],
    level: [0, 0],
    gain: { lna: 40, vga: 40, amp: false },
    agc: true,
    volume: 70,
    muted: false,
    scan: { active: false, channel: "", index: 0, total: 0, results: [] },
    ews_present: false,
    alert: null,
    ews_switched_from: null,
    recording: false,
    file: null,
    audio_devices: [],
    audio_device_current: null,
    pending: null,
    clock_utc: null,
    log_tail: [],
    logo_data_url: null,
    now_next: null,
    tii: [],
    debug: emptyDebugState(),
    stations: [],
    timeshift: emptyTimeshift(),
    music_candidates: [],
    ews_history: [],
    traffic_active: null,
    traffic_history: [],
    traffic_supported: false,
    radiodns: { enabled: false, busy: false, eid: 0, services_found: 0, services_none: 0, logos: 0, schedules: 0, last_unix: 0, error: null },
    tpeg: { enabled: true, available: false, sid: 0, service_name: "", description: "", provider: "", tec_version: "", last_unix: 0, groups: 0, frames: 0, tfp_seen: false, home_known: false, messages: [] },
    storage: { recording_dir: "", announcement_dir: "", recordings: { files: 0, bytes: 0 }, announcements: { files: 0, bytes: 0 } },
  };
}

export interface Notice {
  id: number;
  level: "info" | "warn" | "error";
  text: string;
  at: number;
}

export const s = $state<AppState>(emptyState());

export const ui = $state({
  presets: { version: 1, slots: Array<null>(10).fill(null) } as Presets,
  settings: null as Settings | null,
  dataDir: "",
  portable: false,
  notices: [] as Notice[],
  /** Letzter Preset-Status (Zeile in der Statusleiste). */
  presetStatus: null as { slot: number | null; status: string; name: string; channel: string; at: number } | null,
  /** Sekundentakt fuer Uhr/MOT-Frische. */
  now: Date.now(),
  /** Kern hat fuer den aktuellen Kanal `no_signal` gemeldet (bis zum naechsten Sync). */
  noSignal: false,
  ready: false,
});

let noticeId = 0;
export function notify(level: Notice["level"], text: string, ttlMs = 4000) {
  const n: Notice = { id: ++noticeId, level, text, at: Date.now() };
  ui.notices = [...ui.notices, n].slice(-3);
  setTimeout(() => {
    ui.notices = ui.notices.filter((x) => x.id !== n.id);
  }, ttlMs);
}

// ---------------------------------------------------------------------------
// Ableitungen
// ---------------------------------------------------------------------------

export function currentService(): ServiceInfo | null {
  const c = s.current;
  if (!c) return null;
  return s.services.find((x) => x.sid === c.sid && x.scids === c.scids) ?? null;
}

export function activePresetSlot(): number | null {
  const c = s.current;
  const ch = s.channel;
  if (!c || !ch) return null;
  const i = ui.presets.slots.findIndex(
    (p) => p && p.sid === c.sid && p.scids === c.scids && p.channel.toUpperCase() === ch.toUpperCase(),
  );
  return i >= 0 ? i : null;
}

export function slideUrl(): string | null {
  const sl = s.slide;
  return sl ? `data:${sl.mime || "image/jpeg"};base64,${sl.data_b64}` : null;
}

export function motFresh(): boolean {
  return !!s.slide && ui.now / 1000 - s.slide.received_at < 60;
}

/** Laeuft gerade ein Alarm, der uns betrifft? `relevant === false` heisst:
 * der Kern hat den Alarm per Geofencing (Heimatkoordinaten, ETSI TS 104 089)
 * als ortsfremd eingestuft - er wird weiter mitgeschrieben (EWF-Historie),
 * aber nicht als Alarm dargestellt (kein Banner, keine rote LED, kein
 * Alarmfenster; letzteres entscheidet die Rust-Seite in timer_cmds.rs). */
export function alertActive(): boolean {
  const a = s.alert;
  return !!a && (a.phase === "trigger" || a.phase === "sustain") && a.relevant !== false;
}

export function dlPlusTitle(): { title: string; artist: string } | null {
  const d = s.dl_plus;
  if (!d) return null;
  const tag = (t: number) => d.tags.find((x) => x[0] === t)?.[1] ?? "";
  const title = tag(1);
  const artist = tag(4) || tag(9) || tag(8);
  if (!title && !artist) return null;
  return { title, artist };
}

// ---------------------------------------------------------------------------
// Reducer (spiegelt dab-app::state::AppState::apply)
// ---------------------------------------------------------------------------

function sortServices(list: ServiceInfo[]) {
  list.sort((a, b) => a.name.trim().toLowerCase().localeCompare(b.name.trim().toLowerCase()) || a.scids - b.scids);
}

function clearService() {
  s.current = null;
  s.dls = "";
  s.dl_plus = null;
  s.slide = null;
  s.slides = [];
  s.level = [0, 0];
  s.logo_data_url = null;
  s.now_next = null;
}

/** Spiegelt dab-app::timeshift: Kern leert den Ring, Anzeige zurueck auf live. */
function resetTimeshift() {
  if (s.timeshift.demo) return;
  s.timeshift = { ...s.timeshift, mode: "live", buffered_s: 0, offset_s: 0, live_unix: 0 };
}

function clearReception() {
  s.synced = false;
  ui.noSignal = false;
  s.snr = 0;
  s.fic_ok = 0;
  s.fic_total = 0;
  s.ensemble = null;
  s.services = [];
  resetTimeshift();
  clearService();
}

export function applyCoreEvent(ev: CoreEvent) {
  const e = ev as Record<string, any>;
  switch (ev.type) {
    case "ready":
      s.core_alive = true;
      s.core_version = String(e.core_version);
      break;
    case "device_opened":
      s.device = { kind: s.device?.kind ?? "hackrf", name: e.name, serial: e.serial, clock: null };
      s.device_error = null;
      break;
    // Referenztakt (HackRF CLKIN/GPSDO), kommt nach jedem Kanalstart.
    case "clock_source":
      if (s.device) s.device = { ...s.device, clock: String(e.source) };
      break;
    // Befund 3 (Review 2026-09-16): die Rust-Seite loescht ihr `pending` bei
    // device_closed/device_error/exiting still (app.rs), ohne preset_status -
    // hier nachziehen, sonst blinkt "Suche ..." fuer immer.
    case "device_closed":
      s.device = null;
      s.file = null;
      s.pending = null;
      clearReception();
      break;
    case "device_error":
      s.device_error = e.message;
      s.pending = null;
      break;
    case "gain_changed":
      s.gain = { lna: e.lna, vga: e.vga, amp: e.amp };
      s.agc = e.agc;
      break;
    case "file_progress":
      if (s.file) {
        s.file.position_s = e.position_s;
        s.file.length_s = e.length_s;
      } else {
        s.file = { path: "", loop: false, position_s: e.position_s, length_s: e.length_s, ended: false };
      }
      break;
    case "file_ended":
      if (s.file) s.file.ended = true;
      break;
    case "synced":
      s.synced = e.synced;
      if (e.synced) ui.noSignal = false;
      break;
    case "no_signal":
      s.synced = false;
      s.channel = e.channel;
      ui.noSignal = true;
      break;
    case "snr":
      s.snr = e.db;
      break;
    case "fic_quality":
      s.fic_ok = e.ok;
      s.fic_total = e.total;
      break;
    case "ensemble_found": {
      if (!s.ensemble || s.ensemble.eid !== e.eid) {
        s.services = [];
        clearService();
      }
      s.ensemble = { eid: e.eid, name: e.name, channel: e.channel };
      s.channel = e.channel;
      break;
    }
    case "service_added": {
      const svc = e.service as ServiceInfo;
      const list = s.services.filter((x) => !(x.sid === svc.sid && x.scids === svc.scids));
      list.push(svc);
      sortServices(list);
      s.services = list;
      break;
    }
    case "ensemble_reconfigured":
      s.services = [];
      break;
    case "clock_time":
      s.clock_utc = e.unix_utc;
      break;
    case "service_started":
      if (e.slot === "primary") {
        const same = s.current && s.current.sid === e.sid && s.current.scids === e.scids;
        resetTimeshift();
        if (!same) clearService();
        s.current = { sid: e.sid, scids: e.scids, codec: e.codec, stereo: e.stereo };
      }
      break;
    case "service_stopped":
      if (e.slot === "primary") {
        resetTimeshift();
        // Befund 8: liefert der Kern (kuenftig) `scids`, gilt der Stop nur fuer
        // genau diese Komponente - ein verspaeteter Stop von scids=0 darf die
        // schon laufende scids=1 desselben Dienstes nicht loeschen.
        const sameComponent = typeof e.scids !== "number" || s.current?.scids === e.scids;
        if (s.current?.sid === e.sid && sameComponent) clearService();
      }
      break;
    case "dls":
      if (e.slot === "primary") s.dls = e.text;
      break;
    case "dl_plus":
      if (e.slot === "primary") s.dl_plus = { item_running: e.item_running, item_toggle: e.item_toggle, tags: e.tags };
      break;
    case "mot_slide":
      if (e.slot === "primary") {
        s.slide = { sid: e.sid, mime: e.mime, name: e.name, data_b64: e.data_b64, received_at: Math.floor(Date.now() / 1000) };
        // Bilderstreifen im Display (Stefan 16.09.2026): jedes neue Bild
        // rechts anhaengen, bis 5 verschiedene da sind, dann rutscht das
        // aelteste links raus. Gleicher Inhalt (Wiederholung im Carousel)
        // zaehlt nicht als neu, sondern wandert nur ans rechte Ende.
        const same = s.slides.findIndex((x) => x.data_b64 === e.data_b64);
        if (same >= 0) s.slides.splice(same, 1);
        s.slides = [...s.slides, s.slide].slice(-5);
      }
      break;
    case "audio_level":
      s.level = [e.left, e.right];
      break;
    case "audio_devices":
      s.audio_devices = e.devices;
      s.audio_device_current = e.current ?? null;
      break;
    case "ews_present":
      s.ews_present = true;
      break;
    case "ews_alert":
      if (e.phase === "end") {
        // Alarmfenster (timer_cmds) geht zu; Hinweis im Hauptfenster (Entscheidung 5).
        // Ortsfremde Alarme (Geofencing, relevant === false) sind nie aufgepoppt,
        // darum auch kein Ende-Hinweis - sonst meldete sich der Eiffelturm-Test
        // alle fuenf Minuten.
        if (s.alert && s.alert.phase !== "pre_trigger" && s.alert.relevant !== false) notify("info", t("alarm.ended"), 8000);
        s.alert = null;
        s.ews_switched_from = null;
      } else {
        const dismissed = !!s.alert && s.alert.dismissed && s.alert.iid === e.iid && s.alert.sub_ch === e.sub_ch;
        s.alert = { phase: e.phase, sub_ch: e.sub_ch, stage: e.stage, stage_raw: e.stage_raw ?? 0, iid: e.iid, locations: e.locations, location_info: [], is_test: e.is_test, relevant: e.relevant ?? null, dismissed };
      }
      break;
    case "ews_switched":
      s.ews_switched_from = e.from_sid ?? null;
      break;
    case "recording_state":
      if (e.slot === "primary") {
        s.recording = e.active;
        applyRecordingState(e);
      }
      break;
    // Timeshift (lib/timeshift.ts): Zustand kommt nur vom Kern
    case "timeshift_state":
      if (!s.timeshift.demo) {
        s.timeshift = {
          mode: e.mode,
          buffered_s: e.buffered_s,
          offset_s: e.offset_s,
          capacity_s: e.capacity_s > 0 ? e.capacity_s : s.timeshift.capacity_s,
          live_unix: e.live_unix ?? 0,
          frame_index: e.frame_index || s.timeshift.frame_index,
          demo: false,
        };
      }
      break;
    case "scan_progress":
      if (!s.scan.active) s.scan.results = [];
      clearReception();
      s.scan.active = true;
      s.scan.channel = e.channel;
      s.scan.index = e.index;
      s.scan.total = e.total;
      s.channel = e.channel;
      break;
    case "scan_result":
      s.scan.results = [
        ...s.scan.results.filter((r) => r.channel !== e.channel),
        { channel: e.channel, eid: e.eid ?? null, ensemble: e.ensemble ?? null, services: e.services, snr: e.snr },
      ];
      break;
    case "scan_finished":
      s.scan.active = false;
      break;
    // Scope-Rohdaten (lib/debug.ts): nicht in den Store, direkt an die Canvas-Komponenten
    case "spectrum":
    case "iq_samples":
      feedScopeEvent(ev);
      break;
    case "log":
      if (e.level === "error" || e.level === "warn") {
        s.log_tail = [...s.log_tail, String(e.text)].slice(-20);
      }
      break;
    case "exiting":
      s.core_alive = false;
      s.device = null;
      s.scan.active = false;
      s.pending = null;
      clearReception();
      break;
  }
}

export function applyAppEvent(ev: AppEvent) {
  switch (ev.type) {
    case "preset_status":
      ui.presetStatus = { ...ev, at: Date.now() };
      if (ev.status === "tuning") {
        s.pending = { slot: ev.slot, channel: ev.channel, name: ev.name };
      } else {
        s.pending = null;
      }
      break;
    case "presets_changed":
      ui.presets = ev.presets;
      break;
    case "settings_changed":
      ui.settings = ev.settings;
      setLang(ev.settings.language);
      break;
    case "core_restarted":
      s.core_restarts = ev.attempt;
      notify("warn", `core_restarted:${ev.reason}:${ev.attempt}`, 8000);
      break;
    case "notice":
      notify(ev.level, ev.text, 6000);
      break;
    // EPG/Logos (lib/epg.ts): Display-Felder spiegeln, Panels benachrichtigen
    case "current_media":
      s.logo_data_url = ev.logo_data_url;
      s.now_next = ev.now_next;
      emitEpgEvent(ev);
      break;
    case "epg_updated":
    case "logo_updated":
      emitEpgEvent(ev);
      break;
    // TII/Debug-Panel (lib/debug.ts)
    case "tii_updated":
    case "debug_stats":
      applyDebugAppEvent(ev);
      break;
    // Senderliste (lib/stations.ts)
    case "stations_changed":
      s.stations = ev.stations;
      break;
    // Timeshift (lib/timeshift.ts): Puffer verworfen (Alarm, Dienstwechsel)
    case "timeshift_notice":
      s.timeshift = { ...s.timeshift, mode: "live", buffered_s: 0, offset_s: 0, live_unix: 0 };
      notify("info", t(`ts.notice.${ev.notice}`), 8000);
      break;
    // Musik-Trennung (lib/music.ts): Vorschlagsliste geaendert
    case "music_candidates":
      s.music_candidates = ev.candidates;
      break;
    // EWS-Ortscodes uebersetzt (dab_app::ews_location); kommt kurz nach dem
    // rohen "ews_alert" nach, gegen iid/sub_ch geprueft, falls inzwischen
    // schon ein neuerer Alarm lief.
    case "ews_locations":
      if (s.alert && s.alert.iid === ev.iid && s.alert.sub_ch === ev.sub_ch) {
        s.alert = { ...s.alert, location_info: ev.location_info };
      }
      break;
    // EWF-Historie (Bugfixes.txt #10): kommt nach, wenn ein Alarm endet.
    case "ews_history":
      s.ews_history = ev.history;
      break;
    // Verkehrs-/Sonderdurchsagen (dab_app::traffic): laufende, Historie, Unterstuetzung.
    case "traffic":
      s.traffic_active = ev.active;
      s.traffic_history = ev.history;
      s.traffic_supported = ev.supported;
      break;
    // Hybrid Radio / RadioDNS (dab_app::radiodns): Status des Abrufs.
    case "radiodns":
      s.radiodns = ev.status;
      break;
    // TPEG-Verkehrsmeldungen (dab_app::tpeg): Dienst, Zaehler, Liste.
    case "tpeg":
      s.tpeg = ev.status;
      break;
    // Belegung Aufnahmeordner / Durchsagen (dab_app::storage, N2).
    case "storage":
      s.storage = ev.storage;
      break;
    // Timer/Aufnahme/Sleep (lib/timers.svelte.ts)
    default:
      applyTimerAppEvent(ev);
      break;
  }
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

let unsubs: (() => void)[] = [];
let ticker: ReturnType<typeof setInterval> | undefined;

export async function init() {
  unsubs.push(await api.onEvent(applyCoreEvent));
  unsubs.push(await api.onAppEvent(applyAppEvent));
  unsubs.push(await api.onAlarmDismissed(applyAlarmDismissed));
  const [snap, settings, presets, [dir, portable]] = await Promise.all([
    api.getState(),
    api.getSettings(),
    api.getPresets(),
    api.dataDir(),
  ]);
  Object.assign(s, snap);
  ui.settings = settings;
  setLang(settings.language);
  ui.presets = presets;
  ui.dataDir = dir;
  ui.portable = portable;
  await initTimers().catch((e) => notify("warn", String(e)));
  ui.ready = true;
  ticker = setInterval(() => (ui.now = Date.now()), 1000);
}

export function dispose() {
  unsubs.forEach((u) => u());
  unsubs = [];
  if (ticker) clearInterval(ticker);
}

/** Snapshot neu holen (nach Aktionen, die der Spiegel nicht selbst sieht, z. B. open_device). */
export async function refreshState() {
  Object.assign(s, await api.getState());
}

// ---------------------------------------------------------------------------
// Aktionen mit lokalem Spiegel
// ---------------------------------------------------------------------------

/** Stummschaltung setzen; der Kern meldet kein Ereignis zurueck (Befund 1),
 * darum wird der Spiegel nach erfolgreichem Kommando selbst nachgefuehrt. */
export async function setMute(muted: boolean) {
  await api.setMute(muted);
  s.muted = muted;
}

export function toggleMute() {
  return setMute(!s.muted);
}

/** Lautstaerke 0..100 setzen, Spiegel nach Erfolg nachfuehren (Befund 1). */
export async function setVolume(percent: number) {
  const v = Math.round(Math.min(100, Math.max(0, percent)));
  await api.setVolume(v);
  s.volume = v;
}

/** Alarm quittieren (Banner im Hauptfenster oder Alarmfenster, Befund 2):
 * lokal sofort als quittiert markieren, dem Kern melden und die anderen
 * Fenster per Frontend-Ereignis nachziehen - die Rust-Seite merkt sich
 * `dismissed` nur still, ohne Ereignis. */
export async function dismissAlert() {
  const a = s.alert;
  if (!a) return;
  a.dismissed = true;
  const key = { iid: a.iid, sub_ch: a.sub_ch };
  try {
    await api.ewsDismiss();
  } finally {
    await api.alarmDismissed(key).catch(() => {});
  }
}

function applyAlarmDismissed(ev: { iid: number; sub_ch: number }) {
  if (s.alert && s.alert.iid === ev.iid && s.alert.sub_ch === ev.sub_ch) s.alert.dismissed = true;
}

/** Einstellungen aendern (Teilobjekt) und an die Rust-Seite geben.
 *
 * Befund 7: Patches laufen nacheinander (Promise-Kette), damit der zweite
 * nicht auf einer veralteten Rust-Fassung aufsetzt und den ersten
 * ueberschreibt. Fehler landen in `notify`, die Rueckgabe lehnt nie ab;
 * danach wird der Spiegel aus der Rust-Fassung wiederhergestellt. */
let settingsQueue: Promise<void> = Promise.resolve();
const settingsPending: Partial<Settings>[] = [];

export function patchSettings(patch: Partial<Settings>): Promise<void> {
  if (!ui.settings) return Promise.resolve();
  // Sofort im Spiegel (Sprache/Panels reagieren ohne Wartezeit), dann auf der
  // frischen Rust-Fassung aufsetzen: die pflegt z. B. gain_by_channel und
  // last_channel selbst, das darf ein Patch nicht ueberschreiben.
  ui.settings = { ...ui.settings, ...patch };
  if (patch.language !== undefined) setLang(patch.language);
  settingsPending.push(patch);
  const job = settingsQueue.then(async () => {
    const base = await api.getSettings().catch(() => ui.settings as Settings);
    const next = { ...base, ...patch };
    // Noch wartende Patches bleiben optimistisch sichtbar.
    ui.settings = Object.assign({}, next, ...settingsPending.slice(settingsPending.indexOf(patch) + 1));
    await api.updateSettings(next);
  });
  settingsQueue = job.catch(() => {});
  return job
    .catch(async (e) => {
      notify("warn", tError(e));
      const truth = await api.getSettings().catch(() => null);
      if (truth) {
        ui.settings = truth;
        setLang(truth.language);
      }
    })
    .finally(() => {
      const i = settingsPending.indexOf(patch);
      if (i >= 0) settingsPending.splice(i, 1);
    });
}

export function togglePanel(name: keyof Settings["panels"]): Promise<void> {
  if (!ui.settings) return Promise.resolve();
  return patchSettings({ panels: { ...ui.settings.panels, [name]: !ui.settings.panels[name] } });
}
