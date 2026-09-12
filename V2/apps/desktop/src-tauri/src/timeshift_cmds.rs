//! Tauri-Kommandos fuer den Timeshift-Puffer (Entscheidungen 4, 5, 21;
//! Plan M4 Abschnitt 2). Die Logik liegt in `dab-app::timeshift`, hier ist nur
//! die Bruecke. Der Zustand kommt als Kern-Ereignis `timeshift_state` ueber
//! `dab://event` in den Spiegel; Hinweise (Puffer verworfen) laufen als
//! `timeshift_notice` ueber `dab://app` mit den Effekten der App-Schicht.

use crate::{act, lock_app, Shared, R};
use dab_app::TimeshiftInfo;
use tauri::{AppHandle, State};

/// Leertaste bzw. Knopf ⏸/▶: pausieren oder ab dem Lesezeiger fortsetzen.
#[tauri::command]
pub fn timeshift_pause_toggle(handle: AppHandle, shared: State<'_, Shared>) -> R<()> {
    act(&handle, &shared, |a| a.timeshift_pause_toggle().map(|fx| ((), fx)))
}

/// Pfeil links/rechts bzw. Knoepfe −30/+30 s: `delta_s > 0` geht Richtung live.
#[tauri::command]
pub fn timeshift_skip(handle: AppHandle, shared: State<'_, Shared>, delta_s: f64) -> R<()> {
    act(&handle, &shared, |a| a.timeshift_skip(delta_s).map(|fx| ((), fx)))
}

/// Klick/Drag auf den Pufferbalken: `offset_s` = Sekunden hinter live (0 = live).
#[tauri::command]
pub fn timeshift_seek(handle: AppHandle, shared: State<'_, Shared>, offset_s: f64) -> R<()> {
    act(&handle, &shared, |a| a.timeshift_seek(offset_s).map(|fx| ((), fx)))
}

/// Esc bzw. Knopf LIVE: zurueck an den Schreibzeiger.
#[tauri::command]
pub fn timeshift_live(handle: AppHandle, shared: State<'_, Shared>) -> R<()> {
    act(&handle, &shared, |a| a.timeshift_live().map(|fx| ((), fx)))
}

/// "Letzte n Minuten sichern": Ausschnitt des Rings als WAV (Dateiname wie bei
/// der Aufnahme mit Suffix `_timeshift`). Liefert den Zielpfad; das Ende meldet
/// der Kern als `recording_state` mit diesem Pfad.
#[tauri::command]
pub fn timeshift_export(handle: AppHandle, shared: State<'_, Shared>, from_s: f64, to_s: f64) -> R<String> {
    let path = act(&handle, &shared, |a| a.timeshift_export(from_s, to_s))?;
    Ok(path.display().to_string())
}

/// Zustand einmalig abfragen (der laufende Betrieb nutzt `timeshift_state`).
#[tauri::command]
pub fn timeshift_status(shared: State<'_, Shared>) -> R<TimeshiftInfo> {
    Ok(*lock_app(&shared)?.timeshift_info())
}
