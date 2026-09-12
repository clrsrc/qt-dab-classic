//! Aufnahme des Primary-Dienstes als WAV ueber den Kern (`start_recording` /
//! `stop_recording`, Zustand aus `recording_state`). Dateinamen nach dem
//! v1-Schema von `recording-manager.cpp`:
//! `yyyyMMdd_HHmmss_<Dienst auf 16 Zeichen mit _ aufgefuellt>_<Titel>.wav`,
//! z. B. `20260328_121101_Dlf______________Informationen_am_Mittag.wav`.
//! Zielordner: Einstellung `recording_dir`, sonst `data/recordings`.
//!
//! Umschaltsperre (Projektregel, Entscheidung 8/24): Kanal-, Dienstwechsel,
//! Preset-Abruf und Scan werden waehrend einer Aufnahme mit
//! [`AppError::Recording`] abgewiesen; das Frontend bietet dann "Aufnahme
//! beenden und wechseln" an.

use crate::app::{App, AppError, AppEvent, Effects};
use chrono::{DateTime, Local};
use dab_api::{Command, Event, RecFormat, ServiceSlot};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// Hoechstlaenge des Titelteils im Dateinamen (v1: 40).
pub const TITLE_MAX: usize = 40;
/// Breite des Dienstnamens im Dateinamen (v1: FIC-Label, 16 Zeichen).
pub const SERVICE_WIDTH: usize = 16;

/// Zustand der Aufnahme fuer das Frontend (`recording_changed`).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
#[serde(default)]
pub struct RecordingInfo {
    pub active: bool,
    pub path: Option<String>,
    pub bytes: u64,
    pub seconds: f64,
    pub sid: u32,
    pub service: String,
    pub title: String,
    /// Unix-Zeit des Starts.
    pub started_at: i64,
    /// Timer, der die Aufnahme gestartet hat.
    pub timer_id: Option<u32>,
    /// Geplantes Ende (Timer inkl. Nachlauf).
    pub stop_at: Option<i64>,
}

#[derive(Debug, Default)]
pub struct Recording {
    pub info: RecordingInfo,
}

/// v1: alles ausser `[a-zA-Z0-9_-]` wird `_`.
pub fn clean_component(s: &str) -> String {
    s.chars().map(|c| if c.is_ascii_alphanumeric() || c == '_' || c == '-' { c } else { '_' }).collect()
}

/// Dateiname nach v1-Schema. `service` wird wie das FIC-Label auf 16 Zeichen
/// aufgefuellt (Leerzeichen -> `_`), `title` auf 40 Zeichen gekuerzt.
pub fn file_name(at: &DateTime<Local>, service: &str, title: &str) -> String {
    let mut svc = service.trim().chars().take(SERVICE_WIDTH).collect::<String>();
    while svc.chars().count() < SERVICE_WIDTH {
        svc.push(' ');
    }
    let svc = clean_component(&svc);
    let title: String = title.trim().chars().take(TITLE_MAX).collect();
    let title = clean_component(&title).trim_matches('_').to_string();
    format!("{}_{}_{}.wav", at.format("%Y%m%d_%H%M%S"), svc, title)
}

impl App {
    /// Zielordner (Einstellung oder `data/recordings`).
    pub fn recording_dir(&self) -> PathBuf {
        match &self.settings.recording_dir {
            Some(p) if !p.as_os_str().is_empty() => p.clone(),
            _ => self.dirs.recordings_dir(),
        }
    }

    /// Titel fuer eine manuelle Aufnahme: DL+ ITEM.TITLE, sonst DLS-Text.
    pub fn current_title(&self) -> String {
        if let Some(t) = self.state.dl_plus.as_ref().and_then(|d| d.title()) {
            if !t.trim().is_empty() {
                return t.trim().to_string();
            }
        }
        self.state.dls.trim().to_string()
    }

    /// Aufnahme des laufenden Dienstes starten. `title` = None: DL+/DLS.
    /// `timer` = (Timer-Id, geplantes Ende); eine laufende Timer-Aufnahme
    /// desselben Dienstes wird dafuer beendet (Kettenaufnahme).
    pub fn recording_start(&mut self, title: Option<&str>, timer: Option<(u32, Option<i64>)>) -> Result<Effects, AppError> {
        let current = self.state.current.clone().ok_or(AppError::NoService)?;
        let name = self
            .state
            .service(current.sid, current.scids)
            .map(|s| s.name.trim().to_string())
            .unwrap_or_else(|| format!("{:04X}", current.sid));
        let mut fx = Effects::default();
        if self.state.recording || self.rec.info.active {
            if timer.is_some() && self.rec.info.timer_id.is_some() {
                fx.commands.push(Command::StopRecording { slot: ServiceSlot::Primary, sid: None });
            } else {
                return Err(AppError::Recording);
            }
        }
        let title = match title {
            Some(t) => t.trim().to_string(),
            None => self.current_title(),
        };
        let dir = self.recording_dir();
        if let Err(e) = std::fs::create_dir_all(&dir) {
            return Err(AppError::Other(format!("{}: {e}", dir.display())));
        }
        let path = dir.join(file_name(&Local::now(), &name, &title));
        self.rec.info = RecordingInfo {
            active: true,
            path: Some(path.display().to_string()),
            bytes: 0,
            seconds: 0.0,
            sid: current.sid,
            service: name,
            title,
            started_at: crate::state::unix_now(),
            timer_id: timer.map(|(id, _)| id),
            stop_at: timer.and_then(|(_, stop)| stop),
        };
        // Sperre sofort, nicht erst mit der Bestaetigung des Kerns.
        self.state.recording = true;
        fx.commands.push(Command::StartRecording { path, format: RecFormat::Wav, slot: ServiceSlot::Primary, sid: None });
        fx.events.push(AppEvent::RecordingChanged { recording: self.rec.info.clone() });
        Ok(fx)
    }

