//! Tauri-Kommandos fuer TII und Debug-Panel (Entscheidung 25; Logik in
//! `dab-app::tii`, hier nur die Bruecke). Aenderungen kommen als `dab://app`-
//! Ereignisse `tii_updated{tii}` und `debug_stats{debug}` (1 Hz, nur bei
//! offenem Panel) ueber die Effekte der App-Schicht. Spektrum/IQ laufen
//! als Kern-Ereignisse `spectrum`/`iq_samples` direkt ueber `dab://event`.

use crate::{act, lock_app, Shared};
use dab_app::{App, DebugState, TiiSeen};
use std::path::PathBuf;
use std::sync::MutexGuard;
use tauri::{AppHandle, Manager, State};

type R<T> = Result<T, String>;

/// Gemeinsamer App-Lock (vergifteter Mutex wird geloggt und weiterbenutzt, Befund 9).
fn app(s: &Shared) -> R<MutexGuard<'_, App>> {
    lock_app(s)
}

/// Ressource `resources/tii/txdata.tii` (Tauri legt sie neben die EXE bzw. in
/// den Ressourcenordner; Reihenfolge wie `resource_core_path`).
pub fn resource_tii_path(app: &AppHandle) -> Option<PathBuf> {
    let dir = app.path().resource_dir().ok()?;
    [dir.join("resources").join("tii").join("txdata.tii"), dir.join("tii").join("txdata.tii")]
        .into_iter()
        .find(|p| p.is_file())
}

/// Beim Start: Datenbank laden (`data/txdata.tii` hat Vorrang).
pub fn load_database(handle: &AppHandle, shared: &Shared) {
    let res = resource_tii_path(handle);
    if let Ok(mut a) = lock_app(shared) {
        a.tii_load(res.as_deref());
    }
}

/// Panel oeffnen/schliessen → `set_scopes` an den Kern, persistiert in `settings.panels.debug`.
#[tauri::command]
pub fn debug_set_open(handle: AppHandle, shared: State<'_, Shared>, open: bool) -> R<()> {
    act(&handle, &shared, |a| Ok(((), a.debug_set_open(open))))
}

/// Scope-Rate 1..10 Hz.
#[tauri::command]
pub fn debug_set_rate(handle: AppHandle, shared: State<'_, Shared>, rate_hz: u8) -> R<()> {
    act(&handle, &shared, |a| Ok(((), a.debug_set_rate(rate_hz))))
}

/// Aktueller Debug-Zustand (SNR-Verlauf, Zaehler) fuer das gerade geoeffnete Panel.
#[tauri::command]
pub fn debug_state(shared: State<'_, Shared>) -> R<DebugState> {
    Ok(app(&shared)?.state.debug.clone())
}

/// TII-Detektor an/aus, Schwelle, DX-Modus (CSV-Protokoll).
#[tauri::command]
pub fn tii_set(handle: AppHandle, shared: State<'_, Shared>, enabled: bool, threshold: i16, dx_mode: bool) -> R<()> {
    act(&handle, &shared, |a| Ok(((), a.tii_set(enabled, threshold, dx_mode))))
}

/// Zuletzt gesehene Sender, nach Staerke sortiert.
#[tauri::command]
pub fn tii_list(shared: State<'_, Shared>) -> R<Vec<TiiSeen>> {
    Ok(app(&shared)?.state.tii.clone())
}

/// Heimatkoordinaten (beide None = loeschen).
#[tauri::command]
pub fn home_set(handle: AppHandle, shared: State<'_, Shared>, lat: Option<f64>, lon: Option<f64>) -> R<()> {
    act(&handle, &shared, |a| Ok(((), a.home_set(lat, lon))))
}

/// ASA-"Standort-Code" (ETSI TS 104 089 Annex A, z. B. "1253-3513-3668",
/// wie auf asa.radio adressgenau erzeugt) in Dezimalgrad umrechnen - reine
/// Rechnung ohne Kern-/App-Zustand, `home_set` traegt das Ergebnis ein.
#[tauri::command]
pub fn home_code_decode(code: String) -> R<(f64, f64)> {
    dab_app::ews_location::decode_presentation_code(&code).ok_or_else(|| "ungueltiger Standort-Code".to_string())
}
