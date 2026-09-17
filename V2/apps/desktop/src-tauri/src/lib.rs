//! DAB Classic – Tauri-Seite: startet den Kern ueber `dab-core`, fuehrt den
//! App-Zustand (`dab-app::App`, die "Truth") im Ereignis-Thread, reicht
//! Kern-Ereignisse als Tauri-Events an das Frontend und nimmt Aktionen per
//! `invoke` entgegen. Logik liegt in `dab-app`; hier ist nur die Bruecke.

use dab_api::{Command, Event, Gain, SourceKind};
use dab_app::{App, AppEvent, AppState as Truth, DataDirs, Effects, NoticeLevel, Preset, Presets, Settings, StoreResult};
use dab_core::{locate_core, CoreBackend, IpcBackend, IpcConfig};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter, Manager, State};

/// EPG/Logo-Kommandos (eigenes Modul, Registrierung unten).
mod epg_cmds;
/// Timer/Aufnahme/Sleep/Alarmfenster (eigenes Modul, Registrierung unten).
mod timer_cmds;
/// TII/Debug-Panel (eigenes Modul, Registrierung unten).
mod debug_cmds;
/// Senderliste ueber alle Ensembles (eigenes Modul, Registrierung unten).
mod stations_cmds;
/// Timeshift-Puffer (eigenes Modul, Registrierung unten).
mod timeshift_cmds;
/// Musik-Trennung (eigenes Modul, Registrierung unten).
mod music_cmds;
/// Hybrid Radio / RadioDNS: Worker-Thread und Kommandos (eigenes Modul).
mod radiodns_cmds;

/// Gemeinsamer Zustand (Tauri-managed).
pub struct Shared {
    pub app: Arc<Mutex<App>>,
    pub core: Mutex<Option<IpcBackend>>,
    pub shutting_down: AtomicBool,
    pub restarts: AtomicU32,
    /// Generation des laufenden Kerns (Review 16.09.2026, Befund 2): jeder
    /// `spawn_core` zaehlt hoch, jeder Brueckenthread merkt sich seine
    /// Generation. Ein Thread, dessen Generation nicht mehr die aktuelle ist
    /// (bewusster Neustart per `restart_core`, Neustart durch einen anderen
    /// Thread), raeumt nichts ab und startet nichts neu - sonst nahm der
    /// alte Thread beim `Exiting` des alten Kerns den GERADE NEU gestarteten
    /// Kern aus `core` und loeste eine Neustart-Kaskade aus.
    pub core_gen: AtomicU32,
    /// Der App-Mutex war vergiftet (Panic unter dem Lock); einmal gemeldet
    /// (Review 16.09.2026, Befund 9). Der Zustand wird trotzdem
    /// weiterverwendet (`PoisonError::into_inner`): jede Aktion der
    /// App-Schicht arbeitet abgeschlossen auf `App`, ein halb ausgefuehrter
    /// Schritt ist verkraftbar - ein stillstehender Brueckenthread nicht.
    pub poisoned: AtomicBool,
}

/// Kern-Ereignisse 1:1 (Delta-Events fuer den Spiegel im Frontend).
const CORE_EVENT: &str = "dab://event";
/// Hinweise der App-Schicht (Preset-Status, Presets/Settings geaendert, Kern-Neustart).
const APP_EVENT: &str = "dab://app";
/// Hoechstens so viele automatische Neustarts des Kerns hintereinander.
const MAX_RESTARTS: u32 = 5;

type R<T> = Result<T, String>;

