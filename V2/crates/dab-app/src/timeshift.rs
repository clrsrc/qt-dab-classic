//! Timeshift-Steuerung (Entscheidungen 4, 5, 18, 21; Plan M4 Abschnitt 2).
//!
//! Der Ring liegt im Kern (`libdabcore`), diese Schicht ist nur die Steuerung:
//! Der Zustand kommt ausschliesslich aus dem Kern (`timeshift_state` bzw.
//! `state_snapshot.timeshift`) – nichts wird hier geraten oder hochgezaehlt.
//! Jede Bedienung liefert [`Effects`] mit dem passenden Kern-Kommando.
//!
//! Regeln:
//! - Bei laufender Aufnahme ist die Bedienung **erlaubt** (die Aufnahme laeuft
//!   live weiter, der Kern trennt Ring und Aufnahme).
//! - Dienst-/Kanalwechsel und die EWS-Umschaltung leeren den Ring im Kern; hier
//!   wird die Anzeige sofort auf "live" zurueckgesetzt, damit die Leiste nicht
//!   kurz einen falschen Versatz zeigt.
//! - Kapazitaet kommt aus `settings.timeshift_capacity_s` (beim Start und bei
//!   jeder Aenderung als `timeshift_configure`).

use crate::app::{App, AppError, AppEvent, Effects};
use dab_api::{Command, Event, ServiceSlot, TimeshiftBacking, TimeshiftMode};
use serde::{Deserialize, Serialize};

/// Sprungweite je Tastendruck/Knopf (Entscheidung 21: Pfeil links/rechts).
pub const SKIP_STEP_S: f64 = 30.0;
/// Bereich der Ringkapazitaet (Plan M4 1.3): 1 min .. 4 h.
pub const CAPACITY_MIN_S: u32 = 60;
pub const CAPACITY_MAX_S: u32 = 14_400;
/// Umgebungsvariable fuer die Sichtpruefung der Leiste ohne Kern-Funktion:
/// `DABCLASSIC_TS_DEMO=1` setzt einen festen Demo-Zustand (Plan M4, Abnahme
/// der Shell-Optik). Ohne die Variable passiert nichts.
pub const DEMO_ENV: &str = "DABCLASSIC_TS_DEMO";

/// Zustand des Timeshift-Puffers fuer das Frontend (Teil von [`crate::AppState`]).
/// Alle Werte stammen vom Kern; `demo` ist nur der Sichtpruefungs-Schalter.
#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq)]
#[serde(default)]
pub struct TimeshiftInfo {
    pub mode: TimeshiftMode,
    /// Inhalt des Rings in Sekunden.
    pub buffered_s: f64,
    /// Abstand des Lesezeigers zu live in Sekunden (0 = live).
    pub offset_s: f64,
    /// Eingestellte Ringgroesse in Sekunden.
    pub capacity_s: f64,
    /// Ensemble-Uhrzeit am Schreibzeiger (0 = unbekannt).
    pub live_unix: i64,
    /// Schreibzeiger in Logikrahmen (24 ms). Bezugsgroesse der Musik-Trennung
    /// (crate::music): daran haengen `start_frame`/`end_frame` der Kandidaten.
    pub frame_index: u64,
    /// Nur mit [`DEMO_ENV`]: Leiste auch ohne Kern-Funktion anzeigen.
    pub demo: bool,
}

impl Default for TimeshiftInfo {
    fn default() -> Self {
        Self { mode: TimeshiftMode::Live, buffered_s: 0.0, offset_s: 0.0, capacity_s: 0.0, live_unix: 0, frame_index: 0, demo: false }
    }
}

impl TimeshiftInfo {
    pub fn is_live(&self) -> bool {
        self.mode == TimeshiftMode::Live && self.offset_s <= 0.0
    }

    /// Zurueck auf live; Kapazitaet und Demo-Schalter bleiben stehen.
    pub(crate) fn reset(&mut self) {
        if self.demo {
            return;
        }
        self.mode = TimeshiftMode::Live;
        self.buffered_s = 0.0;
        self.offset_s = 0.0;
        self.live_unix = 0;
    }
}

