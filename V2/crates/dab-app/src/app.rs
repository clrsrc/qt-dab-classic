//! Anwendungslogik (Entscheidung 14): Preset-Aufruf als Zustandsmaschine,
//! Gain je Geraet und Kanal (Entscheidung 26), Wiederherstellung beim Start,
//! Umschaltsperre bei Aufnahme. Ohne Fenster und ohne Kernprozess testbar:
//! jede Aktion liefert [`Effects`] (Kommandos an den Kern, Hinweise an das
//! Frontend), die der Aufrufer ausfuehrt.

use crate::favorites;
use crate::state::{AppState, PendingState};
use crate::{DataDirs, Preset, Presets, Settings, PRESET_SLOTS};
use dab_api::{Command, Event, EwsPhase, Gain, ScanMode, ServiceInfo, ServiceSlot, SourceKind};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

/// Wartezeit auf `service_added(sid)` nach einem Kanalwechsel (Analyse 5.1).
pub const PRESET_TIMEOUT: Duration = Duration::from_secs(8);
/// Wartezeit auf `service_started` nach `select_service`: solange zeigt
/// `state.current` den neuen Dienst optimistisch an; danach wird die Anzeige
/// zurueckgesetzt (Review 16.09.2026, Befund 4). Der Kern meldet einen
/// gescheiterten Dienststart nur als Log-Warnung, nicht als eigenes
/// Ereignis; siehe `select_failed_by_log`.
pub const SELECT_TIMEOUT: Duration = Duration::from_secs(5);
/// Lebensdauer eines Eintrags in `expected_stops`: der Kern stoppt den
/// verdraengten Dienst synchron, sein `service_stopped` kommt innerhalb von
/// Millisekunden. Ein Eintrag, der so lange ueberlebt, gehoert zu einem
/// Dienst, den der Kern nie gestoppt hat (Dienststart fehlgeschlagen).
pub const EXPECTED_STOP_TTL: Duration = Duration::from_secs(10);
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
    /// Musik-Trennung (crate::music): Vorschlagsliste geaendert.
    MusicCandidates { candidates: Vec<dab_music::TrackCandidate> },
    /// EWS-Ortscodes uebersetzt (crate::ews_location); `iid`/`sub_ch` zum
    /// Abgleich, falls im Frontend inzwischen ein neuerer Alarm ansteht.
    EwsLocations { iid: u16, sub_ch: u8, location_info: Vec<crate::ews_location::LocationInfo> },
    /// EWF-Historie geaendert (ein Alarm endete, Bugfixes.txt #10); komplette
    /// Liste, neueste zuerst, wie `AppState::ews_history`.
    EwsHistory { history: Vec<crate::state::EwsHistoryEntry> },
    /// Verkehrs-/Sonderdurchsagen (crate::traffic): laufende, Historie,
    /// Unterstuetzung des laufenden Dienstes - wie `AppState::traffic_*`.
    Traffic { active: Option<crate::traffic::TrafficEntry>, history: Vec<crate::traffic::TrafficEntry>, supported: bool },
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
    /// Titelerkennung der Musik-Trennung (crate::music, Entscheidungen 6, 7).
    pub music: crate::music::MusicDetector,
    /// Verkehrsfunk-Durchsagen (crate::traffic).
    pub traffic: crate::traffic::TrafficCtl,
    pending: Option<Pending>,
    /// SIds, deren `service_stopped` wir noch erwarten, weil wir sie selbst
    /// durch eine neuere Auswahl ersetzt haben (Fund 15.09.2026: bei
    /// schnellem Umschalten - z. B. mehrfach hintereinander Favoriten
    /// anklicken - kommt der `service_stopped` des VERDRAENGTEN Dienstes oft
    /// erst an, wenn `state.current` laengst wieder denselben SId zeigt (weil
    /// er zwischendurch erneut angewaehlt wurde); ohne diese Liste wuerde er
    /// dann faelschlich als "kein Dienst" geloescht, obwohl Audio laeuft.
    /// Siehe `select_service` (Eintragen) und `handle_event` (Abgleich).
    /// Jeder Eintrag traegt seinen Zeitpunkt und verfaellt nach
    /// [`EXPECTED_STOP_TTL`]; Kanalwechsel, Geraet zu und Kern-Ende leeren
    /// die Liste (Review 16.09.2026, Befund 4).
    expected_stops: std::collections::VecDeque<(u32, Instant)>,
    /// Laufende optimistische Anzeige nach `select_service` (Befund 4):
    /// bis `service_started` kommt oder [`SELECT_TIMEOUT`] ablaeuft.
    optimistic: Option<Optimistic>,
}

/// `state.current` wurde in `select_service` vorab auf den neuen Dienst
/// gesetzt; `prev` ist die Anzeige davor (fuer den Fall, dass der Kern den
/// alten Dienst gar nicht gestoppt hat, z. B. "Dienst nicht in der FIC").
#[derive(Clone, Debug)]
struct Optimistic {
    sid: u32,
    scids: u8,
    prev: Option<crate::state::CurrentService>,
    deadline: Instant,
}