/// App-Lock; ein vergifteter Mutex (Panic unter dem Lock) wird einmal als
/// Fehler geloggt und danach weiterbenutzt statt jede Aktion mit "poisoned"
/// abzuweisen (Befund 9). Die Meldung an die UI schickt der Brueckenthread
/// (`spawn_core`), der als einziger einen `AppHandle` zur Hand hat.
pub(crate) fn lock_app(s: &Shared) -> R<std::sync::MutexGuard<'_, App>> {
    match s.app.lock() {
        Ok(g) => Ok(g),
        Err(poison) => {
            if !s.poisoned.swap(true, Ordering::SeqCst) {
                log::error!("App-Zustand: Mutex vergiftet (Panic unter dem Lock); Zustand wird weiterbenutzt, Neustart der App empfohlen");
            }
            Ok(poison.into_inner())
        }
    }
}

/// Einmalige Fehlermeldung an die UI, sobald `lock_app` einen vergifteten
/// Mutex gesehen hat (Befund 9).
fn report_poison_once(handle: &AppHandle, shared: &Shared, reported: &mut bool) {
    if *reported || !shared.poisoned.load(Ordering::SeqCst) {
        return;
    }
    *reported = true;
    let _ = handle.emit(
        APP_EVENT,
        AppEvent::Notice { level: NoticeLevel::Error, text: "internal error: app state may be inconsistent, please restart".into() },
    );
}

/// Ablaufspur (Umgebungsvariable DABCLASSIC_TRACE=1): jedes Kommando an den Kern,
/// jedes nicht gedrosselte Kern-Ereignis und jedes App-Ereignis als Log-Zeile.
fn trace_on() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var("DABCLASSIC_TRACE").map(|v| v != "0" && !v.is_empty()).unwrap_or(false))
}

fn trace_json<T: serde::Serialize>(prefix: &str, v: &T) {
    if trace_on() {
        let mut s = serde_json::to_string(v).unwrap_or_default();
        if s.len() > 300 {
            s.truncate(300);
            s.push_str("…");
        }
        log::info!("{prefix} {s}");
    }
}

/// Fuehrt die Effekte einer Aktion aus: Kommandos an den Kern, Hinweise ans Frontend.
pub(crate) fn run_effects(handle: &AppHandle, shared: &Shared, fx: Effects) {
    if trace_on() {
        for c in &fx.commands {
            trace_json("->", c);
        }
        for ev in &fx.events {
            trace_json("app", ev);
        }
    }
    if !fx.commands.is_empty() {
        match shared.core.lock() {
            Ok(guard) => match guard.as_ref() {
                Some(core) => {
                    let tx = core.commands();
                    for c in fx.commands {
                        if let Err(e) = tx.send(c) {
                            log::warn!("Kommando verloren: {e}");
                        }
                    }
                }
                None => {
                    let _ = handle.emit(APP_EVENT, AppEvent::Notice { level: NoticeLevel::Error, text: "core not running".into() });
                }
            },
            Err(e) => log::warn!("core lock: {e}"),
        }
    }
    for ev in fx.events {
        if let Err(e) = handle.emit(APP_EVENT, &ev) {
            log::warn!("emit: {e}");
        }
    }
}

/// Aktion unter dem App-Lock ausfuehren und die Effekte anschliessend (ohne Lock) anwenden.
pub(crate) fn act<T>(handle: &AppHandle, shared: &Shared, f: impl FnOnce(&mut App) -> Result<(T, Effects), dab_app::AppError>) -> R<T> {
    let (out, fx) = {
        let mut app = lock_app(shared)?;
        f(&mut app).map_err(|e| e.to_string())?
    };
    run_effects(handle, shared, fx);
    Ok(out)
}

// ---------------------------------------------------------------------------
// invoke-Handler
// ---------------------------------------------------------------------------

#[tauri::command]
fn get_state(shared: State<'_, Shared>) -> R<Truth> {
    Ok(lock_app(&shared)?.state.clone())
}

#[tauri::command]
fn get_settings(shared: State<'_, Shared>) -> R<Settings> {
    Ok(lock_app(&shared)?.settings.clone())
}

#[tauri::command]
fn update_settings(handle: AppHandle, shared: State<'_, Shared>, settings: Settings) -> R<()> {
    act(&handle, &shared, |a| Ok(((), a.update_settings(settings))))
}

