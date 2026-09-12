//! Datenordner: portabel (`data/` neben der EXE) oder Benutzerprofil.

use std::path::{Path, PathBuf};

/// Alle Ablageorte der App.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DataDirs {
    pub root: PathBuf,
    pub portable: bool,
}

impl DataDirs {
    /// Reihenfolge: Umgebungsvariable `DABCLASSIC_DATA` (Entwicklung/Tests),
    /// dann portabel (`data/` oder `portable.txt` neben der EXE), im
    /// Entwicklungsbaum (`target/debug/`) auch `apps/desktop/data/`, sonst
    /// `%APPDATA%\DAB Classic`.
    pub fn detect() -> Self {
        if let Some(env) = std::env::var_os("DABCLASSIC_DATA").filter(|v| !v.is_empty()) {
            return Self { root: PathBuf::from(env), portable: true };
        }
        if let Ok(exe) = std::env::current_exe() {
            if let Some(dir) = exe.parent() {
                if let Some(d) = Self::detect_in(dir) {
                    return d;
                }
                if let Some(d) = Self::detect_dev(dir) {
                    return d;
                }
            }
        }
        Self::user_profile()
    }

    /// Entwicklungsmodus: EXE unter `<workspace>/target/{debug,release}/`,
    /// dort `apps/desktop/data/` als portabler Ordner (falls vorhanden).
    pub fn detect_dev(exe_dir: &Path) -> Option<Self> {
        let mut dir = exe_dir.to_path_buf();
        for _ in 0..4 {
            let data = dir.join("apps").join("desktop").join("data");
            if data.is_dir() {
                return Some(Self { root: data, portable: true });
            }
            if !dir.pop() {
                break;
            }
        }
        None
    }

    /// Wie [`detect`](Self::detect), aber fuer ein gegebenes EXE-Verzeichnis (testbar).
    pub fn detect_in(exe_dir: &Path) -> Option<Self> {
        let data = exe_dir.join("data");
        if data.is_dir() || exe_dir.join("portable.txt").is_file() {
            return Some(Self { root: data, portable: true });
        }
        None
    }

    pub fn user_profile() -> Self {
        let base = std::env::var_os("APPDATA")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("."));
        Self { root: base.join("DAB Classic"), portable: false }
    }

    pub fn with_root(root: impl Into<PathBuf>, portable: bool) -> Self {
        Self { root: root.into(), portable }
    }

    pub fn settings_file(&self) -> PathBuf { self.root.join("settings.json") }
    pub fn presets_file(&self) -> PathBuf { self.root.join("presets.json") }
    pub fn timers_file(&self) -> PathBuf { self.root.join("timers.json") }
    /// Senderliste ueber alle Ensembles (crate::stations).
    pub fn stations_file(&self) -> PathBuf { self.root.join("stations.json") }
    pub fn epg_dir(&self) -> PathBuf { self.root.join("epg") }
    pub fn logos_dir(&self) -> PathBuf { self.root.join("logos") }
    pub fn recordings_dir(&self) -> PathBuf { self.root.join("recordings") }
    pub fn music_dir(&self) -> PathBuf { self.root.join("music") }
    pub fn timeshift_dir(&self) -> PathBuf { self.root.join("timeshift") }
    pub fn log_dir(&self) -> PathBuf { self.root.join("log") }

    /// Legt alle Unterordner an.
    pub fn ensure(&self) -> std::io::Result<()> {
        for d in [
            self.root.clone(),
            self.epg_dir(),
            self.logos_dir(),
            self.recordings_dir(),
            self.music_dir(),
            self.log_dir(),
        ] {
            std::fs::create_dir_all(d)?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn portable_detection() {
        let tmp = std::env::temp_dir().join(format!("dabclassic-paths-{}", std::process::id()));
        std::fs::create_dir_all(&tmp).unwrap();
        assert!(DataDirs::detect_in(&tmp).is_none());
        std::fs::create_dir_all(tmp.join("data")).unwrap();
        let d = DataDirs::detect_in(&tmp).unwrap();
        assert!(d.portable);
        assert_eq!(d.root, tmp.join("data"));
        std::fs::remove_dir_all(&tmp).unwrap();
    }
}
