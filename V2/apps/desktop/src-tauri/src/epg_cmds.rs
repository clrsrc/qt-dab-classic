//! Tauri-Kommandos fuer EPG und Logos (Entscheidung 14: Cache und Parser in
//! `dab-app`, hier nur die Bruecke). Aenderungen kommen als `dab://app`-
//! Ereignisse `epg_updated{eid,sid,day}`, `logo_updated{eid,sid}` und
//! `current_media{logo_data_url, now_next}` ueber die Effekte der App-Schicht.

use crate::Shared;
use dab_app::{App, LogoSize, NowNext, Programme};
use serde::Serialize;
use std::sync::MutexGuard;
use tauri::State;

type R<T> = Result<T, String>;

fn app(s: &Shared) -> R<MutexGuard<'_, App>> {
    s.app.lock().map_err(|e| e.to_string())
}

#[derive(Serialize, Clone, Debug)]
pub struct EpgService {
    pub sid: u32,
    pub name: String,
}

/// Tage (yyyymmdd) mit Sendeplan-Daten fuer ein Ensemble.
#[tauri::command]
pub fn epg_days(shared: State<'_, Shared>, eid: u16) -> R<Vec<u32>> {
    Ok(app(&shared)?.epg.days(eid))
}

/// Dienste mit Sendeplan an einem Tag (Name aus der Senderliste, sonst Hex-SId).
#[tauri::command]
pub fn epg_services(shared: State<'_, Shared>, eid: u16, day: u32) -> R<Vec<EpgService>> {
    Ok(app(&shared)?.epg_services_named(eid, day).into_iter().map(|(sid, name)| EpgService { sid, name }).collect())
}

/// Sendungen eines Dienstes an einem Tag (inkl. Uebertrag aus dem Vortag), sortiert.
#[tauri::command]
pub fn epg_programmes(shared: State<'_, Shared>, eid: u16, sid: u32, day: u32) -> R<Vec<Programme>> {
    Ok(app(&shared)?.epg.programmes_for_day(eid, sid, day))
}

/// Laufende und naechste Sendung eines Dienstes des aktuellen Ensembles.
#[tauri::command]
pub fn epg_now_next(shared: State<'_, Shared>, sid: u32) -> R<Option<NowNext>> {
    Ok(app(&shared)?.now_next_for(sid))
}

/// Logo als `data:`-URL in der Wunschgroesse (`large` | `medium` | `small`).
#[tauri::command]
pub fn logo_data_url(shared: State<'_, Shared>, eid: u16, sid: u32, size: LogoSize) -> R<Option<String>> {
    Ok(app(&shared)?.logos.data_url(eid, sid, size))
}

/// Verfuegbare Logo-Groessen eines Dienstes (Debug/Panel).
#[tauri::command]
pub fn logo_sizes(shared: State<'_, Shared>, eid: u16, sid: u32) -> R<Vec<(u32, u32)>> {
    Ok(app(&shared)?.logos.sizes(eid, sid))
}
