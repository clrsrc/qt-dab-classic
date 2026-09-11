//! DAB Classic – `dab-app`: Anwendungslogik ohne GUI.
//!
//! Alles, was nicht Empfang (Kern) und nicht Darstellung (Svelte) ist, lebt
//! hier: Datenordner (portabel oder Benutzerprofil), Einstellungen, die zehn
//! Stationsspeicher, spaeter Timer, EPG-Cache, Timeshift-Steuerung,
//! Aufnahme-Regeln und die Schnittliste der Musik-Trennung. Alles hier ist
//! ohne Fenster testbar und wird auch vom Headless-Treiber `dab-cli` genutzt.

pub mod paths;
pub mod presets;
pub mod settings;

pub use paths::DataDirs;
pub use presets::{Preset, Presets, PRESET_SLOTS};
pub use settings::Settings;