/// Log-Warnungen des Kerns (core.cpp `selectService`), die einen
/// gescheiterten Dienststart anzeigen. Es gibt dafuer kein eigenes Ereignis;
/// der Text ist der schnellste Weg, [`SELECT_TIMEOUT`] die Absicherung, falls
/// sich der Wortlaut aendert. `Some(true)`: der alte Dienst spielt weiter
/// (Abbruch VOR `stopOneLocked`); `Some(false)`: der alte Dienst ist bereits
/// gestoppt, der neue kommt nicht.
fn select_failed_by_log(text: &str) -> Option<bool> {
    if text.starts_with("Dienst nicht in der FIC") || text.starts_with("Dienstwechsel blockiert") {
        Some(true)
    } else if text.starts_with("Audiodienst noch nicht vollstaendig")
        || text.starts_with("MP2-Dienst")
        || text.starts_with("select_service ohne geoeffnete Quelle")
    {
        Some(false)
    } else {
        None
    }
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
            music: Default::default(),
            traffic: Default::default(),
            pending: None,
            expected_stops: Default::default(),
            optimistic: None,
        };
        app.stations_load();
        app.timeshift_init();
        app
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
            // Heimatkoordinaten fuer das EWS-Geofencing: der Kern entscheidet
            // damit selbst (synchron im FIC-Callback), ob er auf den Warndienst
            // umschaltet, und meldet sein Urteil als `EwsAlert.relevant` zurueck.
            // Ohne Koordinaten (None/None) bleibt es beim ungefilterten Verhalten.
            .cmd(Command::SetHomeLocation { lat: s.home_lat, lon: s.home_lon })
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
        // `service_stopped` eines Dienstes, den WIR selbst durch eine neuere
        // Auswahl verdraengt haben (siehe `select_service`): kommt oft erst
        // an, nachdem `state.current` (bei schnellem Umschalten sogar
        // wiederholt) laengst weitergezogen ist. Ignorieren statt an
        // state/epg/tii/timeshift/music weiterzureichen, sonst loescht ein
        // veralteter Stop die Anzeige eines laengst wieder aktiven Dienstes
        // ("kein Dienst", obwohl Audio laeuft - Live-Test Stefan 15.09.2026).
        if let Event::ServiceStopped { slot: ServiceSlot::Primary, sid } = ev {
            if let Some(pos) = self.expected_stops.iter().position(|(s, _)| s == sid) {
                self.expected_stops.remove(pos);
                return Effects::default();
            }
        }
        self.state.apply(ev);
        let mut fx = self.media_on_event(ev);
        match ev {
            Event::EwsAlert { locations, iid, sub_ch, phase, .. } => {
                // Ortscodes uebersetzen (Mittelpunkt, Entfernung/Richtung):
                // braucht die Heimatkoordinaten aus den Settings, die
                // `AppState::apply` nicht kennt (siehe state.rs). Das
                // Frontend baut `s.alert` selbst aus dem rohen `dab://event`
                // auf (ohne `location_info`); hier zusaetzlich als
                // `dab://app`-Ereignis nachreichen, gegen `iid`/`sub_ch`
                // geprueft, damit ein spaeterer neuer Alarm nicht mit den
                // Ortscodes des vorigen ueberschrieben wird.
                if *phase != EwsPhase::End {
                    let home = match (self.settings.home_lat, self.settings.home_lon) {
                        (Some(lat), Some(lon)) => Some((lat, lon)),
                        _ => None,
                    };
                    let location_info = crate::ews_location::translate(locations, home);
                    if let Some(alert) = self.state.alert.as_mut() {
                        alert.location_info = location_info.clone();
                    }
                    fx = fx.ev(AppEvent::EwsLocations { iid: *iid, sub_ch: *sub_ch, location_info });
                } else {
                    // `AppState::apply` hat den beendeten Alarm bereits in
                    // `ews_history` einsortiert (Bugfixes.txt #10); die Liste
                    // baut sich (anders als `s.alert`) nicht aus rohen
                    // Kern-Ereignissen zusammen, darum hier nachreichen.
                    fx = fx.ev(AppEvent::EwsHistory { history: self.state.ews_history.clone() });
                }
            }
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
                // Der Kern hat den Dienst wirklich gestartet: die optimistische
                // Anzeige ist bestaetigt (bzw. durch die echte ersetzt).
                self.optimistic = None;
                self.remember_last();
            }
            Event::Log { level: dab_api::LogLevel::Warn | dab_api::LogLevel::Error, text } => {
                if self.optimistic.is_some() {
                    if let Some(old_still_plays) = select_failed_by_log(text) {
                        self.optimistic_resolve(old_still_plays, text);
                    }
                }
            }
            Event::DeviceError { message } => {
                self.pending = None;
                self.state.pending = None;
                fx = fx.ev(AppEvent::Notice { level: NoticeLevel::Error, text: message.clone() });
            }
            Event::Exiting { .. } | Event::DeviceClosed => {
                self.pending = None;
                self.state.pending = None;
                // Kein Dienst laeuft mehr: keine ausstehenden Stops, keine
                // optimistische Anzeige (Befund 4).
                self.expected_stops.clear();
                self.optimistic = None;
            }
            _ => {}
        }
        fx.append(self.recording_on_event(ev));
        fx.append(self.sched_on_event(ev));
        fx.append(self.debug_on_event(ev, now));
        fx.append(self.stations_on_event(ev, now));
        fx.append(self.timeshift_on_event(ev));
        fx.append(self.music_on_event(ev, crate::state::unix_now()));
        fx.append(self.traffic_on_event(ev, crate::state::unix_now()));
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
        // Optimistische Anzeige ohne `service_started` (Befund 4): kam das
        // `service_stopped` des alten Dienstes nie an, hat der Kern ihn auch
        // nie gestoppt - dann spielt er weiter und die Anzeige geht zurueck.
        if let Some(o) = self.optimistic.clone() {
            if now >= o.deadline {
                let old_still_plays = o.prev.as_ref().map(|p| self.expected_stops.iter().any(|(s, _)| *s == p.sid)).unwrap_or(false);
                self.optimistic_resolve(old_still_plays, "kein service_started innerhalb der Wartezeit");
            }
        }
        // Verfallene Vormerkungen (der Kern stoppt synchron; was so lange
        // ueberlebt, kommt nie mehr) - sonst verschluckt ein haengender
        // Eintrag spaeter genau ein echtes `service_stopped` desselben SId.
        self.expected_stops.retain(|(_, t)| now.saturating_duration_since(*t) < EXPECTED_STOP_TTL);
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
        // `select_service` statt eines selbst gebauten Kommandos: sonst fehlt
        // Preset-/Senderlisten-Aufrufen die sofortige optimistische Anzeige
        // (Bugfixes.txt #5/#7) UND die `expected_stops`-Vormerkung (Fund
        // 15.09.2026, siehe `handle_event`). `tune_to` hat `recording`/
        // `scan.active` nur beim KLICK geprueft; `pending_found` laeuft
        // spaeter (nach dem Kanalwechsel), in der Zwischenzeit kann z. B. ein
        // Timer eine Aufnahme gestartet haben - select_service kann also doch
        // noch scheitern. Vorher wurde das SelectService-Kommando hier immer
        // unbedingt gebaut, also auch waehrend einer Aufnahme gesendet
        // (Inkonsistenz zur manuellen Umschaltung); jetzt greift dieselbe
        // Sperre wie ueberall sonst, aber ehrlich gemeldet statt eines
        // faelschlich "Selected" trotz ausgebliebener Umschaltung.
        let fx_select = match self.select_service(s.sid, s.scids) {
            Ok(fx) => fx,
            Err(e) => return Effects::default().ev(AppEvent::Notice { level: NoticeLevel::Warn, text: e.to_string() }),
        };
        let mut fx = fx_select;
        // Favoriten-Import: SId/EId nachtragen, Namen aktualisieren.
        if let Some(slot) = p.slot {
            if let Some(preset) = self.presets.slots.get_mut(slot).and_then(|x| x.as_mut()) {
                let eid = self.state.ensemble.as_ref().map(|e| e.eid).unwrap_or(preset.eid);
                let short = s.short_name.trim();
                if preset.sid != s.sid || preset.eid != eid || preset.name != s.name.trim() || (!short.is_empty() && preset.short_name != short) {
                    preset.sid = s.sid;
                    preset.scids = s.scids;
                    preset.eid = eid;
                    preset.name = s.name.trim().to_string();
                    if !short.is_empty() {
                        preset.short_name = short.to_string();
                    }
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
        self.expected_stops.clear();
        self.optimistic = None;
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
            // Alle Dienste des alten Kanals enden; nichts mehr vorzumerken (Befund 4).
            self.expected_stops.clear();
            self.optimistic = None;
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
        let cmd = Command::SelectService { sid, scids, slot: ServiceSlot::Primary };
        let prev = self.state.current.clone();
        if prev.as_ref().map(|c| c.sid == sid && c.scids == scids).unwrap_or(false) {
            // Derselbe Dienst (SId UND Komponente, wie core.cpp selectService
            // prueft): der Kern tut nichts und meldet nichts - die Anzeige
            // bleibt, wie sie ist (kein optimistischer Neuanfang, der nach
            // SELECT_TIMEOUT den laufenden Dienst loeschen wuerde).
            return Ok(Effects::default().cmd(cmd));
        }
        // Der bisherige Dienst wird im Kern verdraengt (core.cpp selectService
        // stoppt ihn synchron vor dem Start des neuen; auch beim Wechsel auf
        // eine andere Komponente desselben Dienstes) - dessen spaeter
        // eintreffendes `service_stopped` darf `state.current` nicht mehr
        // loeschen, siehe `expected_stops` und `handle_event`.
        let now = Instant::now();
        if let Some(old) = prev.as_ref() {
            self.expected_stops.push_back((old.sid, now));
        }
        // Kopfzeile sofort auf den neuen Dienst umstellen: `service_started`
        // kommt aus dem Kern bewusst erst mit dem ersten dekodierten
        // Audioblock (V2/docs/protocol.md), ohne Fallback bei verlorenem
        // erstem Block. Bis dahin sonst "kein Dienst" (Bugfixes.txt #5/#7);
        // der codec wird nachgetragen, sobald das Event eintrifft (state.rs
        // Event::ServiceStarted), Name/SId stehen aber sofort. Bleibt es aus,
        // raeumt `tick` (SELECT_TIMEOUT) bzw. die Log-Warnung des Kerns auf.
        self.state.current = Some(crate::state::CurrentService { sid, scids, codec: None, stereo: false });
        self.optimistic = Some(Optimistic { sid, scids, prev, deadline: now + SELECT_TIMEOUT });
        Ok(Effects::default().cmd(cmd))
    }

    /// Optimistische Anzeige aufloesen, wenn der Kern den Dienst nicht
    /// gestartet hat (Befund 4). `old_still_plays`: der alte Dienst wurde nie
    /// gestoppt -> Anzeige zurueck auf ihn (nur, wenn er selbst bestaetigt
    /// lief, sonst "kein Dienst"); andernfalls "kein Dienst".
    fn optimistic_resolve(&mut self, old_still_plays: bool, why: &str) {
        let Some(o) = self.optimistic.take() else { return };
        let still_optimistic = self
            .state
            .current
            .as_ref()
            .map(|c| c.sid == o.sid && c.scids == o.scids && c.codec.is_none())
            .unwrap_or(false);
        if !still_optimistic {
            return;
        }
        // Die Vormerkung des alten Dienstes ist erledigt - entweder kam sein
        // Stop laengst, oder er kommt nie (der Kern hat ihn nicht gestoppt).
        if let Some(prev) = o.prev.as_ref() {
            if let Some(pos) = self.expected_stops.iter().rposition(|(s, _)| *s == prev.sid) {
                self.expected_stops.remove(pos);
            }
        }
        match o.prev {
            Some(prev) if old_still_plays && prev.codec.is_some() => {
                log::info!("Dienst {:04X}/{} nicht gestartet ({why}); Anzeige zurueck auf {:04X}", o.sid, o.scids, prev.sid);
                self.state.current = Some(prev);
            }
            _ => {
                log::info!("Dienst {:04X}/{} nicht gestartet ({why}); Anzeige geleert", o.sid, o.scids);
                self.state.clear_service();
            }
        }
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
        self.expected_stops.clear();
        self.optimistic = None;
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
        if old.home_lat != s.home_lat || old.home_lon != s.home_lon {
            // Geofencing im Kern nachziehen (siehe `startup`); die gleichen
            // Koordinaten dienen weiterhin der TII-Entfernungsanzeige, die
            // `debug_on_settings` behandelt.
            fx = fx.cmd(Command::SetHomeLocation { lat: s.home_lat, lon: s.home_lon });
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
        fx.append(self.music_on_settings(&old));
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
            short_name: svc.short_name.trim().to_string(),
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
                Preset { channel: fav.channel, eid: 0, sid: 0, scids: 0, name: fav.name, short_name: String::new(), logo_path: None, logo_data_url: None, stored_at: crate::state::unix_now() },
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
        ServiceInfo { sid, scids: 0, name: name.into(), is_audio: true, is_primary: true, sub_ch: 1, bitrate_kbps: 96, pty: 0, short_name: String::new(), language: 0 }
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
        Preset { channel: channel.into(), eid: 0x10BC, sid, scids: 0, name: name.into(), short_name: String::new(), logo_path: None, logo_data_url: None, stored_at: 0 }
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
        // Bugfixes.txt #5/#7 galt bisher nur select_service()/step_service();
        // pending_found() (Preset-/Senderlisten-Aufruf) baute das Kommando
        // bislang selbst und liess state.current bis zum echten
        // service_started unveraendert (Fund 15.09.2026).
        assert_eq!(a.state.current.as_ref().map(|c| c.sid), Some(0xD220), "Preset-Aufruf muss sofort anzeigen, nicht erst nach service_started");
    }

    /// Fund 15.09.2026 (Stefans Live-Test): schnelles Umschalten (Preset A ->
    /// Preset B, bevor A jemals ein echtes `service_started` bekommen hat)
    /// darf die Anzeige von B nicht loeschen, wenn As verspaetetes
    /// `service_stopped` eintrifft - vorher geschah das, obwohl B laengst
    /// (optimistisch oder echt) angezeigt wurde und ggf. schon Audio lief.
    #[test]
    fn stale_service_stopped_for_a_superseded_selection_is_ignored() {
        let now = Instant::now();
        let mut a = app();
        a.state.channel = Some("5C".into());
        tune(&mut a, "5C", 0x10BC, &[(0xD210, "Dlf"), (0xD220, "Dlf Kultur")], now);
        a.select_service(0xD210, 0).unwrap(); // Preset A angewaehlt, noch kein service_started
        let fx = a.select_service(0xD220, 0).unwrap(); // Preset B verdraengt A, bevor A startete
        assert!(fx.commands.contains(&Command::SelectService { sid: 0xD220, scids: 0, slot: ServiceSlot::Primary }));
        assert_eq!(a.state.current.as_ref().map(|c| c.sid), Some(0xD220));

        // As (verspaetetes) service_stopped darf B nicht von der Anzeige nehmen.
        a.handle_event(&Event::ServiceStopped { slot: ServiceSlot::Primary, sid: 0xD210 }, now);
        assert_eq!(a.state.current.as_ref().map(|c| c.sid), Some(0xD220), "veraltetes service_stopped von A hat B faelschlich geloescht");

        // Ein ECHTES service_stopped fuer den gerade aktiven Dienst B (z. B.
        // Empfang verloren) muss weiterhin ganz normal loeschen.
        a.handle_event(&Event::ServiceStopped { slot: ServiceSlot::Primary, sid: 0xD220 }, now);
        assert!(a.state.current.is_none(), "ein echtes service_stopped des aktiven Dienstes muss weiterhin loeschen");
    }

    /// Review 16.09.2026, Befund 4: Vormerkungen in `expected_stops` verfielen
    /// nie. Nach einem Kanalwechsel darf ein alter Eintrag kein echtes
    /// `service_stopped` desselben SId mehr verschlucken.
    #[test]
    fn expected_stops_are_cleared_on_channel_change_and_expire() {
        let now = Instant::now();
        let mut a = app();
        a.state.channel = Some("5C".into());
        tune(&mut a, "5C", 0x10BC, &[(0xD210, "Dlf"), (0xD220, "Dlf Kultur")], now);
        started(&mut a, 0xD210, now);
        a.select_service(0xD220, 0).unwrap(); // D210 vorgemerkt
        assert_eq!(a.expected_stops.len(), 1);
        a.set_channel("11D");
        assert!(a.expected_stops.is_empty(), "Kanalwechsel leert die Vormerkungen");
        assert!(a.optimistic.is_none());
        // Zurueck auf 5C, D210 laeuft wieder; sein echtes Stop muss loeschen.
        a.set_channel("5C");
        tune(&mut a, "5C", 0x10BC, &[(0xD210, "Dlf")], now);
        started(&mut a, 0xD210, now);
        a.handle_event(&Event::ServiceStopped { slot: ServiceSlot::Primary, sid: 0xD210 }, now);
        assert!(a.state.current.is_none(), "echtes service_stopped wurde von einer alten Vormerkung verschluckt");

        // Verfall: ein Eintrag, dessen Stop nie kommt, ueberlebt EXPECTED_STOP_TTL nicht.
        started(&mut a, 0xD210, now);
        a.select_service(0xD220, 0).unwrap();
        a.optimistic = None; // nur den Verfall pruefen, nicht die Rueckstellung
        a.tick(now + EXPECTED_STOP_TTL + Duration::from_secs(1));
        assert!(a.expected_stops.is_empty(), "Vormerkung ist nicht verfallen");
        // DeviceClosed / Exiting leeren ebenfalls.
        a.select_service(0xD210, 0).unwrap();
        a.handle_event(&Event::DeviceClosed, now);
        assert!(a.expected_stops.is_empty() && a.optimistic.is_none());
    }

    /// Befund 4: startet der Kern den neuen Dienst nicht (kein
    /// `service_started`), darf `state.current` ihn nicht dauerhaft anzeigen.
    /// Kam kein `service_stopped` des alten Dienstes, hat der Kern ihn auch
    /// nie gestoppt ("Dienst nicht in der FIC" bricht VOR dem Stop ab) - dann
    /// zeigt die Anzeige wieder den weiterlaufenden alten Dienst.
    #[test]
    fn optimistic_current_falls_back_when_service_started_never_comes() {
        let now = Instant::now();
        let mut a = app();
        a.state.channel = Some("5C".into());
        tune(&mut a, "5C", 0x10BC, &[(0xD210, "Dlf"), (0xD220, "Dlf Kultur")], now);
        started(&mut a, 0xD210, now);
        a.select_service(0xD220, 0).unwrap();
        assert_eq!(a.state.current.as_ref().map(|c| c.sid), Some(0xD220));
        // Vor Ablauf: nichts passiert.
        a.tick(now + Duration::from_secs(4));
        assert_eq!(a.state.current.as_ref().map(|c| c.sid), Some(0xD220));
        // Ablauf ohne service_stopped(D210): alter Dienst spielt weiter.
        a.tick(now + SELECT_TIMEOUT + Duration::from_secs(1));
        assert_eq!(a.state.current.as_ref().map(|c| (c.sid, c.codec.is_some())), Some((0xD210, true)), "Anzeige muss auf den weiterlaufenden Dienst zurueck");
        assert!(a.expected_stops.is_empty(), "die Vormerkung fuer D210 ist damit erledigt");
        assert!(a.optimistic.is_none());
        // Ein spaeteres echtes Stop von D210 loescht normal.
        a.handle_event(&Event::ServiceStopped { slot: ServiceSlot::Primary, sid: 0xD210 }, now);
        assert!(a.state.current.is_none());

        // Variante: der alte Dienst WURDE gestoppt (Stop kam), der neue kommt
        // nie ("noch nicht vollstaendig in der FIC") -> "kein Dienst".
        started(&mut a, 0xD210, now);
        a.select_service(0xD220, 0).unwrap();
        a.handle_event(&Event::ServiceStopped { slot: ServiceSlot::Primary, sid: 0xD210 }, now);
        assert_eq!(a.state.current.as_ref().map(|c| c.sid), Some(0xD220), "vorgemerktes Stop laesst die Anzeige stehen");
        a.tick(now + SELECT_TIMEOUT + Duration::from_secs(1));
        assert!(a.state.current.is_none(), "ohne service_started und mit gestopptem altem Dienst: kein Dienst");

        // Variante: service_started kommt rechtzeitig -> keine Rueckstellung.
        a.select_service(0xD220, 0).unwrap();
        started(&mut a, 0xD220, now);
        a.tick(now + SELECT_TIMEOUT + Duration::from_secs(5));
        assert_eq!(a.state.current.as_ref().map(|c| (c.sid, c.codec.is_some())), Some((0xD220, true)));
    }

    /// Befund 4: die Log-Warnung des Kerns ist der schnellere Weg als der
    /// Timeout - und die Wiederanwahl desselben Dienstes (SId + Komponente)
    /// startet keine optimistische Anzeige, weil der Kern dann nichts tut.
    #[test]
    fn core_log_warning_resolves_optimistic_current_and_same_service_is_a_noop() {
        let now = Instant::now();
        let mut a = app();
        a.state.channel = Some("5C".into());
        tune(&mut a, "5C", 0x10BC, &[(0xD210, "Dlf"), (0xD220, "Dlf Kultur")], now);
        started(&mut a, 0xD210, now);
        a.select_service(0xD220, 0).unwrap();
        a.handle_event(&Event::Log { level: dab_api::LogLevel::Warn, text: "Dienst nicht in der FIC: SId 53792".into() }, now);
        assert_eq!(a.state.current.as_ref().map(|c| c.sid), Some(0xD210), "alter Dienst spielt weiter");
        assert!(a.expected_stops.is_empty());

        a.select_service(0xD220, 0).unwrap();
        a.handle_event(&Event::ServiceStopped { slot: ServiceSlot::Primary, sid: 0xD210 }, now);
        a.handle_event(&Event::Log { level: dab_api::LogLevel::Warn, text: "Audiodienst noch nicht vollstaendig in der FIC".into() }, now);
        assert!(a.state.current.is_none(), "alter Dienst gestoppt, neuer kommt nicht");

        // Unverwandte Warnungen aendern nichts.
        a.select_service(0xD220, 0).unwrap();
        a.handle_event(&Event::Log { level: dab_api::LogLevel::Warn, text: "irgendwas anderes".into() }, now);
        assert_eq!(a.state.current.as_ref().map(|c| c.sid), Some(0xD220));

        // Gleicher Dienst noch einmal: Anzeige (inkl. codec) bleibt, kein Timeout.
        started(&mut a, 0xD220, now);
        let fx = a.select_service(0xD220, 0).unwrap();
        assert_eq!(fx.commands.len(), 1);
        assert!(a.optimistic.is_none());
        assert!(a.expected_stops.is_empty());
        a.tick(now + SELECT_TIMEOUT + Duration::from_secs(1));
        assert_eq!(a.state.current.as_ref().map(|c| (c.sid, c.codec.is_some())), Some((0xD220, true)));

        // Andere Komponente desselben Dienstes: der Kern stoppt die alte,
        // ihr service_stopped(sid) darf die neue Anzeige nicht loeschen.
        a.select_service(0xD220, 1).unwrap();
        assert_eq!(a.expected_stops.len(), 1);
        a.handle_event(&Event::ServiceStopped { slot: ServiceSlot::Primary, sid: 0xD220 }, now);
        assert_eq!(a.state.current.as_ref().map(|c| (c.sid, c.scids)), Some((0xD220, 1)));
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

    /// Fund 15.09.2026: `tune_to` prueft `recording`/`scan.active` nur beim
    /// Klick; startet zwischen Klick und dem tatsaechlichen `ServiceAdded`
    /// (z. B. durch einen Timer) eine Aufnahme, darf `pending_found` NICHT
    /// mehr unbedingt umschalten UND nicht mehr "Selected" behaupten, obwohl
    /// nichts passiert ist.
    #[test]
    fn recall_blocked_by_recording_reports_the_reason_instead_of_pretending_success() {
        let now = Instant::now();
        let mut a = app();
        a.state.channel = Some("5C".into());
        tune(&mut a, "5C", 0x10BC, &[(0xD210, "Dlf")], now);
        a.presets.set(1, preset("11D", 0xE1C0, "WDR 5"));
        a.preset_recall(1, now).unwrap();
        a.state.recording = true; // Aufnahme startet waehrend des Kanalwechsels
        let fx = a.handle_event(&Event::ServiceAdded { service: svc(0xE1C0, "WDR 5") }, now + Duration::from_secs(2));
        assert!(fx.commands.is_empty(), "waehrend einer Aufnahme darf keine Umschaltung gesendet werden");
        assert!(
            matches!(&fx.events[0], AppEvent::Notice { level: NoticeLevel::Warn, .. }),
            "muss den Grund melden statt Selected vorzutaeuschen: {:?}",
            fx.events
        );
        assert!(!fx.events.iter().any(|e| matches!(e, AppEvent::PresetStatus { status: PresetStatus::Selected, .. })));
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

    /// 17.09.2026: Kurzlabel (FIG 1 Zeichen-Flags) wandert in den Speicher und
    /// wird beim Aufruf nachgetragen, wenn der Speicher noch keins hat (Favoriten-Import).
    #[test]
    fn store_keeps_short_label_and_recall_fills_it() {
        let now = Instant::now();
        let mut a = app();
        a.state.channel = Some("5C".into());
        tune(&mut a, "5C", 0x10BC, &[(0xD210, "Deutschlandfunk")], now);
        let mut full = svc(0xD210, "Deutschlandfunk");
        full.short_name = "Dlf".into();
        full.language = 0x08;
        a.handle_event(&Event::ServiceAdded { service: full }, now);
        started(&mut a, 0xD210, now);
        let (r, _) = a.preset_store(0, false).unwrap();
        assert!(r.stored);
        assert_eq!(a.presets.get(0).unwrap().short_name, "Dlf");
        assert_eq!(a.state.service(0xD210, 0).unwrap().language, 0x08);
        // Favoriten-Import ohne Kurzlabel: beim Aufruf wird es aus dem FIC ergaenzt
        a.presets.set(1, preset("5C", 0, "Deutschlandfunk"));
        assert!(a.preset_recall(1, now).is_ok());
        let p = a.presets.get(1).unwrap();
        assert_eq!(p.sid, 0xD210);
        assert_eq!(p.short_name, "Dlf");
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
    fn ews_alert_translates_locations_with_home_and_reports_them() {
        let now = Instant::now();
        let mut a = app();
        // Duesseldorf als Heimat (Sendestandort aus dem Warntag-Test).
        a.settings.home_lat = Some(51.2180);
        a.settings.home_lon = Some(6.7617);
        let ev = Event::EwsAlert {
            phase: EwsPhase::Trigger,
            sub_ch: 1,
            stage: 0,
            stage_raw: 0x01,
            iid: 1,
            locations: vec!["Z1:5C+F300".into(), "bad-code".into()],
            is_test: false,
            relevant: None,
        };
        let fx = a.handle_event(&ev, now);
        let alert = a.state.alert.as_ref().expect("Alarm gesetzt");
        assert_eq!(alert.location_info.len(), 2);
        assert!(alert.location_info[0].distance_km.is_some(), "gueltiger Code bekommt eine Entfernung");
        assert!(alert.location_info[1].distance_km.is_none(), "unbekannter Code bleibt ohne Entfernung");
        match fx.events.as_slice() {
            [AppEvent::EwsLocations { iid, sub_ch, location_info }] => {
                assert_eq!(*iid, 1);
                assert_eq!(*sub_ch, 1);
                assert_eq!(location_info, &alert.location_info);
            }
            other => panic!("EwsLocations-Ereignis erwartet, nicht {other:?}"),
        }

        // Phase "end": keine Ortscodes mehr zu uebersetzen, aber der Alarm
        // wandert in die Sitzungs-Historie (Bugfixes.txt #10) und wird als
        // EwsHistory nachgereicht.
        let end = Event::EwsAlert { phase: EwsPhase::End, sub_ch: 1, stage: 0, stage_raw: 0, iid: 1, locations: vec![], is_test: false, relevant: None };
        let fx = a.handle_event(&end, now);
        assert!(a.state.alert.is_none());
        assert_eq!(a.state.ews_history.len(), 1);
        assert_eq!(a.state.ews_history[0].iid, 1);
        assert_eq!(a.state.ews_history[0].location_info.len(), 2, "die zuletzt uebersetzten Ortscodes bleiben erhalten");
        match fx.events.as_slice() {
            [AppEvent::EwsHistory { history }] => assert_eq!(history, &a.state.ews_history),
            other => panic!("EwsHistory-Ereignis erwartet, nicht {other:?}"),
        }
    }

    #[test]
    fn home_location_goes_to_the_core_on_startup_and_on_change() {
        let now = Instant::now();
        let mut s = Settings::default();
        s.autostart = false;
        s.home_lat = Some(51.2180);
        s.home_lon = Some(6.7617);
        let tmp = std::env::temp_dir().join(format!("dabclassic-app-home-{}", std::process::id()));
        let mut a = App::with(DataDirs::with_root(&tmp, true), s, Presets::default());
        let fx = a.startup(now);
        assert!(
            fx.commands.contains(&Command::SetHomeLocation { lat: Some(51.2180), lon: Some(6.7617) }),
            "Startwerte enthalten die Heimatkoordinaten fuers Geofencing"
        );

        // Aenderung ueber die Einstellungen: genau einmal nachziehen.
        let fx = a.home_set(Some(48.8584), Some(2.2945));
        assert!(fx.commands.contains(&Command::SetHomeLocation { lat: Some(48.8584), lon: Some(2.2945) }));
        // Unveraenderte Einstellungen schicken nichts.
        let same = a.settings.clone();
        let fx = a.update_settings(same);
        assert!(!fx.commands.iter().any(|c| matches!(c, Command::SetHomeLocation { .. })));
        // Loeschen schickt None/None (Kern schaltet das Geofencing ab).
        let fx = a.home_set(None, None);
        assert!(fx.commands.contains(&Command::SetHomeLocation { lat: None, lon: None }));
        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    fn ews_alert_carries_the_cores_relevance_verdict_into_state_and_history() {
        let now = Instant::now();
        let mut a = app();
        // Eiffelturm-Testalarm des Bundesmux, von einer deutschen Heimat aus
        // gesehen: der Kern hat bereits entschieden (relevant = false), die
        // App reicht das Urteil nur weiter und rechnet nichts nach.
        let ev = Event::EwsAlert {
            phase: EwsPhase::Trigger,
            sub_ch: 1,
            stage: 0,
            stage_raw: 0x01,
            iid: 3,
            locations: vec!["Z1:5C+F300".into()],
            is_test: false,
            relevant: Some(false),
        };
        a.handle_event(&ev, now);
        assert_eq!(a.state.alert.as_ref().expect("Alarm gesetzt").relevant, Some(false));

        let end = Event::EwsAlert { phase: EwsPhase::End, sub_ch: 1, stage: 0, stage_raw: 0, iid: 3, locations: vec![], is_test: false, relevant: Some(false) };
        let fx = a.handle_event(&end, now);
        assert_eq!(a.state.ews_history[0].relevant, Some(false), "auch in der Sitzungs-Historie");
        match fx.events.as_slice() {
            [AppEvent::EwsHistory { history }] => assert_eq!(history[0].relevant, Some(false)),
            other => panic!("EwsHistory-Ereignis erwartet, nicht {other:?}"),
        }

        // Ohne Heimatkoordinaten im Kern bleibt es beim Ausgangsverhalten (None).
        let plain = Event::EwsAlert { phase: EwsPhase::Trigger, sub_ch: 1, stage: 0, stage_raw: 0x01, iid: 4, locations: vec![], is_test: false, relevant: None };
        a.handle_event(&plain, now);
        assert_eq!(a.state.alert.as_ref().expect("Alarm gesetzt").relevant, None);
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
        // select_service setzt state.current sofort optimistisch (Bugfixes.txt
        // #5/#7), daher fuehrt "prev" direkt danach wieder zur Ausgangsstation
        // zurueck, statt (wie vor dem Fix) vom noch nicht aktualisierten alten
        // current aus zu rechnen.
        let fx = a.step_service(-1).unwrap();
        assert_eq!(fx.commands, vec![Command::SelectService { sid: 3, scids: 0, slot: ServiceSlot::Primary }]);
        a.handle_event(&Event::EwsAlert { phase: EwsPhase::Trigger, sub_ch: 1, stage: 1, stage_raw: 0x81, iid: 1, locations: vec![], is_test: false, relevant: None }, now);
        let fx = a.command(Command::EwsDismiss).unwrap();
        assert_eq!(fx.commands, vec![Command::EwsDismiss]);
        assert!(a.state.alert.as_ref().unwrap().dismissed);
    }
}
