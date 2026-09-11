//! DAB Classic – Tauri-Seite: startet den Kern ueber `dab-core`, reicht
//! Ereignisse als Tauri-Events an das Frontend und nimmt Kommandos per
//! `invoke` entgegen. Die App-Logik liegt in `dab-app`; hier ist nur die Bruecke.

use dab_api::{Command, Event};
use dab_app::{DataDirs, Presets, Settings};
use dab_core::{locate_core, CoreBackend, IpcBackend, IpcConfig};
use std::sync::Mutex;
use tauri::{AppHandle, Emitter, Manager, State};

/// Gemeinsamer Zustand der App (Tauri-managed).
pub struct AppState {
    pub dirs: DataDirs,
    pub settings: Mutex<Settings>,
    pub presets: Mutex<Presets>,
    pub core: Mutex<Option<IpcBackend>>,
}

/// Name des Tauri-Events, unter dem alle Kern-Ereignisse ankommen.
const CORE_EVENT: &str = "dab://event";

// ---------------------------------------------------------------------------
// invoke-Handler
// ---------------------------------------------------------------------------

#[tauri::command]
fn core_send(state: State<'_, AppState>, command: Command) -> Result<(), String> {
    let guard = state.core.lock().map_err(|e| e.to_string())?;
    let core = guard.as_ref().ok_or("Kern laeuft nicht")?;
    core.commands().send(command).map_err(|e| e.to_string())
}

#[tauri::command]
fn core_alive(state: State<'_, AppState>) -> bool {
    state.core.lock().map(|g| g.as_ref().map(|c| c.is_alive()).unwrap_or(false)).unwrap_or(false)
}

#[tauri::command]
fn get_settings(state: State<'_, AppState>) -> Result<Settings, String> {
    state.settings.lock().map(|s| s.clone()).map_err(|e| e.to_string())
}

#[tauri::command]
fn save_settings(state: State<'_, AppState>, settings: Settings) -> Result<(), String> {
    let mut s = state.settings.lock().map_err(|e| e.to_string())?;
    *s = settings;
    s.save(&state.dirs.settings_file()).map_err(|e| e.to_string())
}

#[tauri::command]
fn get_presets(state: State<'_, AppState>) -> Result<Presets, String> {
    state.presets.lock().map(|p| p.clone()).map_err(|e| e.to_string())
}

#[tauri::command]
fn save_presets(state: State<'_, AppState>, presets: Presets) -> Result<(), String> {
    let mut p = state.presets.lock().map_err(|e| e.to_string())?;
    *p = presets;
    p.save(&state.dirs.presets_file()).map_err(|e| e.to_string())
}

#[tauri::command]
fn data_dir(state: State<'_, AppState>) -> (String, bool) {
    (state.dirs.root.display().to_string(), state.dirs.portable)
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
        .or_else(|| {
            app.path().resource_dir().ok().map(|d| d.join("core").join("dabcored.exe")).filter(|p| p.is_file())
        })
}

fn start_core(app: &AppHandle) -> anyhow::Result<IpcBackend> {
    let exe = resource_core_path(app)
        .or_else(|| locate_core(None))
        .ok_or_else(|| anyhow::anyhow!("dabcored.exe nicht gefunden"))?;
    log::info!("Kern: {}", exe.display());
    let backend = IpcBackend::spawn(IpcConfig::new(&exe))?;

    // Bruecke: crossbeam -> Tauri-Event. Latest-wins-Ereignisse werden auf
    // ~20 Hz je Typ gedrosselt, damit das WebView nicht ueberflutet wird.
    let rx = backend.events();
    let handle = app.clone();
    std::thread::Builder::new()
        .name("core-event-bridge".into())
        .spawn(move || {
            use std::collections::HashMap;
            use std::time::{Duration, Instant};
            let min_gap = Duration::from_millis(50);
            let mut last: HashMap<&'static str, Instant> = HashMap::new();
            for ev in rx.iter() {
                if ev.is_latest_wins() {
                    let key = latest_key(&ev);
                    let now = Instant::now();
                    if let Some(t) = last.get(key) {
                        if now.duration_since(*t) < min_gap {
                            continue;
                        }
                    }
                    last.insert(key, now);
                }
                let exiting = matches!(ev, Event::Exiting { .. });
                if let Err(e) = handle.emit(CORE_EVENT, &ev) {
                    log::warn!("emit: {e}");
                }
                if exiting {
                    break;
                }
            }
        })?;
    Ok(backend)
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

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    let dirs = DataDirs::detect();
    if let Err(e) = dirs.ensure() {
        log::warn!("Datenordner {}: {e}", dirs.root.display());
    }
    log::info!("Datenordner: {} (portabel: {})", dirs.root.display(), dirs.portable);
    let settings = Settings::load(&dirs.settings_file());
    let presets = Presets::load(&dirs.presets_file());

    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .manage(AppState {
            dirs,
            settings: Mutex::new(settings),
            presets: Mutex::new(presets),
            core: Mutex::new(None),
        })
        .setup(|app| {
            let handle = app.handle().clone();
            match start_core(&handle) {
                Ok(backend) => {
                    let state = app.state::<AppState>();
                    // Startwerte an den Kern
                    let s = state.settings.lock().unwrap();
                    let tx = backend.commands();
                    let _ = tx.send(Command::SetVolume { percent: s.volume_percent });
                    let _ = tx.send(Command::SetAgc { enabled: s.agc });
                    let _ = tx.send(Command::SetEws { enabled: s.ews_enabled, autoswitch: s.ews_autoswitch });
                    drop(s);
                    *state.core.lock().unwrap() = Some(backend);
                }
                Err(e) => {
                    log::error!("Kern konnte nicht gestartet werden: {e}");
                    let _ = handle.emit(CORE_EVENT, Event::DeviceError { message: format!("Kern: {e}") });
                }
            }
            Ok(())
        })
        .on_window_event(|window, event| {
            if let tauri::WindowEvent::Destroyed = event {
                if let Some(state) = window.try_state::<AppState>() {
                    if let Ok(mut g) = state.core.lock() {
                        if let Some(mut c) = g.take() {
                            c.shutdown();
                        }
                    }
                }
            }
        })
        .invoke_handler(tauri::generate_handler![
            core_send,
            core_alive,
            get_settings,
            save_settings,
            get_presets,
            save_presets,
            data_dir
        ])
        .run(tauri::generate_context!())
        .expect("Tauri-App konnte nicht gestartet werden");
}