/// Hinweis der Timeshift-Schicht an das Frontend (i18n-Schluessel `ts.notice.*`).
#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum TimeshiftNotice {
    /// Notfallwarnung hat den Puffer verworfen (Entscheidung 5).
    EwsDropped,
    /// Dienst- oder Kanalwechsel: neuer Puffer (Entscheidung 4).
    ServiceChanged,
}

/// Kapazitaet auf den erlaubten Bereich klemmen.
pub fn clamp_capacity(capacity_s: u32) -> u32 {
    capacity_s.clamp(CAPACITY_MIN_S, CAPACITY_MAX_S)
}

/// Kurzform: ein Kommando als [`Effects`].
fn one(cmd: Command) -> Effects {
    let mut fx = Effects::default();
    fx.commands.push(cmd);
    fx
}

fn demo_enabled() -> bool {
    std::env::var(DEMO_ENV).map(|v| v != "0" && !v.is_empty()).unwrap_or(false)
}

impl App {
    // -----------------------------------------------------------------------
    // Start / Einstellungen
    // -----------------------------------------------------------------------

    /// Aus [`App::with`](crate::App::with): Kapazitaet der Einstellungen in den
    /// Zustand uebernehmen, damit die Leiste schon vor der ersten Kern-Meldung
    /// einen sinnvollen Massstab hat. Mit [`DEMO_ENV`] zusaetzlich der
    /// Demo-Zustand fuer die Sichtpruefung.
    pub(crate) fn timeshift_init(&mut self) {
        self.state.timeshift.capacity_s = clamp_capacity(self.settings.timeshift_capacity_s) as f64;
        if demo_enabled() {
            log::info!("{DEMO_ENV} gesetzt: Timeshift-Leiste zeigt einen Demo-Zustand");
            self.state.timeshift.demo = true;
            self.state.timeshift.buffered_s = 1800.0;
            self.state.timeshift.offset_s = 95.0;
            self.state.timeshift.mode = TimeshiftMode::Paused;
        }
    }

    /// Aus [`App::startup`](crate::App::startup): Ringgroesse an den Kern.
    pub fn timeshift_startup(&mut self) -> Effects {
        let capacity_s = clamp_capacity(self.settings.timeshift_capacity_s);
        if !self.state.timeshift.demo {
            self.state.timeshift.capacity_s = capacity_s as f64;
        }
        one(Command::TimeshiftConfigure { capacity_s, backing: TimeshiftBacking::Ram })
    }

    /// Nach `update_settings`: geaenderte Kapazitaet an den Kern (der Kern legt
    /// den Ring neu an, also ist der Puffer danach leer).
    pub fn timeshift_on_settings(&mut self, old: &crate::Settings) -> Effects {
        let capacity_s = clamp_capacity(self.settings.timeshift_capacity_s);
        if clamp_capacity(old.timeshift_capacity_s) == capacity_s {
            return Effects::default();
        }
        self.settings.timeshift_capacity_s = capacity_s;
        self.state.timeshift.reset();
        self.state.timeshift.capacity_s = capacity_s as f64;
        one(Command::TimeshiftConfigure { capacity_s, backing: TimeshiftBacking::Ram })
    }

    // -----------------------------------------------------------------------
    // Ereignisse vom Kern
    // -----------------------------------------------------------------------

