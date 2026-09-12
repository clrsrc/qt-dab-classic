//! Tauri-Kommandos fuer die Senderliste ueber alle Ensembles (Logik in
//! `dab-app::stations`, hier nur die Bruecke). Aenderungen kommen als
//! `dab://app`-Ereignis `stations_changed{stations}` ueber die Effekte der
//! App-Schicht; der Startbestand steckt im Snapshot (`get_state().stations`).

use crate::{act, lock_app, Shared, R};
use dab_app::StationEntry;
use std::time::Instant;
use tauri::{AppHandle, State};

/// Senderliste (sortiert Kanal/Ensemble/Name); `include_data` = false: nur Audiodienste.
#[tauri::command]
pub fn stations_list(shared: State<'_, Shared>, include_data: Option<bool>) -> R<Vec<StationEntry>> {
    Ok(lock_app(&shared)?.stations(include_data.unwrap_or(false)))
}

/// Dienst der Senderliste hoeren – bei Bedarf mit Kanalwechsel (gleiche
/// Zustandsmaschine wie der Preset-Aufruf, Status per `preset_status` ohne Slot).
#[tauri::command]
pub fn station_tune(handle: AppHandle, shared: State<'_, Shared>, channel: String, eid: u16, sid: u32, scids: Option<u8>) -> R<()> {
    act(&handle, &shared, |a| a.tune_station(&channel, eid, sid, scids.unwrap_or(0), Instant::now()).map(|fx| ((), fx)))
}

/// Senderliste leeren (Datei wird leer geschrieben).
#[tauri::command]
pub fn stations_clear(handle: AppHandle, shared: State<'_, Shared>) -> R<()> {
    act(&handle, &shared, |a| Ok(((), a.stations_clear())))
}
