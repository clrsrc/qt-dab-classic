//! Tauri-Kommandos fuer die Musik-Trennung (Entscheidungen 6, 7; Plan M4b
//! Abschnitt 2.3). Die Logik liegt in `dab-app::music`, hier ist nur die
//! Bruecke. Die Vorschlagsliste kommt als `dab://app`-Ereignis
//! `music_candidates{candidates}` mit den Effekten der App-Schicht; der
//! Startbestand steckt im Snapshot (`get_state().music_candidates`).

use crate::{act, lock_app, Shared, R};
use dab_music::TrackCandidate;
use tauri::{AppHandle, State};

/// Vorschlagsliste einmalig abfragen (der laufende Betrieb nutzt das Ereignis).
#[tauri::command]
pub fn music_list(shared: State<'_, Shared>) -> R<Vec<TrackCandidate>> {
    Ok(lock_app(&shared)?.music_candidates().to_vec())
}

/// "uebernehmen": Kandidat als MP3 (mit ID3v2.4 und Cover) aus dem
/// Timeshift-Ring sichern. Liefert den Zielpfad; das Ende meldet der Kern wie
/// bei der Aufnahme als `recording_state` mit diesem Pfad.
#[tauri::command]
pub fn music_export(handle: AppHandle, shared: State<'_, Shared>, candidate_index: usize) -> R<String> {
    let path = act(&handle, &shared, |a| a.music_export(candidate_index))?;
    Ok(path.display().to_string())
}

/// Vorschlagsliste leeren (der Ring bleibt unberuehrt).
#[tauri::command]
pub fn music_clear(handle: AppHandle, shared: State<'_, Shared>) -> R<()> {
    act(&handle, &shared, |a| Ok(((), a.music_clear())))
}