    /// Zustand uebernehmen und bei Wechseln zuruecksetzen. Wird aus
    /// [`App::handle_event`](crate::App::handle_event) aufgerufen.
    pub fn timeshift_on_event(&mut self, ev: &Event) -> Effects {
        let mut fx = Effects::default();
        match ev {
            Event::TimeshiftState { mode, buffered_s, offset_s, capacity_s, live_unix, frame_index } => {
                if self.state.timeshift.demo {
                    return fx;
                }
                let ts = &mut self.state.timeshift;
                ts.mode = *mode;
                ts.buffered_s = *buffered_s;
                ts.offset_s = *offset_s;
                ts.live_unix = *live_unix;
                if *frame_index > 0 {
                    ts.frame_index = *frame_index;
                }
                if *capacity_s > 0.0 {
                    ts.capacity_s = *capacity_s;
                }
            }
            Event::StateSnapshot { state } => {
                if self.state.timeshift.demo {
                    return fx;
                }
                match &state.timeshift {
                    Some((mode, buffered_s, offset_s, capacity_s)) => {
                        let ts = &mut self.state.timeshift;
                        ts.mode = *mode;
                        ts.buffered_s = *buffered_s;
                        ts.offset_s = *offset_s;
                        if *capacity_s > 0.0 {
                            ts.capacity_s = *capacity_s;
                        }
                    }
                    // Kern ohne Timeshift (heute): Anzeige bleibt live.
                    None => self.state.timeshift.reset(),
                }
            }
            // Entscheidung 5: Der Alarm verlaesst den Puffer immer.
            Event::EwsSwitched { .. } => fx.append(self.timeshift_dropped(TimeshiftNotice::EwsDropped)),
            // Entscheidung 4: Puffer beginnt bei Dienst-/Senderwechsel neu.
            Event::ServiceStarted { slot: ServiceSlot::Primary, .. }
            | Event::ServiceStopped { slot: ServiceSlot::Primary, .. }
            | Event::DeviceClosed
            | Event::Exiting { .. } => fx.append(self.timeshift_dropped(TimeshiftNotice::ServiceChanged)),
            _ => {}
        }
        fx
    }

    /// Anzeige auf live zuruecksetzen; nur wenn wirklich ein Versatz bestand,
    /// gibt es einen Hinweis im Hauptfenster.
    fn timeshift_dropped(&mut self, notice: TimeshiftNotice) -> Effects {
        if self.state.timeshift.demo {
            return Effects::default();
        }
        let had_offset = !self.state.timeshift.is_live();
        self.state.timeshift.reset();
        if had_offset {
            { let mut fx = Effects::default(); fx.events.push(AppEvent::TimeshiftNotice { notice }); fx }
        } else {
            Effects::default()
        }
    }

    // -----------------------------------------------------------------------
    // Bedienung (Effects je Kommando)
    // -----------------------------------------------------------------------

    /// Timeshift ist nur fuer einen laufenden Audiodienst aus einer
    /// Live-Quelle sinnvoll (bei Datei-Wiedergabe spult man die Datei).
    /// Eine laufende Aufnahme sperrt hier bewusst nichts.
    fn timeshift_ready(&self) -> Result<(), AppError> {
        if self.state.is_file_source() {
            return Err(AppError::Other("timeshift unavailable".into()));
        }
        if self.state.current.is_none() {
            return Err(AppError::NoService);
        }
        Ok(())
    }

    pub fn timeshift_info(&self) -> &TimeshiftInfo {
        &self.state.timeshift
    }

    /// Leertaste / Knopf ⏸▶: pausiert oder setzt ab dem Lesezeiger fort.
    pub fn timeshift_pause_toggle(&mut self) -> Result<Effects, AppError> {
        self.timeshift_ready()?;
        let cmd = match self.state.timeshift.mode {
            TimeshiftMode::Paused => Command::TimeshiftPlay,
            _ => Command::TimeshiftPause,
        };
        Ok(one(cmd))
    }

    /// Pfeil links/rechts bzw. Knoepfe −30/+30: `+` geht Richtung live.
    pub fn timeshift_skip(&mut self, delta_s: f64) -> Result<Effects, AppError> {
        self.timeshift_ready()?;
        if !delta_s.is_finite() || delta_s == 0.0 {
            return Ok(Effects::default());
        }
        Ok(one(Command::TimeshiftSkip { delta_s }))
    }

    /// Klick/Drag auf den Pufferbalken: `offset_s` = Sekunden hinter live.
    /// Wird auf 0..buffered_s geklemmt (der Kern klemmt ebenfalls).
    pub fn timeshift_seek(&mut self, offset_s: f64) -> Result<Effects, AppError> {
        self.timeshift_ready()?;
        if !offset_s.is_finite() {
            return Err(AppError::Other("offset_s invalid".into()));
        }
        let max = self.state.timeshift.buffered_s.max(0.0);
        let offset_s = offset_s.clamp(0.0, max);
        Ok(one(Command::TimeshiftSeek { offset_s }))
    }