#[tauri::command]
fn get_presets(shared: State<'_, Shared>) -> R<Presets> {
    Ok(lock_app(&shared)?.presets.clone())
}

/// Generischer Weg fuer Kern-Kommandos; die App-Schicht fuehrt Kanal, Geraet,
/// Lautstaerke usw. mit und ergaenzt z. B. den gespeicherten Gain.
#[tauri::command]
fn core_send(handle: AppHandle, shared: State<'_, Shared>, command: Command) -> R<()> {
    act(&handle, &shared, |a| a.command(command).map(|fx| ((), fx)))
}

#[tauri::command]
fn core_alive(shared: State<'_, Shared>) -> bool {
    shared.core.lock().map(|g| g.as_ref().map(|c| c.is_alive()).unwrap_or(false)).unwrap_or(false)
}

/// Bewusster Neustart des Kerns (Frontend-Knopf). Zuerst die Generation
/// hochzaehlen: der alte Brueckenthread erkennt daran, dass sein `Exiting`
/// kein Absturz ist, und laesst den neuen Kern in Ruhe (Befund 2). Das
/// `Exiting` fuer App-Zustand und Frontend wird hier genau einmal
/// nachgestellt, weil der alte Thread es nicht mehr verarbeitet.
#[tauri::command]
fn restart_core(handle: AppHandle, shared: State<'_, Shared>) -> R<()> {
    shared.restarts.store(0, Ordering::SeqCst);
    shared.core_gen.fetch_add(1, Ordering::SeqCst);
    if let Ok(mut g) = shared.core.lock() {
        if let Some(mut c) = g.take() {
            c.shutdown();
        }
    }
    let exiting = Event::Exiting { reason: "restart".into() };
    let fx = {
        let mut a = lock_app(&shared)?;
        let mut fx = a.handle_event(&exiting, Instant::now());
        fx.commands.clear();
        fx
    };
    let _ = handle.emit(CORE_EVENT, &exiting);
    timer_cmds::on_core_event(&handle, &shared, &exiting);
    run_effects(&handle, &shared, fx);
    spawn_core(&handle, "manual").map_err(|e| e.to_string())
}

#[tauri::command]
fn open_device(handle: AppHandle, shared: State<'_, Shared>, source: SourceKind) -> R<()> {
    act(&handle, &shared, |a| Ok(((), a.open_device(source))))
}

#[tauri::command]
fn step_service(handle: AppHandle, shared: State<'_, Shared>, delta: i32) -> R<()> {
    act(&handle, &shared, |a| a.step_service(delta).map(|fx| ((), fx)))
}

#[tauri::command]
fn set_gain(handle: AppHandle, shared: State<'_, Shared>, gain: Gain) -> R<()> {
    act(&handle, &shared, |a| Ok(((), a.set_gain(gain))))
}

#[tauri::command]
fn preset_recall(handle: AppHandle, shared: State<'_, Shared>, slot: usize) -> R<()> {
    if trace_on() {
        log::info!("invoke preset_recall slot={slot}");
    }
    act(&handle, &shared, |a| a.preset_recall(slot, Instant::now()).map(|fx| ((), fx)))
}

/// Ohne `sid`: aktueller Dienst. Mit `sid`: Dienst des aktuellen Ensembles
/// (Drag aus dem Ensemble-Tab); mit `channel` + `eid` dazu: Eintrag der
/// Senderliste, auch aus einem anderen Ensemble (stations_cmds).
#[tauri::command]
fn preset_store(
    handle: AppHandle,
    shared: State<'_, Shared>,
    slot: usize,
    sid: Option<u32>,
    scids: Option<u8>,
    channel: Option<String>,
    eid: Option<u16>,
    force: bool,
) -> R<StoreResult> {
    let scids = scids.unwrap_or(0);
    match (channel, eid, sid) {
        (Some(ch), Some(eid), Some(sid)) => act(&handle, &shared, |a| a.preset_store_station(slot, &ch, eid, sid, scids, force)),
        (_, _, sid) => {
            let service = sid.map(|s| (s, scids));
            act(&handle, &shared, |a| a.preset_store_service(slot, service, force))
        }
    }
}

