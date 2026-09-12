//! Anwendungslogik (Entscheidung 14): Preset-Aufruf als Zustandsmaschine,
//! Gain je Geraet und Kanal (Entscheidung 26), Wiederherstellung beim Start,
//! Umschaltsperre bei Aufnahme. Ohne Fenster und ohne Kernprozess testbar:
//! jede Aktion liefert [`Effects`] (Kommandos an den Kern, Hinweise an das
//! Frontend), die der Aufrufer ausfuehrt.

use crate::favorites;
use crate::state::{AppState, PendingState};
use crate::{DataDirs, Preset, Presets, Settings, PRESET_SLOTS};
use dab_api::{Command, Event, Gain, ScanMode, ServiceInfo, ServiceSlot, SourceKind};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

/// Wartezeit auf `service_added(sid)` nach einem Kanalwechsel (Analyse 5.1).
pub const PRESET_TIMEOUT: Duration = Duration::from_secs(8);
/// Ohne gespeicherten Wert: Startwerte fuer HackRF (Befund M1: 11D braucht VGA 40).
pub const DEFAULT_HACKRF_GAIN: Gain = Gain { lna: 40, vga: 40, amp: false };

/// Hinweise der App-Schicht an das Frontend (Tauri-Event `dab://app`).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum AppEvent {
    /// Fortschritt eines Preset-Aufrufs bzw. der Start-Wiederherstellung.
    PresetStatus { slot: Option<usize>, status: PresetStatus, name: String, channel: String },
    PresetsChanged { presets: Presets },
    SettingsChanged { settings: Settings },
    CoreRestarted { reason: String, attempt: u32 },
    Notice { level: NoticeLevel, text: String },
    /// EPG/Logos (crate::epg, crate::logos): neue Sendeplan-Datei, neues Logo,
    /// Logo + Now/Next des aktuellen Dienstes geaendert.
    EpgUpdated { eid: u16, sid: u32, day: u32 },
    LogoUpdated { eid: u16, sid: u32 },
    CurrentMedia { logo_data_url: Option<String>, now_next: Option<crate::epg::NowNext> },
    /// Timer/Aufnahme/Sleep (crate::timer, crate::recording, crate::sleep).
    TimersChanged { timers: crate::timer::Timers },
    TimerStatus { id: u32, kind: crate::timer::TimerKind, service: String, title: String, status: crate::timer::TimerFireStatus },
    RecordingChanged { recording: crate::recording::RecordingInfo },
    SleepChanged { sleep: Option<crate::sleep::SleepState> },
    SleepElapsed { action: crate::sleep::SleepAction },
    /// TII / Debug-Panel (crate::tii): Senderliste geaendert, Zaehler (1 Hz bei offenem Panel).
    TiiUpdated { tii: Vec<crate::tii::TiiSeen> },
    DebugStats { debug: crate::tii::DebugState },
    /// Senderliste ueber alle Ensembles geaendert (crate::stations).
    StationsChanged { stations: Vec<crate::stations::StationEntry> },
    /// Timeshift (crate::timeshift): Puffer verworfen (Alarm, Dienstwechsel).
    TimeshiftNotice { notice: crate::timeshift::TimeshiftNotice },
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum PresetStatus {
    Tuning,
    Selected,
    NotFound,
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum NoticeLevel {
    Info,
    Warn,
    Error,
}

/// Ergebnis einer Aktion: an den Kern zu sendende Kommandos und Hinweise.
#[derive(Debug, Default, PartialEq)]
pub struct Effects {
    pub commands: Vec<Command>,
    pub events: Vec<AppEvent>,
}

impl Effects {
    fn cmd(mut self, c: Command) -> Self {
        self.commands.push(c);
        self
    }
    fn ev(mut self, e: AppEvent) -> Self {
        self.events.push(e);
        self
    }
    pub fn append(&mut self, mut other: Effects) {
        self.commands.append(&mut other.commands);
        self.events.append(&mut other.events);
    }
}

/// Fehler einer Aktion (als Text an das Frontend).
#[derive(Debug, thiserror::Error, PartialEq)]
pub enum AppError {
    #[error("slot out of range")]
    Slot,
    #[error("preset empty")]
    Empty,
    #[error("no service selected")]
    NoService,
    #[error("recording active")]
    Recording,
    #[error("scan active")]
    Scanning,
    #[error("{0}")]
    Other(String),
}

/// Ergebnis von [`App::preset_store`].
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct StoreResult {
    pub stored: bool,
    pub previous: Option<Preset>,
}

#[derive(Clone, Debug)]
struct Pending {
    slot: Option<usize>,
    channel: String,
    sid: u32,
    scids: u8,
    name: String,
    deadline: Instant,
}

/// Die App-Logik. Haelt Zustand, Einstellungen und Presets; persistiert
/// selbst in den Datenordner.
pub struct App {
    pub dirs: DataDirs,
    pub state: AppState,
    pub settings: Settings,
    pub presets: Presets,
    /// Logo- und EPG-Cache (crate::logos, crate::epg).
    pub logos: crate::logos::LogoCache,
    pub epg: crate::epg::EpgCache,
    /// Timer-Scheduler, Aufnahme, Sleep-Timer (crate::timer, crate::recording, crate::sleep).
    pub sched: crate::timer::Scheduler,
    pub rec: crate::recording::Recording,
    pub sleep: crate::sleep::Sleep,
    /// TII-Datenbank und Debug-Zeitgeber (crate::tii).
    pub tii: crate::tii::TiiCtl,
    /// Senderliste ueber alle Ensembles (crate::stations).
    pub stations_ctl: crate::stations::StationsCtl,
    pending: Option<Pending>,
}

impl App {
    pub fn new(dirs: DataDirs) -> Self {
        let settings = Settings::load(&dirs.settings_file());
        let presets = Presets::load(&dirs.presets_file());
        Self::with(dirs, settings, presets)
    }

    pub fn with(dirs: DataDirs, settings: Settings, presets: Presets) -> Self {
        let mut state = AppState::default();
        state.volume = settings.volume_percent;
        state.agc = settings.agc;
        let (logos, epg) = crate::epg::open_caches(&dirs);
        let sched = crate::timer::Scheduler::load(&dirs, settings.record_pre_s as i64, settings.record_post_s as i64);
        let mut app = Self {
            dirs,
            state,
            settings,
            presets,
            logos,
            epg,
            sched,
            rec: Default::default(),
            sleep: Default::default(),
            tii: Default::default(),
            stations_ctl: Default::default(),
            pending: None,
        };
        app.stations_load();
        app.timeshift_init();
        app
    }

    /// Laufenden Preset-Aufruf verwerfen (Timer-Scheduler uebernimmt die Dienstwahl).
    pub(crate) fn pending_clear(&mut self) {
        self.pending = None;
        self.state.pending = None;
    }

    // -----------------------------------------------------------------------
    // Start / Ende
    // -----------------------------------------------------------------------

    /// Kommandos nach `ready`: Startwerte, Geraet, letzter Kanal/Dienst.
    pub fn startup(&mut self, now: Instant) -> Effects {
        let s = self.settings.clone();
        let mut fx = Effects::default()
            .cmd(Command::SetVolume { percent: s.volume_percent })
            .cmd(Command::SetAgc { enabled: s.agc })
            .cmd(Command::SetEws { enabled: s.ews_enabled, autoswitch: s.ews_autoswitch })
            .cmd(Command::SetEpg { enabled: s.epg_enabled });
        fx.append(self.debug_startup());
        fx.append(self.timeshift_startup());
        if s.ppm != 0 {
            fx = fx.cmd(Command::SetPpm { ppm: s.ppm });
        }
        if let Some(idx) = s.audio_device {
            fx = fx.cmd(Command::SetAudioDevice { index: Some(idx) });
        }
        if !s.autostart {
            return fx;
        }
        let source = match s.device.as_str() {
            "file" => s.last_file.as_ref().map(|p| SourceKind::File { path: p.clone(), r#loop: s.file_loop, fast: false }),
            "rtlsdr" => Some(SourceKind::RtlSdr { index: s.rtlsdr_index }),
            _ => Some(SourceKind::HackRf { serial: None }),
        };
        let Some(source) = source else { return fx };
        let is_file = matches!(source, SourceKind::File { .. });
        fx.append(self.open_device(source));
        if let Some(ch) = s.last_channel.clone() {
            if !is_file {
                fx.append(self.set_channel(&ch));
            }
            if let Some((sid, scids)) = s.last_service {
                // Wie ein Preset-Aufruf ohne Slot: warten auf service_added.
                self.pending = Some(Pending { slot: None, channel: ch.clone(), sid, scids, name: String::new(), deadline: now + PRESET_TIMEOUT * 2 });
                self.state.pending = Some(PendingState { slot: None, channel: ch.clone(), name: String::new() });
                fx = fx.ev(AppEvent::PresetStatus { slot: None, status: PresetStatus::Tuning, name: String::new(), channel: ch });
            }
        }
        fx
    }

    /// Letzten Kanal/Dienst/Geraet/Lautstaerke merken und alles speichern.
    pub fn save_all(&mut self) -> std::io::Result<()> {
        self.remember_last();
        if self.stations_ctl.unsaved {
            self.stations_save();
        }
        self.settings.save(&self.dirs.settings_file())?;
        self.presets.save(&self.dirs.presets_file())
    }

    fn remember_last(&mut self) {
        // Waehrend eines Scans ist state.channel der Scan-Kanal (zuletzt 13F) –
        // der darf nicht als "letzter Kanal" fuer den naechsten Start gelten.
        if self.state.scan.active {
            return;
        }
        if let Some(ch) = &self.state.channel {
            self.settings.last_channel = Some(ch.clone());
        }
        if let Some(c) = &self.state.current {
            self.settings.last_service = Some((c.sid, c.scids));
        }
        if let Some(kind) = self.state.device_kind() {
            self.settings.device = kind.to_string();
        }
        self.settings.volume_percent = self.state.volume;
    }

    pub(crate) fn save_settings(&self) -> Effects {
        if let Err(e) = self.settings.save(&self.dirs.settings_file()) {
            log::warn!("settings.json: {e}");
        }
        Effects::default().ev(AppEvent::SettingsChanged { settings: self.settings.clone() })
    }

    pub(crate) fn save_presets(&self) -> Effects {
        if let Err(e) = self.presets.save(&self.dirs.presets_file()) {
            log::warn!("presets.json: {e}");
        }
        Effects::default().ev(AppEvent::PresetsChanged { presets: self.presets.clone() })
    }

    // -----------------------------------------------------------------------
    // Ereignisse vom Kern
    // -----------------------------------------------------------------------

    /// Wendet ein Kern-Ereignis an und fuehrt die Preset-Zustandsmaschine weiter.
    pub fn handle_event(&mut self, ev: &Event, now: Instant) -> Effects {
        self.state.apply(ev);
        let mut fx = self.media_on_event(ev);
        match ev {
            Event::GainChanged { lna, vga, amp, agc } => {
                // Entscheidung 26: Gain-Merker je Geraet/Kanal nur bei AGC AUS
                // (manuelle Werte). Bei AGC an findet der Kern den Wert selbst
                // (Akquisitions-Ramp + Nachfuehrung); ein gemerkter AGC-Zwischenstand
                // (Befund 12.09.: 11D mit VGA 58 = 5 dB statt 7 dB) wuerde sonst
                // beim naechsten Wechsel als Startpunkt schaden.
                if let (Some(dev), Some(ch)) = (self.state.device_kind(), self.state.channel.clone()) {
                    if dev != "file" && !self.state.scan.active && !*agc {
                        self.settings.set_gain_for(dev, &ch, Gain { lna: *lna, vga: *vga, amp: *amp });
                    }
                }
            }
            Event::ServiceAdded { service } => {
                if let Some(p) = self.pending.clone() {
                    if self.pending_matches(&p, service) {
                        fx.append(self.pending_found(p, service.clone()));
                    }
                }
            }
            Event::NoSignal { channel } => {
                if let Some(p) = self.pending.clone() {
                    if p.channel.eq_ignore_ascii_case(channel) && !self.state.is_file_source() {
                        fx.append(self.pending_failed(p));
                    }
                }
            }
            Event::ServiceStarted { slot: ServiceSlot::Primary, .. } => {
                self.remember_last();
            }
            Event::DeviceError { message } => {
                self.pending = None;
                self.state.pending = None;
                fx = fx.ev(AppEvent::Notice { level: NoticeLevel::Error, text: message.clone() });
            }
            Event::Exiting { .. } => {
                self.pending = None;
                self.state.pending = None;
            }
            _ => {}
        }
        fx.append(self.recording_on_event(ev));
        fx.append(self.sched_on_event(ev));
        fx.append(self.debug_on_event(ev, now));
        fx.append(self.stations_on_event(ev, now));
        fx.append(self.timeshift_on_event(ev));
        fx.append(self.tick(now));
        fx
    }

    /// Zeitgeber: Timeout des laufenden Preset-Aufrufs.
    pub fn tick(&mut self, now: Instant) -> Effects {
        let mut fx = Effects::default();
        if let Some(p) = self.pending.clone() {
            if now >= p.deadline {
                fx = self.pending_failed(p);
            }
        }
        fx.append(self.media_tick(now));
        fx.append(self.timer_tick_all(crate::state::unix_now()));
        fx.append(self.debug_tick(now));
        fx.append(self.stations_tick(now));
        fx
    }

    fn pending_matches(&self, p: &Pending, s: &ServiceInfo) -> bool {
        if p.sid != 0 {
            return s.sid == p.sid && s.scids == p.scids;
        }
        // Importierter Favorit ohne SId: ueber den Namen aufloesen.
        s.name.trim().eq_ignore_ascii_case(p.name.trim())
    }

    fn pending_found(&mut self, p: Pending, s: ServiceInfo) -> Effects {
        self.pending = None;
        self.state.pending = None;
        let mut fx = Effects::default().cmd(Command::SelectService { sid: s.sid, scids: s.scids, slot: ServiceSlot::Primary });
        // Favoriten-Import: SId/EId nachtragen, Namen aktualisieren.
        if let Some(slot) = p.slot {
            if let Some(preset) = self.presets.slots.get_mut(slot).and_then(|x| x.as_mut()) {
                let eid = self.state.ensemble.as_ref().map(|e| e.eid).unwrap_or(preset.eid);
                if preset.sid != s.sid || preset.eid != eid || preset.name != s.name.trim() {
                    preset.sid = s.sid;
                    preset.scids = s.scids;
                    preset.eid = eid;
                    preset.name = s.name.trim().to_string();
                    fx.append(self.save_presets());
                }
            }
        }
        fx.ev(AppEvent::PresetStatus { slot: p.slot, status: PresetStatus::Selected, name: s.name.trim().to_string(), channel: p.channel })
    }

    fn pending_failed(&mut self, p: Pending) -> Effects {
        self.pending = None;
        self.state.pending = None;
        Effects::default().ev(AppEvent::PresetStatus { slot: p.slot, status: PresetStatus::NotFound, name: p.name, channel: p.channel })
    }

    pub fn is_pending(&self) -> bool {
        self.pending.is_some()
    }

    // -----------------------------------------------------------------------
    // Aktionen
    // -----------------------------------------------------------------------

    /// Generischer Weg: jedes Kommando laeuft hier durch, damit der Zustand
    /// (Kanal, Geraet, Lautstaerke) mitgefuehrt wird.
    pub fn command(&mut self, cmd: Command) -> Result<Effects, AppError> {
        Ok(match cmd {
            Command::OpenDevice { source } => self.open_device(source),
            Command::SetChannel { channel } => {
                if self.state.recording {
                    return Err(AppError::Recording);
                }
                self.set_channel(&channel)
            }
            Command::SelectService { sid, scids, slot: ServiceSlot::Primary } => self.select_service(sid, scids)?,
            Command::StopService { slot: ServiceSlot::Primary, .. } if self.state.recording => return Err(AppError::Recording),
            Command::StartRecording { slot: ServiceSlot::Primary, .. } => self.recording_start(None, None)?,
            Command::StopRecording { slot: ServiceSlot::Primary, .. } => self.recording_stop()?,
            Command::SetVolume { percent } => self.set_volume(percent),
            Command::SetMute { muted } => self.set_mute(muted),
            Command::SetAgc { enabled } => {
                self.settings.agc = enabled;
                self.state.agc = enabled;
                let mut fx = Effects::default().cmd(Command::SetAgc { enabled });
                fx.append(self.save_settings());
                fx
            }
            Command::StartScan { channels, mode } => self.start_scan(channels, mode)?,
            Command::EwsDismiss => {
                if let Some(a) = self.state.alert.as_mut() {
                    a.dismissed = true;
                }
                Effects::default().cmd(Command::EwsDismiss)
            }
            Command::CloseDevice => {
                self.pending = None;
                self.state.pending = None;
                Effects::default().cmd(Command::CloseDevice)
            }
            other => Effects::default().cmd(other),
        })
    }

    pub fn open_device(&mut self, source: SourceKind) -> Effects {
        self.pending = None;
        self.state.pending = None;
        self.state.clear_reception();
        self.state.note_source(&source);
        match &source {
            SourceKind::File { path, r#loop, .. } => {
                self.settings.device = "file".into();
                self.settings.last_file = Some(path.clone());
                self.settings.file_loop = *r#loop;
            }
            SourceKind::RtlSdr { index } => {
                self.settings.device = "rtlsdr".into();
                self.settings.rtlsdr_index = *index;
            }
            SourceKind::HackRf { .. } => self.settings.device = "hackrf".into(),
        }
        Effects::default().cmd(Command::OpenDevice { source })
    }

    /// Kanalwechsel: bei AGC aus vorher den gespeicherten Gain (oder den
    /// Standard) senden; bei AGC an regelt der Kern selbst.
    pub fn set_channel(&mut self, channel: &str) -> Effects {
        let channel = channel.trim().to_uppercase();
        let mut fx = Effects::default();
        if !self.state.is_file_source() && !self.settings.agc {
            if let Some(g) = self.gain_for_channel(&channel) {
                fx = fx.cmd(Command::SetGain { gain: g });
            }
        }
        if self.state.channel.as_deref() != Some(channel.as_str()) || !self.state.is_file_source() {
            self.state.clear_reception();
        }
        self.state.channel = Some(channel.clone());
        self.settings.last_channel = Some(channel.clone());
        // Gain-Tabelle und letzten Kanal gleich sichern (nicht erst beim Beenden).
        if self.dirs.root.is_dir() {
            if let Err(e) = self.settings.save(&self.dirs.settings_file()) {
                log::warn!("settings.json: {e}");
            }
        }
        fx.cmd(Command::SetChannel { channel })
    }

    fn gain_for_channel(&self, channel: &str) -> Option<Gain> {
        let dev = self.state.device_kind()?;
        if let Some(g) = self.settings.gain_for(dev, channel) {
            return Some(g);
        }
        (dev == "hackrf").then_some(DEFAULT_HACKRF_GAIN)
    }

    pub fn select_service(&mut self, sid: u32, scids: u8) -> Result<Effects, AppError> {
        if self.state.recording {
            return Err(AppError::Recording);
        }
        if self.state.scan.active {
            return Err(AppError::Scanning);
        }
        self.pending = None;
        self.state.pending = None;
        Ok(Effects::default().cmd(Command::SelectService { sid, scids, slot: ServiceSlot::Primary }))
    }

    /// Naechster/vorheriger hoerbarer Dienst der Liste (mit Umbruch).
    pub fn step_service(&mut self, delta: i32) -> Result<Effects, AppError> {
        let list: Vec<(u32, u8)> = self.state.audio_services().map(|s| (s.sid, s.scids)).collect();
        if list.is_empty() {
            return Err(AppError::NoService);
        }
        let idx = self
            .state
            .current
            .as_ref()
            .and_then(|c| list.iter().position(|(sid, scids)| *sid == c.sid && *scids == c.scids));
        let next = match idx {
            Some(i) => (i as i32 + delta).rem_euclid(list.len() as i32) as usize,
            None => 0,
        };
        let (sid, scids) = list[next];
        self.select_service(sid, scids)
    }

    pub fn set_volume(&mut self, percent: u8) -> Effects {
        let percent = percent.min(100);
        self.state.volume = percent;
        self.settings.volume_percent = percent;
        Effects::default().cmd(Command::SetVolume { percent })
    }

    pub fn set_mute(&mut self, muted: bool) -> Effects {
        self.state.muted = muted;
        Effects::default().cmd(Command::SetMute { muted })
    }

    pub fn start_scan(&mut self, channels: Vec<String>, mode: ScanMode) -> Result<Effects, AppError> {
        if self.state.recording {
            return Err(AppError::Recording);
        }
        if self.state.is_file_source() || self.state.device.is_none() {
            return Err(AppError::Other("scan needs a device".into()));
        }
        self.pending = None;
        self.state.pending = None;
        self.state.scan.results.clear();
        self.state.scan.active = true;
        self.state.scan.index = 0;
        self.state.scan.total = if channels.is_empty() { dab_api::BAND_III.len() as u16 } else { channels.len() as u16 };
        self.state.clear_reception();
        Ok(Effects::default().cmd(Command::StartScan { channels, mode }))
    }

    /// Einstellungen ersetzen; Unterschiede mit Kern-Wirkung werden gesendet.
    pub fn update_settings(&mut self, new: Settings) -> Effects {
        let old = std::mem::replace(&mut self.settings, new);
        let s = self.settings.clone();
        let mut fx = Effects::default();
        if old.volume_percent != s.volume_percent {
            self.state.volume = s.volume_percent;
            fx = fx.cmd(Command::SetVolume { percent: s.volume_percent });
        }
        if old.agc != s.agc {
            self.state.agc = s.agc;
            fx = fx.cmd(Command::SetAgc { enabled: s.agc });
        }
        if old.ews_enabled != s.ews_enabled || old.ews_autoswitch != s.ews_autoswitch {
            fx = fx.cmd(Command::SetEws { enabled: s.ews_enabled, autoswitch: s.ews_autoswitch });
        }
        if old.epg_enabled != s.epg_enabled {
            fx = fx.cmd(Command::SetEpg { enabled: s.epg_enabled });
        }
        if old.ppm != s.ppm {
            fx = fx.cmd(Command::SetPpm { ppm: s.ppm });
        }
        if old.audio_device != s.audio_device {
            fx = fx.cmd(Command::SetAudioDevice { index: s.audio_device });
        }
        fx.append(self.debug_on_settings(&old));
        fx.append(self.timeshift_on_settings(&old));
        fx.append(self.save_settings());
        fx
    }

    /// Manueller Gain: an den Kern und (per `gain_changed`) in die Tabelle.
    pub fn set_gain(&mut self, gain: Gain) -> Effects {
        Effects::default().cmd(Command::SetGain { gain })
    }

    // -----------------------------------------------------------------------
    // Presets (Entscheidung 8, Analyse 5.1)
    // -----------------------------------------------------------------------

    pub fn preset_recall(&mut self, slot: usize, now: Instant) -> Result<Effects, AppError> {
        let preset = self.presets.get(slot).cloned().ok_or(if slot < PRESET_SLOTS { AppError::Empty } else { AppError::Slot })?;
        self.tune_to(Some(slot), &preset.channel, preset.sid, preset.scids, &preset.name, now)
    }

    /// Gemeinsame Zustandsmaschine fuer Preset-Aufruf (`slot`), Senderliste
    /// ([`tune_station`](Self::tune_station)) und Aehnliches: gleicher Kanal
    /// und Dienst bekannt -> sofort `select_service`; sonst Kanal abstimmen,
    /// auf `service_added` warten (Timeout [`PRESET_TIMEOUT`]). `sid` 0 =
    /// ueber den Namen aufloesen (Favoriten-Import). Fortschritt kommt als
    /// [`AppEvent::PresetStatus`].
    pub(crate) fn tune_to(&mut self, slot: Option<usize>, channel: &str, sid: u32, scids: u8, name: &str, now: Instant) -> Result<Effects, AppError> {
        if self.state.recording {
            return Err(AppError::Recording);
        }
        if self.state.scan.active {
            return Err(AppError::Scanning);
        }
        let channel = channel.trim().to_uppercase();
        let name = name.trim().to_string();
        let same_channel = self.state.channel.as_deref().map(|c| c.eq_ignore_ascii_case(&channel)).unwrap_or(false);
        let found = if sid != 0 { self.state.service(sid, scids).cloned() } else { self.state.service_by_name(&name).cloned() };
        let mut fx = Effects::default();
        if same_channel || self.state.is_file_source() {
            if let Some(s) = found {
                // Gleicher Kanal, Dienst bekannt: direkt umschalten (< 1 s).
                let p = Pending { slot, channel: channel.clone(), sid: s.sid, scids: s.scids, name: name.clone(), deadline: now };
                fx.append(self.pending_found(p, s));
                return Ok(fx);
            }
            if self.state.is_file_source() {
                return Ok(fx.ev(AppEvent::PresetStatus { slot, status: PresetStatus::NotFound, name, channel }));
            }
            // Gleicher Kanal, Dienst (noch) nicht in der Liste: nur warten.
        } else {
            fx.append(self.set_channel(&channel));
        }
        self.pending = Some(Pending { slot, channel: channel.clone(), sid, scids, name: name.clone(), deadline: now + PRESET_TIMEOUT });
        self.state.pending = Some(PendingState { slot, channel: channel.clone(), name: name.clone() });
        Ok(fx.ev(AppEvent::PresetStatus { slot, status: PresetStatus::Tuning, name, channel }))
    }

    /// Belegt den Slot mit dem aktuellen Dienst. Ist der Slot belegt und
    /// `force` falsch, passiert nichts und `previous` liefert den Inhalt
    /// fuer die Nachfrage.
    pub fn preset_store(&mut self, slot: usize, force: bool) -> Result<(StoreResult, Effects), AppError> {
        self.preset_store_service(slot, None, force)
    }

    /// Wie [`preset_store`](Self::preset_store), aber mit explizitem Dienst
    /// (Drag aus der Senderliste); `None` = aktueller Dienst.
    pub fn preset_store_service(&mut self, slot: usize, service: Option<(u32, u8)>, force: bool) -> Result<(StoreResult, Effects), AppError> {
        if slot >= PRESET_SLOTS {
            return Err(AppError::Slot);
        }
        let svc = match service {
            Some((sid, scids)) => self.state.service(sid, scids),
            None => self.state.current_service(),
        }
        .cloned()
        .ok_or(AppError::NoService)?;
        let (channel, eid) = match (&self.state.channel, &self.state.ensemble) {
            (Some(ch), Some(e)) => (ch.clone(), e.eid),
            (_, Some(e)) => (e.channel.clone(), e.eid),
            _ => return Err(AppError::NoService),
        };
        if self.presets.is_occupied(slot) && !force {
            return Ok((StoreResult { stored: false, previous: self.presets.get(slot).cloned() }, Effects::default()));
        }
        let mut preset = Preset {
            channel,
            eid,
            sid: svc.sid,
            scids: svc.scids,
            name: svc.name.trim().to_string(),
            logo_path: None,
            logo_data_url: None,
            stored_at: crate::state::unix_now(),
        };
        self.decorate_preset(&mut preset);
        let previous = self.presets.set(slot, preset);
        let fx = self.save_presets();
        Ok((StoreResult { stored: true, previous }, fx))
    }

    pub fn preset_clear(&mut self, slot: usize) -> Result<(Option<Preset>, Effects), AppError> {
        if slot >= PRESET_SLOTS {
            return Err(AppError::Slot);
        }
        let prev = self.presets.clear(slot);
        let fx = self.save_presets();
        Ok((prev, fx))
    }

    /// Slot des aktuell gehoerten Dienstes (Hervorhebung in der Leiste).
    pub fn active_preset_slot(&self) -> Option<usize> {
        let c = self.state.current.as_ref()?;
        let ch = self.state.channel.as_deref()?;
        self.presets.slots.iter().position(|p| {
            p.as_ref()
                .map(|p| p.sid == c.sid && p.scids == c.scids && p.channel.eq_ignore_ascii_case(ch))
                .unwrap_or(false)
        })
    }

    /// Favoriten aus Qt-DAB (`.qt-dab-presets.xml`) in freie Slots uebernehmen.
    /// `path` = None: an den bekannten Orten suchen. Liefert die Zahl der
    /// uebernommenen Eintraege.
    pub fn presets_import_favorites(&mut self, path: Option<&Path>) -> Result<(usize, Effects), AppError> {
        let path = match path {
            Some(p) => p.to_path_buf(),
            None => favorites::locate().ok_or_else(|| AppError::Other("favorites file not found".into()))?,
        };
        let xml = std::fs::read_to_string(&path).map_err(|e| AppError::Other(format!("{}: {e}", path.display())))?;
        let n = self.import_favorites_xml(&xml);
        let fx = self.save_presets();
        Ok((n, fx))
    }

    pub fn import_favorites_xml(&mut self, xml: &str) -> usize {
        let mut n = 0;
        for fav in favorites::parse(xml) {
            let dup = self.presets.slots.iter().flatten().any(|p| p.channel.eq_ignore_ascii_case(&fav.channel) && p.name.eq_ignore_ascii_case(&fav.name));
            if dup {
                continue;
            }
            let Some(slot) = self.presets.first_free() else { break };
            self.presets.set(
                slot,
                Preset { channel: fav.channel, eid: 0, sid: 0, scids: 0, name: fav.name, logo_path: None, logo_data_url: None, stored_at: crate::state::unix_now() },
            );
            n += 1;
        }
        n
    }

    pub fn favorites_path(&self) -> Option<PathBuf> {
        favorites::locate()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use dab_api::{Codec, EwsPhase};

    fn app() -> App {
        let tmp = std::env::temp_dir().join(format!("dabclassic-app-{}-{}", std::process::id(), unix_nanos()));
        let mut a = App::with(DataDirs::with_root(&tmp, true), Settings::default(), Presets::default());
        a.state.device = Some(crate::state::DeviceState { kind: "hackrf".into(), ..Default::default() });
        a
    }

    fn unix_nanos() -> u128 {
        std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()
    }

    fn svc(sid: u32, name: &str) -> ServiceInfo {
        ServiceInfo { sid, scids: 0, name: name.into(), is_audio: true, is_primary: true, sub_ch: 1, bitrate_kbps: 96, pty: 0 }
    }

    fn tune(a: &mut App, channel: &str, eid: u16, services: &[(u32, &str)], now: Instant) {
        a.handle_event(&Event::EnsembleFound { eid, name: "Ens".into(), channel: channel.into() }, now);
        for (sid, name) in services {
            a.handle_event(&Event::ServiceAdded { service: svc(*sid, name) }, now);
        }
    }

    fn started(a: &mut App, sid: u32, now: Instant) {
        a.handle_event(
            &Event::ServiceStarted { slot: ServiceSlot::Primary, sid, scids: 0, codec: Codec::HeAac { sbr: true, ps: false, sample_rate: 48000 }, stereo: true },
            now,
        );
    }

    fn preset(channel: &str, sid: u32, name: &str) -> Preset {
        Preset { channel: channel.into(), eid: 0x10BC, sid, scids: 0, name: name.into(), logo_path: None, logo_data_url: None, stored_at: 0 }
    }

    #[test]
    fn recall_same_channel_selects_immediately() {
        let now = Instant::now();
        let mut a = app();
        a.state.channel = Some("5C".into());
        tune(&mut a, "5C", 0x10BC, &[(0xD210, "Dlf"), (0xD220, "Dlf Kultur")], now);
        a.presets.set(0, preset("5C", 0xD220, "Dlf Kultur"));
        let fx = a.preset_recall(0, now).unwrap();
        assert_eq!(fx.commands, vec![Command::SelectService { sid: 0xD220, scids: 0, slot: ServiceSlot::Primary }]);
        assert!(matches!(fx.events[0], AppEvent::PresetStatus { status: PresetStatus::Selected, .. }));
        assert!(!a.is_pending());
    }

    #[test]
    fn recall_other_channel_waits_for_service_added() {
        let now = Instant::now();
        let mut a = app();
        a.state.channel = Some("5C".into());
        tune(&mut a, "5C", 0x10BC, &[(0xD210, "Dlf")], now);
        a.presets.set(1, preset("11D", 0xE1C0, "WDR 5"));
        let fx = a.preset_recall(1, now).unwrap();
        // AGC an (Standard): kein Gain vorgeben, nur Kanalwechsel, Status "tuning"
        assert_eq!(fx.commands, vec![Command::SetChannel { channel: "11D".into() }]);
        assert!(matches!(fx.events[0], AppEvent::PresetStatus { status: PresetStatus::Tuning, slot: Some(1), .. }));
        assert!(a.is_pending());
        assert!(a.state.services.is_empty(), "Senderliste beim Kanalwechsel geleert");
        // Fremder Dienst: nichts
        let fx = a.handle_event(&Event::ServiceAdded { service: svc(0xE1C1, "1LIVE") }, now + Duration::from_secs(1));
        assert!(fx.commands.is_empty());
        // Gesuchter Dienst: select_service
        let fx = a.handle_event(&Event::ServiceAdded { service: svc(0xE1C0, "WDR 5") }, now + Duration::from_secs(2));
        assert_eq!(fx.commands, vec![Command::SelectService { sid: 0xE1C0, scids: 0, slot: ServiceSlot::Primary }]);
        assert!(matches!(fx.events[0], AppEvent::PresetStatus { status: PresetStatus::Selected, slot: Some(1), .. }));
        assert!(!a.is_pending());
    }

    #[test]
    fn recall_timeout_keeps_preset() {
        let now = Instant::now();
        let mut a = app();
        a.state.channel = Some("5C".into());
        a.presets.set(2, preset("11D", 0xE1C0, "WDR 5"));
        a.preset_recall(2, now).unwrap();
        let fx = a.tick(now + Duration::from_secs(7));
        assert!(fx.events.is_empty());
        let fx = a.tick(now + PRESET_TIMEOUT + Duration::from_millis(1));
        assert!(matches!(fx.events[0], AppEvent::PresetStatus { status: PresetStatus::NotFound, slot: Some(2), .. }));
        assert!(!a.is_pending());
        assert!(a.presets.is_occupied(2), "Preset bleibt erhalten");
    }

    #[test]
    fn recall_no_signal_fails_fast() {
        let now = Instant::now();
        let mut a = app();
        a.presets.set(0, preset("11D", 0xE1C0, "WDR 5"));
        a.preset_recall(0, now).unwrap();
        let fx = a.handle_event(&Event::NoSignal { channel: "11D".into() }, now + Duration::from_secs(1));
        assert!(matches!(fx.events[0], AppEvent::PresetStatus { status: PresetStatus::NotFound, .. }));
    }

    #[test]
    fn recall_service_missing_in_ensemble() {
        let now = Instant::now();
        let mut a = app();
        a.presets.set(0, preset("11D", 0xE1C0, "WDR 5"));
        a.preset_recall(0, now).unwrap();
        tune(&mut a, "11D", 0x1E1C, &[(0xE1C1, "1LIVE"), (0xE1C2, "WDR 2")], now + Duration::from_secs(2));
        assert!(a.is_pending());
        let fx = a.tick(now + Duration::from_secs(9));
        assert!(matches!(fx.events[0], AppEvent::PresetStatus { status: PresetStatus::NotFound, .. }));
        assert_eq!(a.presets.get(0).unwrap().sid, 0xE1C0);
    }

    #[test]
    fn recall_blocked_while_recording_and_empty_slot() {
        let now = Instant::now();
        let mut a = app();
        assert_eq!(a.preset_recall(3, now).unwrap_err(), AppError::Empty);
        assert_eq!(a.preset_recall(42, now).unwrap_err(), AppError::Slot);
        a.presets.set(3, preset("5C", 0xD210, "Dlf"));
        a.state.recording = true;
        assert_eq!(a.preset_recall(3, now).unwrap_err(), AppError::Recording);
    }

    #[test]
    fn store_asks_before_overwrite() {
        let now = Instant::now();
        let mut a = app();
        a.state.channel = Some("5C".into());
        tune(&mut a, "5C", 0x10BC, &[(0xD210, "Dlf"), (0xD220, "Dlf Kultur")], now);
        assert_eq!(a.preset_store(0, false).unwrap_err(), AppError::NoService);
        started(&mut a, 0xD210, now);
        let (r, fx) = a.preset_store(0, false).unwrap();
        assert!(r.stored && r.previous.is_none());
        assert!(matches!(fx.events[0], AppEvent::PresetsChanged { .. }));
        assert_eq!(a.active_preset_slot(), Some(0));
        started(&mut a, 0xD220, now);
        let (r, _) = a.preset_store(0, false).unwrap();
        assert!(!r.stored);
        assert_eq!(r.previous.unwrap().name, "Dlf");
        let (r, _) = a.preset_store(0, true).unwrap();
        assert!(r.stored);
        assert_eq!(a.presets.get(0).unwrap().name, "Dlf Kultur");
        // Drag aus der Liste
        let (r, _) = a.preset_store_service(4, Some((0xD210, 0)), false).unwrap();
        assert!(r.stored);
        assert_eq!(a.presets.get(4).unwrap().sid, 0xD210);
        assert!(a.preset_clear(4).unwrap().0.is_some());
        assert!(std::fs::read_to_string(a.dirs.presets_file()).unwrap().contains("Dlf Kultur"));
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn favorites_import_and_resolve_by_name() {
        let now = Instant::now();
        let mut a = app();
        let xml = r#"<preset_db>
 <PRESET_ELEMENT CHANNEL="5C" SERVICE_NAME="Dlf             "/>
 <PRESET_ELEMENT CHANNEL="11D" SERVICE_NAME="WDR 5           "/>
 <preset SERVICE_NAME="Dlf" CHANNEL="5C"/>
</preset_db>"#;
        assert_eq!(a.import_favorites_xml(xml), 2, "Duplikat wird nicht doppelt uebernommen");
        assert_eq!(a.presets.get(0).unwrap().name, "Dlf");
        assert_eq!(a.presets.get(1).unwrap().channel, "11D");
        assert_eq!(a.presets.get(1).unwrap().sid, 0);
        // Aufruf: SId unbekannt -> ueber den Namen aufloesen und nachtragen
        a.preset_recall(1, now).unwrap();
        a.handle_event(&Event::EnsembleFound { eid: 0x1E1C, name: "WDR".into(), channel: "11D".into() }, now);
        let fx = a.handle_event(&Event::ServiceAdded { service: svc(0xE1C0, "WDR 5") }, now);
        assert_eq!(fx.commands, vec![Command::SelectService { sid: 0xE1C0, scids: 0, slot: ServiceSlot::Primary }]);
        let p = a.presets.get(1).unwrap();
        assert_eq!((p.sid, p.eid), (0xE1C0, 0x1E1C));
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn gain_persisted_per_device_and_channel() {
        let now = Instant::now();
        let mut a = app();
        // AGC an: nichts merken, beim Kanalwechsel keinen Gain vorgeben
        a.set_channel("11D");
        a.handle_event(&Event::GainChanged { lna: 40, vga: 58, amp: false, agc: true }, now);
        assert_eq!(a.settings.gain_for("hackrf", "11D"), None);
        assert_eq!(a.set_channel("5C").commands, vec![Command::SetChannel { channel: "5C".into() }]);
        // AGC aus: manuelle Werte je Kanal merken
        a.settings.agc = false;
        a.set_channel("11D");
        a.handle_event(&Event::GainChanged { lna: 40, vga: 40, amp: false, agc: false }, now);
        a.handle_event(&Event::GainChanged { lna: 40, vga: 44, amp: false, agc: false }, now);
        assert_eq!(a.settings.gain_for("hackrf", "11D"), Some(Gain { lna: 40, vga: 44, amp: false }));
        assert_eq!(a.settings.gain_for("hackrf", "5C"), None);
        // Beim naechsten Wechsel auf 11D geht der gespeicherte Wert voraus
        a.set_channel("5C");
        let fx = a.set_channel("11D");
        assert_eq!(fx.commands[0], Command::SetGain { gain: Gain { lna: 40, vga: 44, amp: false } });
        // Anderes Geraet: eigener Satz, kein HackRF-Standard
        a.state.device = Some(crate::state::DeviceState { kind: "rtlsdr".into(), ..Default::default() });
        let fx = a.set_channel("11D");
        assert_eq!(fx.commands, vec![Command::SetChannel { channel: "11D".into() }]);
        // Im Scan nichts speichern
        a.state.device = Some(crate::state::DeviceState { kind: "hackrf".into(), ..Default::default() });
        a.state.scan.active = true;
        a.state.channel = Some("7B".into());
        a.handle_event(&Event::GainChanged { lna: 40, vga: 20, amp: true, agc: false }, now);
        assert_eq!(a.settings.gain_for("hackrf", "7B"), None);
    }

    #[test]
    fn startup_restores_last_service() {
        let now = Instant::now();
        let mut s = Settings::default();
        s.last_channel = Some("5C".into());
        s.last_service = Some((0xD210, 0));
        s.volume_percent = 55;
        let tmp = std::env::temp_dir().join(format!("dabclassic-app-start-{}", std::process::id()));
        let mut a = App::with(DataDirs::with_root(&tmp, true), s, Presets::default());
        let fx = a.startup(now);
        assert!(fx.commands.contains(&Command::SetVolume { percent: 55 }));
        assert!(fx.commands.contains(&Command::OpenDevice { source: SourceKind::HackRf { serial: None } }));
        assert!(fx.commands.contains(&Command::SetChannel { channel: "5C".into() }));
        assert!(a.is_pending());
        tune(&mut a, "5C", 0x10BC, &[(0xD220, "Dlf Kultur")], now);
        assert!(a.is_pending());
        let fx = a.handle_event(&Event::ServiceAdded { service: svc(0xD210, "Dlf") }, now);
        assert_eq!(fx.commands, vec![Command::SelectService { sid: 0xD210, scids: 0, slot: ServiceSlot::Primary }]);
        started(&mut a, 0xD210, now);
        a.save_all().unwrap();
        let back = Settings::load(&a.dirs.settings_file());
        assert_eq!(back.last_service, Some((0xD210, 0)));
        assert_eq!(back.device, "hackrf");
        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    fn step_service_wraps_and_alert_dismiss() {
        let now = Instant::now();
        let mut a = app();
        a.state.channel = Some("5C".into());
        tune(&mut a, "5C", 0x10BC, &[(3, "C"), (1, "A"), (2, "B")], now);
        started(&mut a, 3, now);
        let fx = a.step_service(1).unwrap();
        assert_eq!(fx.commands, vec![Command::SelectService { sid: 1, scids: 0, slot: ServiceSlot::Primary }]);
        let fx = a.step_service(-1).unwrap();
        assert_eq!(fx.commands, vec![Command::SelectService { sid: 2, scids: 0, slot: ServiceSlot::Primary }]);
        a.handle_event(&Event::EwsAlert { phase: EwsPhase::Trigger, sub_ch: 1, stage: 1, stage_raw: 0x81, iid: 1, locations: vec![], is_test: false }, now);
        let fx = a.command(Command::EwsDismiss).unwrap();
        assert_eq!(fx.commands, vec![Command::EwsDismiss]);
        assert!(a.state.alert.as_ref().unwrap().dismissed);
    }
}
