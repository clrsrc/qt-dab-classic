//! Speicherplatz der Mitschnitte (Auftrag Stefan 17.09.2026, Punkt N2).
//!
//! Durchsage- und Warnungs-Mitschnitte (crate::traffic,
//! `<Aufnahmeordner>/durchsagen/*.mp3`) entstehen ohne Zutun des Hoerers -
//! jede Verkehrsdurchsage eine MP3 - und wuerden den Datentraeger irgendwann
//! fuellen. Deshalb wird der Unterordner beim Start, nach jedem beendeten
//! Mitschnitt bzw. jeder beendeten Aufnahme und nach einer Aenderung der
//! Grenzen beschnitten: hoechstens `Settings::announcement_keep_files`
//! Dateien und hoechstens `Settings::announcement_keep_mb` MB, die aeltesten
//! zuerst weg (0 = unbegrenzt). Eine gerade laufende Aufzeichnung wird nie
//! angefasst; geloeschte Dateien verlieren ihren Abspielknopf in der Liste.
//!
//! Manuelle Aufnahmen und Musik-Exporte liegen im Aufnahmeordner selbst und
//! werden NIE automatisch geloescht - die hat der Hoerer bewusst angelegt.
//! Fuer beide Ordner zeigt die UI (Einstellungen > Aufnahme) nur Zahl und
//! Groesse der Dateien (`AppState::storage`, `AppEvent::Storage`).

use crate::app::{App, AppEvent, Effects};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::time::SystemTime;

/// Zahl und Gesamtgroesse der Dateien direkt in einem Ordner (ohne Unterordner).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
#[serde(default)]
pub struct DirUsage {
    pub files: u32,
    pub bytes: u64,
}

/// Belegung des Aufnahmeordners und des Durchsagen-Unterordners.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
#[serde(default)]
pub struct StorageInfo {
    /// Aufnahmeordner (Einstellung oder `data/recordings`), zur Anzeige.
    pub recording_dir: String,
    /// `<Aufnahmeordner>/durchsagen`.
    pub announcement_dir: String,
    /// Aufnahmen (WAV) und Musik-Exporte (MP3) im Aufnahmeordner selbst.
    pub recordings: DirUsage,
    /// Durchsage-/Warnungs-Mitschnitte im Unterordner.
    pub announcements: DirUsage,
}

/// Dateien direkt in `dir` (keine Unterordner), aelteste zuerst: nach
/// Aenderungszeit, bei Gleichstand nach Name (der mit dem Zeitstempel beginnt).
fn list_files(dir: &Path) -> Vec<(PathBuf, u64, SystemTime)> {
    let Ok(rd) = std::fs::read_dir(dir) else { return Vec::new() };
    let mut files: Vec<(PathBuf, u64, SystemTime)> = rd
        .flatten()
        .filter_map(|e| {
            let md = e.metadata().ok()?;
            if !md.is_file() {
                return None;
            }
            Some((e.path(), md.len(), md.modified().unwrap_or(SystemTime::UNIX_EPOCH)))
        })
        .collect();
    files.sort_by(|a, b| a.2.cmp(&b.2).then_with(|| a.0.cmp(&b.0)));
    files
}

pub fn dir_usage(dir: &Path) -> DirUsage {
    let files = list_files(dir);
    DirUsage { files: files.len() as u32, bytes: files.iter().map(|f| f.1).sum() }
}

impl App {
    /// Belegung beider Ordner neu zaehlen; Ereignis nur bei Aenderung.
    pub fn storage_refresh(&mut self) -> Effects {
        let rec = self.recording_dir();
        let ann = self.announcement_dir();
        let info = StorageInfo {
            recording_dir: rec.display().to_string(),
            announcement_dir: ann.display().to_string(),
            recordings: dir_usage(&rec),
            announcements: dir_usage(&ann),
        };
        let mut fx = Effects::default();
        if info != self.state.storage {
            self.state.storage = info.clone();
            fx.events.push(AppEvent::Storage { storage: info });
        }
        fx
    }

