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
    /// Eigenes `stop_recording` ist unterwegs, das bestaetigende
    /// `recording_state(active=false)` des Kerns steht noch aus. Bis dahin
    /// gilt ein bereits abgeschicktes 1-Hz `active=true` derselben Datei als
    /// veraltet und darf die Sperre nicht wieder setzen (Review 16.09.2026,
    /// Befund 7).
    pub(crate) stop_pending: bool,
}

/// Gehoert ein `recording_state`-Pfad des Kerns zu unserer Datei? Der Kern
/// gibt den Pfad zurueck, den wir ihm geschickt haben; der Vergleich ueber
/// `Path` ist unempfindlich gegen `/` vs. `\`.
fn same_file(a: &Path, b: &str) -> bool {
    a == Path::new(b)
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
        // Neue Datei, neuer Zustand: ein noch ausstehendes Stop-Echo der
        // vorigen Datei wird ueber den Pfad-Abgleich in
        // `recording_on_event` aussortiert, nicht ueber dieses Flag.
        self.rec.stop_pending = false;
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
        // Vorlauf aus dem Timeshift-Ring nur bei Aufnahme-Timern (Entscheidung 18);
        // eine manuelle Aufnahme (Taste R) beginnt bewusst erst ab jetzt.
        let pre_s = if timer.is_some() { self.settings.record_pre_s as f64 } else { 0.0 };
        fx.commands.push(Command::StartRecording { path, format: RecFormat::Wav, slot: ServiceSlot::Primary, sid: None, pre_s });
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
        // Bis der Kern das Ende bestaetigt, darf ein noch unterwegs
        // befindliches 1-Hz `active=true` die Sperre nicht wieder setzen.
        self.rec.stop_pending = true;
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
    ///
    /// Der Kern meldet unter demselben Ereignistyp auch das Ende eines
    /// Timeshift-/Musik-Exports (core.cpp `startExportThread(...,
    /// reportRecordingState=true)`: `active=false` mit dem EXPORT-Pfad) und
    /// bei einer Kettenaufnahme das synchrone Ende der VORIGEN Datei. Beides
    /// darf die laufende Aufnahme nicht beenden (Review 16.09.2026, Befund 1);
    /// deshalb wird jedes Ereignis zuerst ueber den Pfad der eigenen Datei
    /// zugeordnet. `state.recording` (Umschaltsperre) wird nur hier gesetzt,
    /// nicht mehr blind in `AppState::apply`.
    pub fn recording_on_event(&mut self, ev: &Event) -> Effects {
        let mut fx = Effects::default();
        match ev {
            Event::RecordingState { slot: ServiceSlot::Primary, sid, active, path, bytes, seconds } => {
                // Pfad passt zur eigenen Datei?
                let matches_mine = matches!((path.as_deref(), self.rec.info.path.as_deref()), (Some(p), Some(m)) if same_file(p, m));
                // Ohne Pfad nicht zuzuordnen: gilt als eigene Aufnahme (der
                // Kern liefert `null` nur ohne offene Datei). Fremder Pfad:
                // Export-Ende oder Ende einer verdraengten Kettenaufnahme ->
                // nicht unsere Sache. Einzig eine LAUFENDE Aufnahme, von der
                // wir nichts wissen, wird uebernommen.
                let foreign_running = *active && !self.rec.info.active && !self.state.recording && !self.rec.stop_pending;
                let ours = matches_mine || path.is_none() || foreign_running;
                if !ours {
                    log::debug!("recording_state ({}) fuer fremde Datei {:?} ignoriert", if *active { "an" } else { "aus" }, path);
                    return fx;
                }
                if self.rec.stop_pending {
                    if *active {
                        // Veraltetes 1-Hz-Ereignis nach eigenem Stop (Befund 7).
                        return fx;
                    }
                    self.rec.stop_pending = false;
                }
                if foreign_running && path.is_some() && !matches_mine {
                    // Unbekannte laufende Aufnahme: alte Anzeige verwerfen.
                    self.rec.info = RecordingInfo::default();
                }
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
                self.state.recording = *active;
                fx.events.push(AppEvent::RecordingChanged { recording: info.clone() });
            }
            Event::Exiting { .. } | Event::DeviceClosed => {
                self.rec.stop_pending = false;
                if self.rec.info.active || self.state.recording {
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

    // -----------------------------------------------------------------------
    // Zuordnung von `recording_state` (Review 16.09.2026, Befunde 1 und 7)
    // -----------------------------------------------------------------------

    use crate::{DataDirs, Presets, Settings};
    use dab_api::{Codec, ServiceInfo};
    use std::time::Instant;

    fn app() -> App {
        let nanos = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        let dir = std::env::temp_dir().join(format!("dabclassic-rec-{}-{nanos}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let mut a = App::with(DataDirs::with_root(&dir, true), Settings::default(), Presets::default());
        a.state.device = Some(crate::state::DeviceState { kind: "hackrf".into(), ..Default::default() });
        a.state.channel = Some("5C".into());
        let now = Instant::now();
        a.handle_event(&Event::EnsembleFound { eid: 0x10BC, name: "Ens".into(), channel: "5C".into() }, now);
        a.handle_event(
            &Event::ServiceAdded { service: ServiceInfo { sid: 0xD210, scids: 0, name: "Dlf".into(), is_audio: true, is_primary: true, sub_ch: 1, bitrate_kbps: 96, pty: 0, short_name: String::new(), language: 0 } },
            now,
        );
        a.handle_event(
            &Event::ServiceStarted { slot: ServiceSlot::Primary, sid: 0xD210, scids: 0, codec: Codec::HeAac { sbr: true, ps: false, sample_rate: 48000 }, stereo: true },
            now,
        );
        a
    }

    fn rec_state(active: bool, path: Option<&str>) -> Event {
        Event::RecordingState { slot: ServiceSlot::Primary, sid: 0xD210, active, path: path.map(PathBuf::from), bytes: 100, seconds: 1.0 }
    }

    fn started_path(fx: &Effects) -> String {
        fx.commands
            .iter()
            .find_map(|c| match c {
                Command::StartRecording { path, .. } => Some(path.display().to_string()),
                _ => None,
            })
            .expect("start_recording")
    }

    /// Befund 1a: Kettenaufnahme. Das synchrone `active=false` der ALTEN Datei
    /// (Antwort des Kerns auf das `stop_recording` der Kette) trifft ein,
    /// nachdem `rec.info` schon die neue Aufnahme beschreibt - es darf die
    /// neue `timer_id` nicht loeschen, sonst findet der Scheduler das Ende
    /// des Folge-Timers nie und die Aufnahme laeuft endlos.
    #[test]
    fn chained_recording_keeps_the_new_timer_id_when_the_old_stop_echo_arrives() {
        let mut a = app();
        let now = Instant::now();
        let fx = a.recording_start(Some("Erste"), Some((1, Some(1_800_000_000)))).unwrap();
        let first = started_path(&fx);
        a.handle_event(&rec_state(true, Some(&first)), now);
        assert_eq!(a.rec.info.timer_id, Some(1));

        // Folge-Timer 2 desselben Dienstes: stop + start hintereinander.
        let fx = a.recording_start(Some("Zweite"), Some((2, Some(1_800_003_600)))).unwrap();
        assert!(matches!(fx.commands[0], Command::StopRecording { .. }));
        let second = started_path(&fx);
        assert_ne!(first, second);
        assert_eq!(a.rec.info.timer_id, Some(2));

        // Echo des Kerns fuer die alte Datei: active=false mit ALTEM Pfad.
        let fx = a.handle_event(&rec_state(false, Some(&first)), now);
        assert_eq!(a.rec.info.timer_id, Some(2), "Stop-Echo der alten Datei hat die neue timer_id geloescht");
        assert!(a.rec.info.active, "neue Aufnahme gilt weiter als aktiv");
        assert!(a.state.recording, "Umschaltsperre bleibt");
        assert!(!fx.events.iter().any(|e| matches!(e, AppEvent::RecordingChanged { .. })), "fremdes Ereignis erzeugt keine Meldung");

        // Bestaetigung der neuen Datei: alles bleibt konsistent.
        a.handle_event(&rec_state(true, Some(&second)), now);
        assert_eq!(a.rec.info.timer_id, Some(2));
        assert_eq!(a.rec.info.path.as_deref(), Some(second.as_str()));

        // Echtes Ende der neuen Datei beendet die Aufnahme.
        a.handle_event(&rec_state(false, Some(&second)), now);
        assert!(!a.rec.info.active && !a.state.recording);
        assert_eq!(a.rec.info.timer_id, None);
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    /// Befund 1b: Timeshift-/Musik-Export waehrend einer Aufnahme. Der Kern
    /// meldet das Export-Ende als `recording_state(active=false, <Exportpfad>)`;
    /// die laufende Aufnahme darf davon nichts merken (Sperre, timer_id).
    #[test]
    fn export_finished_event_does_not_end_the_running_recording() {
        let mut a = app();
        let now = Instant::now();
        let fx = a.recording_start(None, Some((7, None))).unwrap();
        let mine = started_path(&fx);
        a.handle_event(&rec_state(true, Some(&mine)), now);

        let export = a.dirs.music_dir().join("20260916_120000_Dlf_Titel.mp3");
        let fx = a.handle_event(&rec_state(false, Some(&export.display().to_string())), now);
        assert!(a.state.recording, "Export-Ende hat die Umschaltsperre aufgehoben");
        assert!(a.rec.info.active);
        assert_eq!(a.rec.info.timer_id, Some(7));
        assert_eq!(a.rec.info.path.as_deref(), Some(mine.as_str()), "Exportpfad darf die Aufnahme nicht ueberschreiben");
        assert!(fx.events.is_empty(), "kein RecordingChanged fuer den Export: {:?}", fx.events);
        // Ein Export-Ende OHNE laufende Aufnahme ist ebenso keine Aufnahmemeldung.
        a.handle_event(&rec_state(false, Some(&mine)), now);
        assert!(!a.state.recording);
        let fx = a.handle_event(&rec_state(false, Some(&export.display().to_string())), now);
        assert!(fx.events.is_empty());
        assert_eq!(a.rec.info.path.as_deref(), Some(mine.as_str()));
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    /// Befund 7: nach eigenem `recording_stop` darf ein noch unterwegs
    /// befindliches 1-Hz `active=true` die Sperre nicht wieder setzen; erst
    /// das `active=false` des Kerns schliesst den Vorgang ab.
    #[test]
    fn stale_active_after_own_stop_does_not_relock() {
        let mut a = app();
        let now = Instant::now();
        let fx = a.recording_start(None, None).unwrap();
        let mine = started_path(&fx);
        a.handle_event(&rec_state(true, Some(&mine)), now);
        a.recording_stop().unwrap();
        assert!(!a.state.recording);
        let fx = a.handle_event(&rec_state(true, Some(&mine)), now);
        assert!(!a.state.recording, "veraltetes active=true hat die Sperre wieder gesetzt");
        assert!(!a.rec.info.active);
        assert!(fx.events.is_empty());
        a.handle_event(&rec_state(false, Some(&mine)), now);
        assert!(!a.state.recording && !a.rec.info.active);
        // Danach ist der Weg fuer eine neue Aufnahme frei, ihr active=true zaehlt wieder.
        let fx = a.recording_start(None, None).unwrap();
        let next = started_path(&fx);
        a.handle_event(&rec_state(true, Some(&next)), now);
        assert!(a.state.recording && a.rec.info.active);
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }
}