#[tauri::command]
fn preset_clear(handle: AppHandle, shared: State<'_, Shared>, slot: usize) -> R<Option<Preset>> {
    act(&handle, &shared, |a| a.preset_clear(slot))
}

#[tauri::command]
fn presets_import(handle: AppHandle, shared: State<'_, Shared>, path: Option<String>) -> R<usize> {
    act(&handle, &shared, |a| a.presets_import_favorites(path.as_deref().map(std::path::Path::new)))
}

#[tauri::command]
fn favorites_path(shared: State<'_, Shared>) -> R<Option<String>> {
    Ok(lock_app(&shared)?.favorites_path().map(|p| p.display().to_string()))
}

#[tauri::command]
fn active_preset_slot(shared: State<'_, Shared>) -> R<Option<usize>> {
    Ok(lock_app(&shared)?.active_preset_slot())
}

#[tauri::command]
fn data_dir(shared: State<'_, Shared>) -> R<(String, bool)> {
    let a = lock_app(&shared)?;
    Ok((a.dirs.root.display().to_string(), a.dirs.portable))
}

/// Ordner im Explorer oeffnen (Einstellungen > Aufnahme: Aufnahmeordner,
/// Durchsagen). Nur vorhandene Ordner, sonst Fehlertext an die UI.
#[tauri::command]
fn open_folder(path: String) -> R<()> {
    let p = std::path::PathBuf::from(&path);
    if !p.is_dir() {
        return Err(format!("{path}: kein Ordner"));
    }
    tauri_plugin_opener::open_path(p, None::<&str>).map_err(|e| e.to_string())
}

// ---------------------------------------------------------------------------
// Kernstart und Ereignis-Bruecke
// ---------------------------------------------------------------------------

fn resource_core_path(app: &AppHandle) -> Option<std::path::PathBuf> {
    // Tauri legt `resources/core/*` neben die EXE (bzw. in den Ressourcenordner).
    app.path()
        .resource_dir()
        .ok()
        .map(|d| d.join("resources").join("core").join("dabcored.exe"))
        .filter(|p| p.is_file())
        .or_else(|| app.path().resource_dir().ok().map(|d| d.join("core").join("dabcored.exe")).filter(|p| p.is_file()))
}

fn latest_key(ev: &Event) -> &'static str {
    match ev {
        Event::Snr { .. } => "snr",
        Event::FicQuality { .. } => "fic",
        Event::FrequencyOffset { .. } => "foff",
        Event::AudioLevel { .. } => "level",
        Event::Spectrum { .. } => "spectrum",
        Event::IqSamples { .. } => "iq",
        Event::TimeshiftState { .. } => "ts",
        Event::FileProgress { .. } => "file",
        Event::ServiceStats { .. } => "stats",
        _ => "other",
    }
}