    /// Esc / Knopf LIVE: zurueck an den Schreibzeiger.
    pub fn timeshift_live(&mut self) -> Result<Effects, AppError> {
        self.timeshift_ready()?;
        Ok(one(Command::TimeshiftLive))
    }

    /// Kapazitaet setzen (Einstellungs-Panel in Minuten); speichert die
    /// Einstellung und schickt `timeshift_configure`.
    pub fn timeshift_configure(&mut self, capacity_s: u32) -> Result<Effects, AppError> {
        let old = self.settings.clone();
        self.settings.timeshift_capacity_s = clamp_capacity(capacity_s);
        let mut fx = self.timeshift_on_settings(&old);
        fx.append(self.save_settings());
        Ok(fx)
    }

    /// Ausschnitt des Rings als WAV sichern ("letzte n min sichern").
    /// `from_s`/`to_s` sind Sekunden hinter live, `from_s > to_s`. Dateiname
    /// wie bei der Aufnahme (crate::recording), aber mit Suffix `_timeshift`.
    /// Liefert den Zielpfad und die Effekte.
    pub fn timeshift_export(&mut self, from_s: f64, to_s: f64) -> Result<(std::path::PathBuf, Effects), AppError> {
        self.timeshift_ready()?;
        if !from_s.is_finite() || !to_s.is_finite() || from_s <= to_s || to_s < 0.0 {
            return Err(AppError::Other("range invalid".into()));
        }
        let current = self.state.current.clone().ok_or(AppError::NoService)?;
        let name = self
            .state
            .service(current.sid, current.scids)
            .map(|s| s.name.trim().to_string())
            .unwrap_or_else(|| format!("{:04X}", current.sid));
        let dir = self.recording_dir();
        if let Err(e) = std::fs::create_dir_all(&dir) {
            return Err(AppError::Other(format!("{}: {e}", dir.display())));
        }
        let path = dir.join(timeshift_file_name(&chrono::Local::now(), &name, &self.current_title()));
        let fx = one(Command::ExportTimeshiftRange { from_s, to_s, path: path.clone(), format: dab_api::RecFormat::Wav });
        Ok((path, fx))
    }
}