    /// Durchsagen-Unterordner auf die eingestellten Grenzen beschneiden
    /// (aelteste MP3 zuerst), danach die Belegung neu melden.
    pub fn announcement_prune(&mut self) -> Effects {
        let max_files = self.settings.announcement_keep_files as usize;
        let max_bytes = self.settings.announcement_keep_mb as u64 * 1024 * 1024;
        let mut fx = Effects::default();
        if max_files > 0 || max_bytes > 0 {
            let dir = self.announcement_dir();
            let busy = self.traffic.capture_paths();
            let files: Vec<_> = list_files(&dir)
                .into_iter()
                .filter(|(p, _, _)| p.extension().map(|e| e.eq_ignore_ascii_case("mp3")).unwrap_or(false))
                .collect();
            let mut count = files.len();
            let mut bytes: u64 = files.iter().map(|f| f.1).sum();
            let mut gone = Vec::new();
            for (path, len, _) in files {
                let over_files = max_files > 0 && count > max_files;
                let over_bytes = max_bytes > 0 && bytes > max_bytes;
                if !over_files && !over_bytes {
                    break;
                }
                if busy.iter().any(|b| b == &path) {
                    continue; // laeuft gerade, bleibt
                }
                match std::fs::remove_file(&path) {
                    Ok(()) => {
                        count -= 1;
                        bytes = bytes.saturating_sub(len);
                        gone.push(path);
                    }
                    Err(e) => log::warn!("Durchsagen aufraeumen: {} nicht loeschbar: {e}", path.display()),
                }
            }
            if !gone.is_empty() {
                log::info!(
                    "Durchsagen aufraeumen: {} alte Mitschnitte geloescht (Grenze {} Dateien / {} MB), {} Dateien bleiben",
                    gone.len(),
                    self.settings.announcement_keep_files,
                    self.settings.announcement_keep_mb,
                    count
                );
                fx.append(self.traffic_forget_files(&gone));
            }
        }
        fx.append(self.storage_refresh());
        fx
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{DataDirs, Presets, Settings};
    use std::io::Write;

    fn app() -> App {
        let nanos = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        let tmp = std::env::temp_dir().join(format!("dabclassic-storage-{}-{nanos}", std::process::id()));
        App::with(DataDirs::with_root(&tmp, true), Settings::default(), Presets::default())
    }

    /// Datei mit `kb` KB und aufsteigender Aenderungszeit (Reihenfolge = Alter).
    fn mp3(dir: &Path, name: &str, kb: usize, age_index: u64) -> PathBuf {
        std::fs::create_dir_all(dir).unwrap();
        let p = dir.join(name);
        let mut f = std::fs::File::create(&p).unwrap();
        f.write_all(&vec![0u8; kb * 1024]).unwrap();
        drop(f);
        let t = SystemTime::UNIX_EPOCH + std::time::Duration::from_secs(1_700_000_000 + age_index * 60);
        std::fs::File::options().write(true).open(&p).unwrap().set_modified(t).unwrap();
        p
    }

    #[test]
    fn usage_counts_only_files_of_the_folder_itself() {
        let mut a = app();
        let rec = a.recording_dir();
        let ann = a.announcement_dir();
        mp3(&rec, "20260917_100000_Dlf_Titel.wav", 10, 1);
        mp3(&ann, "20260917_100100_WDR_2_Verkehrsdurchsage.mp3", 3, 2);
        mp3(&ann, "20260917_100200_WDR_2_Verkehrsdurchsage.mp3", 4, 3);
        let fx = a.storage_refresh();
        assert!(matches!(fx.events.as_slice(), [AppEvent::Storage { .. }]));
        assert_eq!(a.state.storage.recordings, DirUsage { files: 1, bytes: 10 * 1024 }, "Unterordner zaehlt nicht mit");
        assert_eq!(a.state.storage.announcements, DirUsage { files: 2, bytes: 7 * 1024 });
        assert_eq!(a.state.storage.announcement_dir, ann.display().to_string());
        assert!(a.storage_refresh().events.is_empty(), "unveraendert -> kein Ereignis");
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn prune_removes_the_oldest_beyond_the_file_limit_and_drops_list_references() {
        let mut a = app();
        a.settings.announcement_keep_files = 2;
        a.settings.announcement_keep_mb = 0;
        let ann = a.announcement_dir();
        let oldest = mp3(&ann, "20260917_100000_WDR_2_Verkehrsdurchsage.mp3", 1, 1);
        let mid = mp3(&ann, "20260917_100100_WDR_2_Verkehrsdurchsage.mp3", 1, 2);
        let newest = mp3(&ann, "20260917_100200_WDR_2_Verkehrsdurchsage.mp3", 1, 3);
        mp3(&ann, "notizen.txt", 1, 0); // fremde Datei bleibt
        a.state.traffic_history.push(crate::traffic::TrafficEntry { id: 1, file: Some(oldest.display().to_string()), ..Default::default() });
        a.state.traffic_history.push(crate::traffic::TrafficEntry { id: 2, file: Some(mid.display().to_string()), ..Default::default() });
        let fx = a.announcement_prune();
        assert!(!oldest.exists() && mid.exists() && newest.exists());
        assert!(ann.join("notizen.txt").exists());
        assert_eq!(a.state.traffic_history[0].file, None, "Abspielknopf weg");
        assert!(a.state.traffic_history[1].file.is_some());
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::Traffic { .. })));
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::Storage { .. })));
        assert_eq!(a.state.storage.announcements.files, 3);
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn prune_by_size_and_zero_means_unlimited() {
        let mut a = app();
        let ann = a.announcement_dir();
        let old = mp3(&ann, "20260917_100000_WDR_2_Verkehrsdurchsage.mp3", 600, 1);
        let new = mp3(&ann, "20260917_100100_WDR_2_Verkehrsdurchsage.mp3", 600, 2);
        a.settings.announcement_keep_files = 0;
        a.settings.announcement_keep_mb = 0;
        a.announcement_prune();
        assert!(old.exists() && new.exists(), "0/0 = unbegrenzt");
        a.settings.announcement_keep_mb = 1; // 1 MB < 1200 KB
        a.announcement_prune();
        assert!(!old.exists() && new.exists());
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }
}