    /// Aufnahme beenden (ohne laufende Aufnahme: nichts).
    pub fn recording_stop(&mut self) -> Result<Effects, AppError> {
        let mut fx = Effects::default();
        if !(self.state.recording || self.rec.info.active) {
            return Ok(fx);
        }
        // Sperre sofort aufheben: der Kern bearbeitet stop_recording vor dem naechsten Kommando.
        self.state.recording = false;
        self.rec.info.active = false;
        self.rec.info.timer_id = None;
        self.rec.info.stop_at = None;
        fx.commands.push(Command::StopRecording { slot: ServiceSlot::Primary, sid: None });
        fx.events.push(AppEvent::RecordingChanged { recording: self.rec.info.clone() });
        Ok(fx)
    }

    /// An/aus (Taste R, REC-Knopf).
    pub fn recording_toggle(&mut self) -> Result<Effects, AppError> {
        if self.state.recording || self.rec.info.active {
            self.recording_stop()
        } else {
            self.recording_start(None, None)
        }
    }

    /// `recording_state` des Kerns (1 Hz) in den Zustand uebernehmen.
    pub fn recording_on_event(&mut self, ev: &Event) -> Effects {
        let mut fx = Effects::default();
        match ev {
            Event::RecordingState { slot: ServiceSlot::Primary, sid, active, path, bytes, seconds } => {
                let info = &mut self.rec.info;
                let was = info.active;
                info.active = *active;
                info.bytes = *bytes;
                info.seconds = *seconds;
                if let Some(p) = path {
                    info.path = Some(p.display().to_string());
                }
                if *active && info.sid == 0 {
                    info.sid = *sid;
                }
                if !*active {
                    info.timer_id = None;
                    info.stop_at = None;
                } else if !was && info.started_at == 0 {
                    info.started_at = crate::state::unix_now();
                }
                fx.events.push(AppEvent::RecordingChanged { recording: info.clone() });
            }
            Event::Exiting { .. } | Event::DeviceClosed => {
                if self.rec.info.active {
                    self.rec.info.active = false;
                    self.rec.info.timer_id = None;
                    self.rec.info.stop_at = None;
                    self.state.recording = false;
                    fx.events.push(AppEvent::RecordingChanged { recording: self.rec.info.clone() });
                }
            }
            _ => {}
        }
        fx
    }

    /// Geplantes Ende einer manuell gesetzten Dauer (Timer-Ende regelt der Scheduler).
    pub fn recording_tick(&mut self, now: i64) -> Effects {
        match self.rec.info.stop_at {
            Some(stop) if self.rec.info.active && self.rec.info.timer_id.is_none() && now >= stop => {
                self.recording_stop().unwrap_or_default()
            }
            _ => Effects::default(),
        }
    }

    pub fn recording_info(&self) -> &RecordingInfo {
        &self.rec.info
    }

    pub fn is_recording(&self) -> bool {
        self.state.recording || self.rec.info.active
    }
}

/// Hilfe fuer Tests/Tools: existiert die Datei und ist sie nicht leer?
pub fn wav_ok(path: &Path) -> bool {
    std::fs::metadata(path).map(|m| m.len() > 44).unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use chrono::TimeZone;

    #[test]
    fn file_name_matches_v1_examples() {
        let at = Local.with_ymd_and_hms(2026, 3, 28, 12, 11, 1).unwrap();
        assert_eq!(file_name(&at, "Dlf", "Informationen am Mittag"), "20260328_121101_Dlf______________Informationen_am_Mittag.wav");
        assert_eq!(file_name(&at, "Dlf             ", "Kalenderblatt"), "20260328_121101_Dlf______________Kalenderblatt.wav");
        // 16-Zeichen-Name, Umlaute, Kuerzung auf 40
        let long = "Das ist ein sehr langer Titel mit Umlauten äöü und noch mehr Text dahinter";
        let n = file_name(&at, "WDR 2 RHEIN-RUHR", long);
        assert!(n.starts_with("20260328_121101_WDR_2_RHEIN-RUHR_Das_ist_ein_sehr_langer_Titel_mit_Umlau"), "{n}");
        assert!(n.ends_with(".wav"));
        let title_part = n.trim_start_matches("20260328_121101_WDR_2_RHEIN-RUHR_").trim_end_matches(".wav");
        assert!(title_part.chars().count() <= TITLE_MAX);
        // Leerer Titel
        assert_eq!(file_name(&at, "Dlf", ""), "20260328_121101_Dlf______________.wav");
    }

    #[test]
    fn clean_component_rules() {
        assert_eq!(clean_component("a b/c:d.e"), "a_b_c_d_e");
        assert_eq!(clean_component("ok_-09"), "ok_-09");
    }
}