/// Dateiname der Aufnahme mit Suffix `_timeshift` vor der Endung.
pub fn timeshift_file_name(at: &chrono::DateTime<chrono::Local>, service: &str, title: &str) -> String {
    let base = crate::recording::file_name(at, service, title);
    match base.strip_suffix(".wav") {
        Some(stem) => format!("{stem}_timeshift.wav"),
        None => format!("{base}_timeshift"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{DataDirs, Presets, Settings};
    use dab_api::{Codec, ServiceInfo};

    fn app() -> App {
        let nanos = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        let tmp = std::env::temp_dir().join(format!("dabclassic-ts-{}-{nanos}", std::process::id()));
        let mut a = App::with(DataDirs::with_root(&tmp, true), Settings::default(), Presets::default());
        a.state.device = Some(crate::state::DeviceState { kind: "hackrf".into(), ..Default::default() });
        a.state.channel = Some("5C".into());
        a.state.services.push(ServiceInfo { sid: 0xD210, scids: 0, name: "Dlf".into(), is_audio: true, is_primary: true, sub_ch: 1, bitrate_kbps: 104, pty: 0, short_name: String::new(), language: 0 });
        a.state.current = Some(crate::state::CurrentService { sid: 0xD210, scids: 0, codec: None, stereo: true });
        a
    }

    fn ts(mode: TimeshiftMode, buffered_s: f64, offset_s: f64) -> Event {
        Event::TimeshiftState { mode, buffered_s, offset_s, capacity_s: 3600.0, frame_index: 1000, live_unix: 1_789_194_000 }
    }

    #[test]
    fn startup_configures_capacity_from_settings() {
        let mut a = app();
        a.settings.timeshift_capacity_s = 1800;
        let fx = a.timeshift_startup();
        assert_eq!(fx.commands, vec![Command::TimeshiftConfigure { capacity_s: 1800, backing: TimeshiftBacking::Ram }]);
        assert_eq!(a.state.timeshift.capacity_s, 1800.0);
        // Bereich 60..14400 wird geklemmt
        a.settings.timeshift_capacity_s = 5;
        assert_eq!(a.timeshift_startup().commands, vec![Command::TimeshiftConfigure { capacity_s: 60, backing: TimeshiftBacking::Ram }]);
        a.settings.timeshift_capacity_s = 99_999;
        assert_eq!(a.timeshift_startup().commands, vec![Command::TimeshiftConfigure { capacity_s: 14_400, backing: TimeshiftBacking::Ram }]);
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn settings_change_reconfigures_and_clears() {
        let mut a = app();
        a.handle_event(&ts(TimeshiftMode::Paused, 600.0, 120.0), std::time::Instant::now());
        assert_eq!(a.state.timeshift.offset_s, 120.0);
        let mut next = a.settings.clone();
        next.timeshift_capacity_s = 7200;
        let fx = a.update_settings(next);
        assert!(fx.commands.contains(&Command::TimeshiftConfigure { capacity_s: 7200, backing: TimeshiftBacking::Ram }));
        assert!(a.state.timeshift.is_live(), "Kern legt den Ring neu an: Anzeige zurueck auf live");
        assert_eq!(a.state.timeshift.capacity_s, 7200.0);
        // Gleiche Kapazitaet: kein zweites configure
        let same = a.settings.clone();
        assert!(!a.update_settings(same).commands.iter().any(|c| matches!(c, Command::TimeshiftConfigure { .. })));
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn pause_toggle_follows_core_state() {
        let mut a = app();
        let now = std::time::Instant::now();
        assert_eq!(a.timeshift_pause_toggle().unwrap().commands, vec![Command::TimeshiftPause]);
        a.handle_event(&ts(TimeshiftMode::Paused, 300.0, 20.0), now);
        assert_eq!(a.timeshift_pause_toggle().unwrap().commands, vec![Command::TimeshiftPlay]);
        a.handle_event(&ts(TimeshiftMode::Playing, 300.0, 20.0), now);
        assert_eq!(a.timeshift_pause_toggle().unwrap().commands, vec![Command::TimeshiftPause]);
        // Bei laufender Aufnahme bleibt die Bedienung erlaubt (Plan M4 2)
        a.state.recording = true;
        assert_eq!(a.timeshift_pause_toggle().unwrap().commands, vec![Command::TimeshiftPause]);
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn skip_seek_live_commands() {
        let mut a = app();
        a.handle_event(&ts(TimeshiftMode::Playing, 300.0, 60.0), std::time::Instant::now());
        assert_eq!(a.timeshift_skip(-SKIP_STEP_S).unwrap().commands, vec![Command::TimeshiftSkip { delta_s: -30.0 }]);
        assert_eq!(a.timeshift_skip(SKIP_STEP_S).unwrap().commands, vec![Command::TimeshiftSkip { delta_s: 30.0 }]);
        assert!(a.timeshift_skip(0.0).unwrap().commands.is_empty());
        assert_eq!(a.timeshift_seek(45.0).unwrap().commands, vec![Command::TimeshiftSeek { offset_s: 45.0 }]);
        // ueber den Pufferinhalt hinaus wird geklemmt
        assert_eq!(a.timeshift_seek(9999.0).unwrap().commands, vec![Command::TimeshiftSeek { offset_s: 300.0 }]);
        assert_eq!(a.timeshift_seek(-5.0).unwrap().commands, vec![Command::TimeshiftSeek { offset_s: 0.0 }]);
        assert_eq!(a.timeshift_live().unwrap().commands, vec![Command::TimeshiftLive]);
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn guards_no_service_and_file_source() {
        let mut a = app();
        a.state.current = None;
        assert_eq!(a.timeshift_pause_toggle().unwrap_err(), AppError::NoService);
        a.state.current = Some(crate::state::CurrentService { sid: 0xD210, scids: 0, codec: None, stereo: true });
        a.state.device = Some(crate::state::DeviceState { kind: "file".into(), ..Default::default() });
        assert_eq!(a.timeshift_live().unwrap_err(), AppError::Other("timeshift unavailable".into()));
        assert!(a.timeshift_export(60.0, 0.0).is_err());
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn reset_on_service_change_and_channel_change() {
        let mut a = app();
        let now = std::time::Instant::now();
        a.handle_event(&ts(TimeshiftMode::Paused, 600.0, 90.0), now);
        assert_eq!(a.state.timeshift.offset_s, 90.0);
        let fx = a.handle_event(
            &Event::ServiceStarted {
                slot: ServiceSlot::Primary,
                sid: 0xD220,
                scids: 0,
                codec: Codec::HeAac { sbr: true, ps: false, sample_rate: 48000 },
                stereo: true,
            },
            now,
        );
        assert!(a.state.timeshift.is_live());
        assert_eq!(a.state.timeshift.buffered_s, 0.0);
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::TimeshiftNotice { notice: TimeshiftNotice::ServiceChanged })));
        // Ohne Versatz kein Hinweis
        let fx = a.handle_event(&Event::ServiceStopped { slot: ServiceSlot::Primary, sid: 0xD220 }, now);
        assert!(!fx.events.iter().any(|e| matches!(e, AppEvent::TimeshiftNotice { .. })));
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn ews_switch_drops_buffer_with_notice() {
        let mut a = app();
        let now = std::time::Instant::now();
        a.handle_event(&ts(TimeshiftMode::Playing, 900.0, 240.0), now);
        let fx = a.handle_event(&Event::EwsSwitched { to_sid: 0xD3F0, from_sid: Some(0xD210) }, now);
        assert!(a.state.timeshift.is_live());
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::TimeshiftNotice { notice: TimeshiftNotice::EwsDropped })));
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn state_from_event_and_snapshot() {
        let mut a = app();
        let now = std::time::Instant::now();
        a.handle_event(&ts(TimeshiftMode::Playing, 1200.0, 33.0), now);
        let info = *a.timeshift_info();
        assert_eq!((info.mode, info.buffered_s, info.offset_s, info.capacity_s), (TimeshiftMode::Playing, 1200.0, 33.0, 3600.0));
        assert_eq!(info.live_unix, 1_789_194_000);
        let mut cs = dab_api::CoreState::default();
        cs.timeshift = Some((TimeshiftMode::Paused, 60.0, 12.0, 1800.0));
        a.handle_event(&Event::StateSnapshot { state: cs.clone() }, now);
        assert_eq!(a.state.timeshift.mode, TimeshiftMode::Paused);
        assert_eq!(a.state.timeshift.offset_s, 12.0);
        assert_eq!(a.state.timeshift.capacity_s, 1800.0);
        // Kern ohne Timeshift: live
        cs.timeshift = None;
        a.handle_event(&Event::StateSnapshot { state: cs }, now);
        assert!(a.state.timeshift.is_live());
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn export_uses_recording_name_with_suffix() {
        use chrono::TimeZone;
        let at = chrono::Local.with_ymd_and_hms(2026, 9, 12, 12, 11, 1).unwrap();
        assert_eq!(
            timeshift_file_name(&at, "Dlf", "Informationen am Mittag"),
            "20260912_121101_Dlf______________Informationen_am_Mittag_timeshift.wav"
        );
        let mut a = app();
        let (path, fx) = a.timeshift_export(600.0, 0.0).unwrap();
        assert!(path.to_string_lossy().ends_with("_timeshift.wav"), "{}", path.display());
        assert_eq!(
            fx.commands,
            vec![Command::ExportTimeshiftRange { from_s: 600.0, to_s: 0.0, path, format: dab_api::RecFormat::Wav }]
        );
        assert!(a.timeshift_export(0.0, 600.0).is_err(), "from_s muss groesser als to_s sein");
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }
}