/// Startet den Kernprozess und den Brueckenthread. Der Thread verarbeitet
/// jedes Ereignis zuerst in der App-Schicht (Zustand, Preset-Automat), reicht
/// es dann an das Frontend weiter (Latest-wins ~20 Hz je Typ) und fuehrt die
/// Effekte aus. Endet der Kern unerwartet, wird er neu gestartet.
fn spawn_core(handle: &AppHandle, reason: &str) -> anyhow::Result<()> {
    let exe = resource_core_path(handle)
        .or_else(|| locate_core(None))
        .ok_or_else(|| anyhow::anyhow!("dabcored.exe nicht gefunden"))?;
    log::info!("Kern: {} ({reason})", exe.display());
    let backend = IpcBackend::spawn(IpcConfig::new(&exe))?;
    let rx = backend.events();
    let shared = handle.state::<Shared>();
    // Neue Generation: der Kern in `core` gehoert ab jetzt diesem Thread.
    let my_gen = shared.core_gen.fetch_add(1, Ordering::SeqCst) + 1;
    *shared.core.lock().map_err(|e| anyhow::anyhow!("{e}"))? = Some(backend);

    let app = handle.clone();
    std::thread::Builder::new().name("core-event-bridge".into()).spawn(move || {
        use std::collections::HashMap;
        let shared = app.state::<Shared>();
        let min_gap = Duration::from_millis(50);
        let mut last: HashMap<&'static str, Instant> = HashMap::new();
        let mut exit_reason = String::from("stdout geschlossen");
        let mut poison_reported = false;
        let is_current = || shared.core_gen.load(Ordering::SeqCst) == my_gen;
        loop {
            match rx.recv_timeout(Duration::from_millis(200)) {
                Ok(ev) => {
                    if !is_current() {
                        // Abgeloest (restart_core): Ereignisse des alten Kerns -
                        // insbesondere sein `Exiting` - nicht mehr in den
                        // Zustand des neuen Kerns mischen (Befund 2).
                        log::info!("Kern-Bruecke Generation {my_gen} abgeloest, endet");
                        return;
                    }
                    let now = Instant::now();
                    if !ev.is_latest_wins() {
                        trace_json("<-", &ev);
                    }
                    let mut fx = match lock_app(&shared) {
                        Ok(mut a) => {
                            let mut fx = a.handle_event(&ev, now);
                            if matches!(ev, Event::Ready { .. }) {
                                fx.append(a.startup(now));
                            }
                            fx
                        }
                        Err(e) => {
                            log::error!("App-Zustand nicht erreichbar: {e}");
                            Effects::default()
                        }
                    };
                    report_poison_once(&app, &shared, &mut poison_reported);
                    let forward = if ev.is_latest_wins() {
                        let key = latest_key(&ev);
                        let now = Instant::now();
                        let ok = last.get(key).map(|t| now.duration_since(*t) >= min_gap).unwrap_or(true);
                        if ok {
                            last.insert(key, now);
                        }
                        ok
                    } else {
                        true
                    };
                    if forward {
                        if let Err(e) = app.emit(CORE_EVENT, &ev) {
                            log::warn!("emit: {e}");
                        }
                    }
                    timer_cmds::on_core_event(&app, &shared, &ev);
                    if let Event::Exiting { reason } = &ev {
                        exit_reason = reason.clone();
                        fx.commands.clear();
                        run_effects(&app, &shared, fx);
                        break;
                    }
                    run_effects(&app, &shared, fx);
                }
                Err(crossbeam_channel::RecvTimeoutError::Timeout) => {
                    if !is_current() {
                        log::info!("Kern-Bruecke Generation {my_gen} abgeloest, endet");
                        return;
                    }
                    let fx = lock_app(&shared).map(|mut a| a.tick(Instant::now())).unwrap_or_default();
                    report_poison_once(&app, &shared, &mut poison_reported);
                    run_effects(&app, &shared, fx);
                }
                Err(crossbeam_channel::RecvTimeoutError::Disconnected) => break,
            }
        }
        if shared.shutting_down.load(Ordering::SeqCst) || !is_current() {
            // App-Ende oder bewusster Neustart (restart_core hat die
            // Generation schon hochgezaehlt): nichts abraeumen, nichts neu starten.
            return;
        }
        // Unerwartetes Ende: Neustart mit Meldung (begrenzt).
        let attempt = shared.restarts.fetch_add(1, Ordering::SeqCst) + 1;
        if attempt > MAX_RESTARTS {
            let _ = app.emit(APP_EVENT, AppEvent::Notice { level: NoticeLevel::Error, text: format!("core stopped ({exit_reason}), giving up") });
            return;
        }
        log::warn!("Kern beendet ({exit_reason}), Neustart {attempt}/{MAX_RESTARTS}");
        std::thread::sleep(Duration::from_secs(1));
        if shared.shutting_down.load(Ordering::SeqCst) || !is_current() {
            // Waehrend der Wartezeit hat jemand anderes (restart_core) uebernommen.
            return;
        }
        if let Ok(mut g) = shared.core.lock() {
            g.take();
        }
        if let Ok(mut a) = lock_app(&shared) {
            a.state.core_restarts = attempt;
        }
        match spawn_core(&app, &exit_reason) {
            Ok(()) => {
                let _ = app.emit(APP_EVENT, AppEvent::CoreRestarted { reason: exit_reason, attempt });
            }
            Err(e) => {
                let _ = app.emit(APP_EVENT, AppEvent::Notice { level: NoticeLevel::Error, text: format!("core restart failed: {e}") });
            }
        }
    })?;
    Ok(())
}

