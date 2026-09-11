//! DAB Classic – `dab-app`: Anwendungslogik ohne GUI.
//!
//! Alles, was nicht Empfang (Kern) und nicht Darstellung (Svelte) ist, lebt
//! hier: Datenordner (portabel oder Benutzerprofil), Einstellungen, die zehn
//! Stationsspeicher, spaeter Timer, EPG-Cache, Timeshift-Steuerung,
//! Aufnahme-Regeln und die Schnittliste der Musik-Trennung. Alles hier ist
//! ohne Fenster testbar und wird auch vom Headless-Treiber `dab-cli` genutzt.

pub mod app;
pub mod favorites;
pub mod paths;
pub mod presets;
pub mod settings;
pub mod state;

pub use app::{App, AppError, AppEvent, Effects, NoticeLevel, PresetStatus, StoreResult, DEFAULT_HACKRF_GAIN, PRESET_TIMEOUT};
pub use paths::DataDirs;
pub use presets::{Preset, Presets, PRESET_SLOTS};
pub use settings::{Panels, Settings};
pub use state::AppState;