/// Fenstergroesse vor dem Beenden in die Settings uebernehmen (Bugfixes.txt #4).
fn capture_window_size(window: &tauri::Window, shared: &Shared) {
    let Ok(size) = window.inner_size() else { return };
    if size.width == 0 || size.height == 0 {
        return; // minimiert o.ae. liefert 0x0, nicht als Groesse uebernehmen
    }
    if let Ok(mut a) = lock_app(shared) {
        a.settings.window_width = Some(size.width);
        a.settings.window_height = Some(size.height);
    }
}

fn stop_all(shared: &Shared) {
    shared.shutting_down.store(true, Ordering::SeqCst);
    if let Ok(mut a) = lock_app(shared) {
        if let Err(e) = a.save_all() {
            log::warn!("Speichern beim Beenden: {e}");
        }
    }
    if let Ok(mut g) = shared.core.lock() {
        if let Some(mut c) = g.take() {
            c.shutdown();
        }
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    let dirs = DataDirs::detect();
    if let Err(e) = dirs.ensure() {
        log::warn!("Datenordner {}: {e}", dirs.root.display());
    }
    log::info!("Datenordner: {} (portabel: {})", dirs.root.display(), dirs.portable);
    let app = App::new(dirs);

    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .manage(Shared {
            app: Arc::new(Mutex::new(app)),
            core: Mutex::new(None),
            shutting_down: AtomicBool::new(false),
            restarts: AtomicU32::new(0),
            core_gen: AtomicU32::new(0),
            poisoned: AtomicBool::new(false),
        })
        .setup(|app| {
            let handle = app.handle().clone();
            debug_cmds::load_database(&handle, &handle.state::<Shared>());
            // Zuletzt gespeicherte Fenstergroesse wiederherstellen (Bugfixes.txt #4);
            // ohne gespeicherten Wert bleibt es bei der Groesse aus tauri.conf.json.
            if let Some(window) = handle.get_webview_window("main") {
                let saved = lock_app(&handle.state::<Shared>()).ok().and_then(|a| match (a.settings.window_width, a.settings.window_height) {
                    (Some(w), Some(h)) => Some((w, h)),
                    _ => None,
                });
                if let Some((width, height)) = saved {
                    let _ = window.set_size(tauri::Size::Physical(tauri::PhysicalSize { width, height }));
                }
            }
            if let Err(e) = spawn_core(&handle, "start") {
                log::error!("Kern konnte nicht gestartet werden: {e}");
                let _ = handle.emit(APP_EVENT, AppEvent::Notice { level: NoticeLevel::Error, text: format!("core: {e}") });
            }
            // Netzzugriffe (RadioDNS) laufen in einem eigenen Thread; er tut
            // nichts, solange der Schalter in den Einstellungen aus ist.
            radiodns_cmds::spawn_worker(&handle);
            Ok(())
        })
        .on_window_event(|window, event| {
            // Nur das Hauptfenster beendet die App; das Alarmfenster (timer_cmds) darf zugehen.
            if window.label() == "main" && matches!(event, tauri::WindowEvent::CloseRequested { .. } | tauri::WindowEvent::Destroyed) {
                timer_cmds::close_alarm(window.app_handle());
                if let Some(shared) = window.try_state::<Shared>() {
                    capture_window_size(window, &shared);
                    stop_all(&shared);
                }
            }
        })
        .invoke_handler(tauri::generate_handler![
            recording_bytes,
            open_folder,
            get_state,
            get_settings,
            update_settings,
            get_presets,
            core_send,
            core_alive,
            restart_core,
            open_device,
            step_service,
            set_gain,
            preset_recall,
            preset_store,
            preset_clear,
            presets_import,
            favorites_path,
            active_preset_slot,
            data_dir,
            epg_cmds::epg_days,
            epg_cmds::epg_services,
            epg_cmds::epg_programmes,
            epg_cmds::epg_now_next,
            epg_cmds::logo_data_url,
            epg_cmds::logo_sizes,
            timer_cmds::timers_list,
            timer_cmds::timer_add,
            timer_cmds::timer_update,
            timer_cmds::timer_delete,
            timer_cmds::timer_toggle_active,
            timer_cmds::timer_add_from_epg,
            timer_cmds::recording_start,
            timer_cmds::recording_stop,
            timer_cmds::recording_toggle,
            timer_cmds::recording_status,
            timer_cmds::sleep_set,
            timer_cmds::sleep_cancel,
            timer_cmds::sleep_status,
            timer_cmds::alarm_close,
            debug_cmds::debug_set_open,
            debug_cmds::debug_set_rate,
            debug_cmds::debug_state,
            debug_cmds::tii_set,
            debug_cmds::tii_list,
            debug_cmds::home_set,
            debug_cmds::home_code_decode,
            stations_cmds::stations_list,
            stations_cmds::station_tune,
            stations_cmds::stations_clear,
            timeshift_cmds::timeshift_pause_toggle,
            timeshift_cmds::timeshift_skip,
            timeshift_cmds::timeshift_seek,
            timeshift_cmds::timeshift_live,
            timeshift_cmds::timeshift_export,
            timeshift_cmds::timeshift_status,
            music_cmds::music_list,
            music_cmds::music_export,
            music_cmds::music_clear,
            music_cmds::music_adjust,
            music_cmds::music_preview,
            radiodns_cmds::radiodns_refresh,
            radiodns_cmds::radiodns_status
        ])
        .run(tauri::generate_context!())
        .expect("Tauri-App konnte nicht gestartet werden");
}

/// Inhalt eines Mitschnitts (MP3/WAV) fuer die Wiedergabe im Frontend
/// (Durchsagen-/EWF-Liste). Nur Dateien unterhalb des Aufnahmeordners bzw.
/// des Datenordners - kein allgemeiner Dateizugriff.
#[tauri::command]
fn recording_bytes(shared: State<'_, Shared>, path: String) -> Result<tauri::ipc::Response, String> {
    let (allowed, requested) = {
        let a = lock_app(&shared)?;
        let mut allowed = vec![a.recording_dir(), a.dirs.root.clone()];
        allowed.retain(|d| d.exists());
        (allowed, std::path::PathBuf::from(&path))
    };
    let canon = requested.canonicalize().map_err(|e| format!("{path}: {e}"))?;
    let inside = allowed.iter().any(|d| d.canonicalize().map(|c| canon.starts_with(c)).unwrap_or(false));
    if !inside {
        return Err("Datei liegt ausserhalb des Aufnahmeordners".into());
    }
    let bytes = std::fs::read(&canon).map_err(|e| format!("{path}: {e}"))?;
    Ok(tauri::ipc::Response::new(bytes))
}
