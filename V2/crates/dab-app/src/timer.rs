//! Timer (Port von v1 `unified-timer-model`): vier Typen – Umschalten und
//! Aufnahme, jeweils manuell oder aus dem EPG. Persistenz `data/timers.json`
//! (Version 2, Unix-Zeiten); die v1-Datei `.qt-dab-timers.json` (lokale
//! ISO-Zeiten, Dauer in Minuten, Typ als Zahl) wird gelesen und beim ersten
//! Start einmalig importiert, wenn `data/timers.json` fehlt.
//!
//! Der Scheduler laeuft im Sekundentakt aus [`App::tick`](crate::App::tick):
//! Aufnahme-Timer werden mit Vorlauf faellig (`record_pre_s`) und enden mit
//! Nachlauf (`record_post_s`, Entscheidung 18). Faellige Timer stimmen den
//! Kanal ab, waehlen den Dienst und starten/stoppen die Aufnahme ueber
//! [`Effects`]; die Systemzeit ist massgeblich (`clock_time` des Kerns wird
//! ignoriert). Waehrend einer laufenden Aufnahme wartet ein Timer, bis sie
//! endet (v1 verwarf ihn stillschweigend).

use crate::app::{App, AppError, AppEvent, Effects, NoticeLevel};
use crate::DataDirs;
use chrono::{DateTime, Local, NaiveDateTime, TimeZone};
use dab_api::{Command, Event, ServiceSlot};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// Dateiformat-Version von `timers.json`.
pub const TIMERS_VERSION: u32 = 2;
/// Umschalt-Timer, die laenger als so viele Sekunden ueberfaellig sind, gelten als verpasst.
pub const SWITCH_GRACE_S: i64 = 300;
/// Zeit, die ein feuernder Timer auf Kanal, Dienst und Audio warten darf.
pub const FIRE_TIMEOUT_S: i64 = 45;

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[serde(rename_all = "snake_case")]
pub enum TimerKind {
    /// v1 `ManualSwitch` (0)
    ManualSwitch,
    /// v1 `ManualRecord` (1)
    ManualRecord,
    /// v1 `EpgSwitch` (2)
    EpgSwitch,
    /// v1 `EpgRecord` (3)
    EpgRecord,
}

impl TimerKind {
    pub fn is_record(self) -> bool {
        matches!(self, TimerKind::ManualRecord | TimerKind::EpgRecord)
    }

    pub fn from_v1(code: i64) -> Option<Self> {
        Some(match code {
            0 => TimerKind::ManualSwitch,
            1 => TimerKind::ManualRecord,
            2 => TimerKind::EpgSwitch,
            3 => TimerKind::EpgRecord,
            _ => return None,
        })
    }

    pub fn v1_code(self) -> u8 {
        match self {
            TimerKind::ManualSwitch => 0,
            TimerKind::ManualRecord => 1,
            TimerKind::EpgSwitch => 2,
            TimerKind::EpgRecord => 3,
        }
    }
}

/// Ein Timer-Eintrag. Zeiten als Unix-Sekunden (UTC), Dauer in Sekunden
/// (0 = nur umschalten bzw. Aufnahme mit offenem Ende wie in v1).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Timer {
    pub id: u32,
    #[serde(rename = "type")]
    pub kind: TimerKind,
    pub active: bool,
    pub fired: bool,
    /// Kanal ("5C"); leer = aktueller Kanal (v1-Import).
    pub channel: String,
    #[serde(default)]
    pub eid: u16,
    /// 0 = Dienst ueber den Namen aufloesen (v1-Import).
    #[serde(default)]
    pub sid: u32,
    #[serde(default)]
    pub scids: u8,
    pub service: String,
    #[serde(default)]
    pub title: String,
    pub start_unix: i64,
    #[serde(default)]
    pub duration_s: u32,
}

impl Timer {
    /// Ende der Sendung (ohne Nachlauf); `None` bei Dauer 0.
    pub fn end_unix(&self) -> Option<i64> {
        (self.duration_s > 0).then(|| self.start_unix + self.duration_s as i64)
    }

    /// Belegtes Zeitfenster inkl. Vor-/Nachlauf (nur Aufnahme-Timer haben Vor-/Nachlauf).
    pub fn window(&self, pre_s: i64, post_s: i64) -> (i64, Option<i64>) {
        if self.kind.is_record() {
            (self.start_unix - pre_s, self.end_unix().map(|e| e + post_s))
        } else {
            (self.start_unix, Some(self.start_unix + 1))
        }
    }

    pub fn overlaps(&self, other: &Timer, pre_s: i64, post_s: i64) -> bool {
        let (a0, a1) = self.window(pre_s, post_s);
        let (b0, b1) = other.window(pre_s, post_s);
        let a_before_b_ends = b1.map(|b1| a0 < b1).unwrap_or(true);
        let b_before_a_ends = a1.map(|a1| b0 < a1).unwrap_or(true);
        a_before_b_ends && b_before_a_ends
    }

    /// Gleicher Dienst auf demselben Kanal (SId oder – bei 0 – Name).
    pub fn same_service(&self, other: &Timer) -> bool {
        let same_channel = self.channel.eq_ignore_ascii_case(&other.channel);
        let same_sid = self.sid != 0 && self.sid == other.sid && self.scids == other.scids;
        let same_name = self.service.trim().eq_ignore_ascii_case(other.service.trim());
        same_channel && (same_sid || same_name)
    }

    /// Anzeigename: Titel, sonst Dienst (wie v1 Spalte "Titel").
    pub fn label(&self) -> &str {
        if self.title.trim().is_empty() {
            self.service.trim()
        } else {
            self.title.trim()
        }
    }

    /// Ist der Timer abgearbeitet oder verpasst (fuer das Ausduennen beim Laden)?
    fn is_stale(&self, now: i64, pre_s: i64, post_s: i64) -> bool {
        match self.window(pre_s, post_s) {
            (_, Some(to)) if self.kind.is_record() => now >= to,
            (from, _) if !self.kind.is_record() => now >= from + SWITCH_GRACE_S,
            // Aufnahme mit offenem Ende: nur behalten, solange sie nicht gefeuert hat
            _ => self.fired,
        }
    }
}

/// Konflikt beim Anlegen/Aendern eines Timers.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(tag = "reason", rename_all = "snake_case")]
pub enum Conflict {
    /// Startzeit liegt nicht in der Zukunft (v1: "Startzeit muss in der Zukunft liegen").
    Past,
    /// Zeitfenster ueberschneidet einen aktiven Timer auf einem anderen Dienst/Kanal.
    Overlap { other: Timer },
    /// Fenster beginnt sofort, aber eine Aufnahme laeuft bereits.
    RecordingActive,
}

impl Conflict {
    /// i18n-Schluessel fuer das Frontend (Vertrag `timer_add_from_epg`).
    pub fn key(&self) -> &'static str {
        match self {
            Conflict::Past => "timer.conflict.past",
            Conflict::Overlap { .. } => "timer.conflict.overlap",
            Conflict::RecordingActive => "timer.conflict.recording",
        }
    }
}

/// Ergebnis von `timer_add`/`timer_update`: entweder die Id oder der Konflikt.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct AddOutcome {
    pub id: Option<u32>,
    pub conflict: Option<Conflict>,
}

/// Anfrage des EPG-Panels (Vertrag mit dem EPG-Agenten).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct EpgTimerRequest {
    pub channel: String,
    pub eid: u16,
    pub sid: u32,
    pub service: String,
    pub title: String,
    pub start_unix: i64,
    pub duration_s: u32,
    /// "record" | "switch"
    pub kind: String,
}

/// Was beim Feuern eines Timers passiert ist (Tauri-Event `timer_status`).
#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum TimerFireStatus {
    Switched,
    RecordingStarted,
    RecordingStopped,
    /// Wartet, weil eine Aufnahme laeuft.
    Blocked,
    /// Zeitfenster vorbei, ohne dass der Timer feuern konnte.
    Missed,
    /// Kanal/Dienst nicht gefunden oder kein Geraet.
    Failed,
}

/// Alle Timer mit Id-Zaehler (Datei `timers.json`).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(default)]
pub struct Timers {
    pub version: u32,
    pub id_counter: u32,
    pub timers: Vec<Timer>,
}

impl Default for Timers {
    fn default() -> Self {
        Self { version: TIMERS_VERSION, id_counter: 0, timers: Vec::new() }
    }
}

impl Timers {
    /// Liest v2 oder v1; unlesbare Dateien ergeben eine leere Liste.
    pub fn load(path: &Path) -> Self {
        match std::fs::read_to_string(path) {
            Ok(s) => Self::parse(&s).unwrap_or_else(|e| {
                log::warn!("{}: {e}, Standard verwendet", path.display());
                Self::default()
            }),
            Err(_) => Self::default(),
        }
    }

    /// Erkennt das Format an `version`/`startTime`.
    pub fn parse(json: &str) -> anyhow::Result<Self> {
        let v: serde_json::Value = serde_json::from_str(json)?;
        let is_v1 = v.get("version").and_then(|x| x.as_u64()) == Some(1)
            || v.get("timers").and_then(|t| t.as_array()).map(|a| a.iter().any(|t| t.get("startTime").is_some())).unwrap_or(false)
            || v.get("idCounter").is_some();
        if is_v1 {
            return Self::parse_v1(&v);
        }
        let mut t: Timers = serde_json::from_value(v)?;
        t.version = TIMERS_VERSION;
        t.sort();
        Ok(t)
    }

    /// v1 `.qt-dab-timers.json`: `{version:1, idCounter, timers:[{id, type:int, service, channel,
    /// startTime:"yyyy-MM-ddTHH:mm:ss" (lokal), duration:min, active, fired, title}]}`.
    pub fn parse_v1(v: &serde_json::Value) -> anyhow::Result<Self> {
        let mut out = Timers { id_counter: v.get("idCounter").and_then(|x| x.as_u64()).unwrap_or(0) as u32, ..Default::default() };
        let arr = v.get("timers").and_then(|t| t.as_array()).cloned().unwrap_or_default();
        for obj in arr {
            let kind = match obj.get("type").and_then(|x| x.as_i64()).and_then(TimerKind::from_v1) {
                Some(k) => k,
                None => continue,
            };
            let Some(start) = obj.get("startTime").and_then(|x| x.as_str()).and_then(parse_v1_time) else { continue };
            let id = obj.get("id").and_then(|x| x.as_u64()).unwrap_or(0) as u32;
            let minutes = obj.get("duration").and_then(|x| x.as_i64()).unwrap_or(0).max(0) as u32;
            out.timers.push(Timer {
                id,
                kind,
                active: obj.get("active").and_then(|x| x.as_bool()).unwrap_or(true),
                fired: obj.get("fired").and_then(|x| x.as_bool()).unwrap_or(false),
                channel: obj.get("channel").and_then(|x| x.as_str()).unwrap_or("").trim().to_uppercase(),
                eid: 0,
                sid: 0,
                scids: 0,
                service: obj.get("service").and_then(|x| x.as_str()).unwrap_or("").trim().to_string(),
                title: obj.get("title").and_then(|x| x.as_str()).unwrap_or("").trim().to_string(),
                start_unix: start,
                duration_s: minutes * 60,
            });
            out.id_counter = out.id_counter.max(id);
        }
        out.sort();
        Ok(out)
    }

    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        std::fs::write(path, serde_json::to_string_pretty(self).expect("serialisierbar"))
    }

    fn sort(&mut self) {
        self.timers.sort_by(|a, b| a.start_unix.cmp(&b.start_unix).then(a.id.cmp(&b.id)));
    }

    /// Abgearbeitete und verpasste Timer entfernen (v1 `loadFromFile`).
    pub fn prune(&mut self, now: i64, pre_s: i64, post_s: i64) -> usize {
        let before = self.timers.len();
        self.timers.retain(|t| !t.is_stale(now, pre_s, post_s));
        before - self.timers.len()
    }

    pub fn get(&self, id: u32) -> Option<&Timer> {
        self.timers.iter().find(|t| t.id == id)
    }

    pub fn get_mut(&mut self, id: u32) -> Option<&mut Timer> {
        self.timers.iter_mut().find(|t| t.id == id)
    }

    /// Fuegt ein und vergibt die Id; liefert die Id.
    pub fn add(&mut self, mut timer: Timer) -> u32 {
        self.id_counter += 1;
        timer.id = self.id_counter;
        timer.channel = timer.channel.trim().to_uppercase();
        timer.service = timer.service.trim().to_string();
        timer.title = timer.title.trim().to_string();
        let id = timer.id;
        self.timers.push(timer);
        self.sort();
        id
    }

    /// Ersetzt den Eintrag mit derselben Id.
    pub fn update(&mut self, mut timer: Timer) -> bool {
        let Some(slot) = self.timers.iter_mut().find(|t| t.id == timer.id) else { return false };
        timer.channel = timer.channel.trim().to_uppercase();
        timer.service = timer.service.trim().to_string();
        timer.title = timer.title.trim().to_string();
        *slot = timer;
        self.sort();
        true
    }

    pub fn remove(&mut self, id: u32) -> bool {
        let before = self.timers.len();
        self.timers.retain(|t| t.id != id);
        before != self.timers.len()
    }

    /// Naechster wartender Timer (Statusleiste).
    pub fn next_pending(&self, now: i64) -> Option<&Timer> {
        self.timers.iter().filter(|t| t.active && !t.fired && t.start_unix >= now).min_by_key(|t| t.start_unix)
    }

    /// Konfliktpruefung fuer einen neuen oder geaenderten Timer (`cand.id` wird ignoriert).
    pub fn conflict(&self, cand: &Timer, now: i64, pre_s: i64, post_s: i64, recording_active: bool) -> Option<Conflict> {
        if cand.start_unix <= now {
            return Some(Conflict::Past);
        }
        let (from, _) = cand.window(pre_s, post_s);
        if recording_active && cand.kind.is_record() && from <= now {
            return Some(Conflict::RecordingActive);
        }
        for t in &self.timers {
            if t.id == cand.id || !t.active || t.is_stale(now, pre_s, post_s) {
                continue;
            }
            if !(cand.kind.is_record() || t.kind.is_record()) {
                continue; // zwei Umschalt-Timer stoeren sich nicht
            }
            if t.same_service(cand) {
                continue; // gleicher Dienst: Aufnahmen werden nacheinander gestartet
            }
            if cand.overlaps(t, pre_s, post_s) {
                return Some(Conflict::Overlap { other: t.clone() });
            }
        }
        None
    }
}

/// v1-Zeit: Qt `ISODate` ohne Zone = lokale Zeit; mit Zone (`Z`, `+02:00`) RFC 3339.
pub fn parse_v1_time(s: &str) -> Option<i64> {
    let s = s.trim();
    if let Ok(dt) = DateTime::parse_from_rfc3339(s) {
        return Some(dt.timestamp());
    }
    let naive = NaiveDateTime::parse_from_str(s, "%Y-%m-%dT%H:%M:%S")
        .or_else(|_| NaiveDateTime::parse_from_str(s, "%Y-%m-%dT%H:%M"))
        .ok()?;
    Local.from_local_datetime(&naive).earliest().map(|dt| dt.timestamp())
}

/// Bekannte Ablageorte der v1-Datei: Benutzerprofil, Repo-Wurzel/portable Installation.
pub fn locate_v1() -> Option<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(home) = std::env::var_os("USERPROFILE").or_else(|| std::env::var_os("HOME")) {
        candidates.push(PathBuf::from(&home).join(".qt-dab-timers.json"));
    }
    if let Ok(exe) = std::env::current_exe() {
        let mut dir = exe.parent().map(|p| p.to_path_buf());
        for _ in 0..6 {
            let Some(d) = dir else { break };
            candidates.push(d.join("Qt-DAB-timers.json"));
            candidates.push(d.join(".qt-dab-timers.json"));
            candidates.push(d.join("Qt-DAB-portable").join("data").join(".qt-dab-timers.json"));
            dir = d.parent().map(|p| p.to_path_buf());
        }
    }
    candidates.into_iter().find(|p| p.is_file())
}

// ---------------------------------------------------------------------------
// Scheduler (Teil von App)
// ---------------------------------------------------------------------------

/// Laufender Feuer-Vorgang: Kanal abgestimmt, warten auf Dienst/Audio.
#[derive(Clone, Debug)]
struct TimerRun {
    id: u32,
    channel: String,
    sid: u32,
    scids: u8,
    name: String,
    title: String,
    record: bool,
    stop_at: Option<i64>,
    /// `select_service` gesendet (oder Dienst lief schon), warten auf `service_started`.
    selected: bool,
    deadline: i64,
}

/// Timer-Liste plus Scheduler-Zustand.
#[derive(Debug, Default)]
pub struct Scheduler {
    pub timers: Timers,
    path: PathBuf,
    run: Option<TimerRun>,
    /// Timer, fuer den zuletzt "blockiert durch Aufnahme" gemeldet wurde.
    blocked: Option<u32>,
    /// Anzahl beim Start aus v1 importierter Timer (Hinweis).
    pub imported_v1: usize,
}

impl Scheduler {
    /// Laedt `data/timers.json`; fehlt sie, wird einmalig die v1-Datei importiert.
    pub fn load(dirs: &DataDirs, pre_s: i64, post_s: i64) -> Self {
        let path = dirs.timers_file();
        let mut imported = 0;
        let mut timers = if path.is_file() {
            Timers::load(&path)
        } else if let Some(v1) = locate_v1() {
            match std::fs::read_to_string(&v1).map_err(anyhow::Error::from).and_then(|s| Timers::parse(&s)) {
                Ok(t) => {
                    imported = t.timers.len();
                    log::info!("Timer aus {} importiert: {}", v1.display(), imported);
                    t
                }
                Err(e) => {
                    log::warn!("v1-Timer {}: {e}", v1.display());
                    Timers::default()
                }
            }
        } else {
            Timers::default()
        };
        let now = crate::state::unix_now();
        timers.prune(now, pre_s, post_s);
        // Aufnahmen mit offenem Ende ueberleben keinen Neustart.
        for t in timers.timers.iter_mut().filter(|t| t.fired && t.kind.is_record() && t.duration_s == 0) {
            t.active = false;
        }
        let s = Self { timers, path, run: None, blocked: None, imported_v1: imported };
        if imported > 0 || (!s.path.is_file() && dirs.root.is_dir()) {
            if let Err(e) = s.timers.save(&s.path) {
                log::warn!("timers.json: {e}");
            }
        }
        s
    }

    pub fn with_timers(timers: Timers, path: PathBuf) -> Self {
        Self { timers, path, ..Default::default() }
    }

    pub fn is_running(&self) -> bool {
        self.run.is_some()
    }

    fn save(&self) {
        if self.path.as_os_str().is_empty() {
            return;
        }
        if let Err(e) = self.timers.save(&self.path) {
            log::warn!("timers.json: {e}");
        }
    }
}

impl App {
    fn pre_post(&self) -> (i64, i64) {
        (self.settings.record_pre_s as i64, self.settings.record_post_s as i64)
    }

    fn timers_changed(&self) -> Effects {
        self.sched.save();
        let mut fx = Effects::default();
        fx.events.push(AppEvent::TimersChanged { timers: self.sched.timers.clone() });
        fx
    }

    fn timer_status(&self, t: &Timer, status: TimerFireStatus) -> AppEvent {
        AppEvent::TimerStatus { id: t.id, kind: t.kind, service: t.service.clone(), title: t.title.clone(), status }
    }

    /// Timer anlegen. `force` = Ueberschneidung akzeptieren (v1 fragt "Bestehenden Timer ersetzen?";
    /// hier entscheidet das Frontend, ob es den anderen loescht).
    pub fn timer_add(&mut self, timer: Timer, force: bool, now: i64) -> Result<(AddOutcome, Effects), AppError> {
        let (pre, post) = self.pre_post();
        if timer.service.trim().is_empty() {
            return Err(AppError::NoService);
        }
        let mut cand = timer;
        cand.id = 0;
        cand.active = true;
        cand.fired = false;
        if let Some(c) = self.sched.timers.conflict(&cand, now, pre, post, self.state.recording) {
            if !force || c == Conflict::Past {
                return Ok((AddOutcome { id: None, conflict: Some(c) }, Effects::default()));
            }
        }
        let id = self.sched.timers.add(cand);
        Ok((AddOutcome { id: Some(id), conflict: None }, self.timers_changed()))
    }

    /// Anfrage aus dem EPG-Panel (Vertrag): Fehler als i18n-Schluessel.
    pub fn timer_add_from_epg(&mut self, req: EpgTimerRequest, now: i64) -> Result<(u32, Effects), AppError> {
        let kind = match req.kind.as_str() {
            "record" => TimerKind::EpgRecord,
            "switch" => TimerKind::EpgSwitch,
            other => return Err(AppError::Other(format!("timer.error.kind:{other}"))),
        };
        let timer = Timer {
            id: 0,
            kind,
            active: true,
            fired: false,
            channel: req.channel,
            eid: req.eid,
            sid: req.sid,
            scids: 0,
            service: req.service,
            title: req.title,
            start_unix: req.start_unix,
            duration_s: req.duration_s,
        };
        let (out, fx) = self.timer_add(timer, false, now)?;
        match (out.id, out.conflict) {
            (Some(id), _) => Ok((id, fx)),
            (None, Some(c)) => Err(AppError::Other(c.key().to_string())),
            (None, None) => Err(AppError::Other("timer.error.unknown".into())),
        }
    }

    pub fn timer_update(&mut self, timer: Timer, force: bool, now: i64) -> Result<(AddOutcome, Effects), AppError> {
        let (pre, post) = self.pre_post();
        if self.sched.timers.get(timer.id).is_none() {
            return Err(AppError::Other("timer not found".into()));
        }
        if timer.service.trim().is_empty() {
            return Err(AppError::NoService);
        }
        if timer.active && !timer.fired {
            if let Some(c) = self.sched.timers.conflict(&timer, now, pre, post, self.state.recording) {
                if !force || c == Conflict::Past {
                    return Ok((AddOutcome { id: None, conflict: Some(c) }, Effects::default()));
                }
            }
        }
        let id = timer.id;
        self.sched.timers.update(timer);
        Ok((AddOutcome { id: Some(id), conflict: None }, self.timers_changed()))
    }

    /// Loeschen; eine vom Timer gestartete Aufnahme wird beendet.
    pub fn timer_delete(&mut self, id: u32) -> Result<Effects, AppError> {
        if !self.sched.timers.remove(id) {
            return Err(AppError::Other("timer not found".into()));
        }
        let mut fx = Effects::default();
        if self.sched.run.as_ref().map(|r| r.id == id).unwrap_or(false) {
            self.sched.run = None;
        }
        if self.rec.info.timer_id == Some(id) && self.rec.info.active {
            fx.append(self.recording_stop().unwrap_or_default());
        }
        fx.append(self.timers_changed());
        Ok(fx)
    }

    pub fn timer_toggle_active(&mut self, id: u32) -> Result<Effects, AppError> {
        let t = self.sched.timers.get_mut(id).ok_or_else(|| AppError::Other("timer not found".into()))?;
        t.active = !t.active;
        if t.active {
            // Reaktivieren: neu warten, falls die Startzeit noch aussteht
            t.fired = false;
        }
        let stop = !t.active && self.rec.info.timer_id == Some(id) && self.rec.info.active;
        let mut fx = Effects::default();
        if stop {
            fx.append(self.recording_stop().unwrap_or_default());
        }
        if self.sched.run.as_ref().map(|r| r.id == id).unwrap_or(false) {
            self.sched.run = None;
        }
        fx.append(self.timers_changed());
        Ok(fx)
    }

    /// Sekundentakt fuer Timer, Aufnahme-Nachlauf und Sleep-Timer (aus [`App::tick`]).
    pub fn timer_tick_all(&mut self, now: i64) -> Effects {
        let mut fx = self.scheduler_tick(now);
        fx.append(self.recording_tick(now));
        fx.append(self.sleep_tick(now));
        fx
    }

    /// Scheduler: Aufnahmen beenden, Verpasste aussortieren, faellige Timer feuern.
    pub fn scheduler_tick(&mut self, now: i64) -> Effects {
        let (pre, post) = self.pre_post();
        let mut fx = Effects::default();
        let mut changed = false;

        // 1. Aufnahme-Timer, deren Nachlauf abgelaufen ist
        let to_stop: Vec<Timer> = self
            .sched
            .timers
            .timers
            .iter()
            .filter(|t| t.active && t.fired && t.kind.is_record())
            .filter(|t| t.window(pre, post).1.map(|to| now >= to).unwrap_or(false))
            .cloned()
            .collect();
        for t in to_stop {
            if self.rec.info.timer_id == Some(t.id) && (self.rec.info.active || self.state.recording) {
                fx.append(self.recording_stop().unwrap_or_default());
                fx.events.push(self.timer_status(&t, TimerFireStatus::RecordingStopped));
            }
            if let Some(x) = self.sched.timers.get_mut(t.id) {
                x.active = false;
            }
            if self.sched.run.as_ref().map(|r| r.id == t.id).unwrap_or(false) {
                self.sched.run = None;
            }
            changed = true;
        }

        // 2. Verpasste Timer
        let missed: Vec<Timer> = self
            .sched
            .timers
            .timers
            .iter()
            .filter(|t| t.active && !t.fired && t.is_stale(now, pre, post))
            .cloned()
            .collect();
        for t in missed {
            if let Some(x) = self.sched.timers.get_mut(t.id) {
                x.active = false;
                x.fired = true;
            }
            fx.events.push(self.timer_status(&t, TimerFireStatus::Missed));
            changed = true;
        }

        // 3. Laufender Feuer-Vorgang: Zeitueberschreitung?
        if let Some(run) = self.sched.run.clone() {
            if now >= run.deadline {
                fx.append(self.run_failed(&run));
                changed = true;
            }
            if changed {
                fx.append(self.timers_changed());
            }
            return fx;
        }

        // 4. Naechster faelliger Timer
        let due = self
            .sched
            .timers
            .timers
            .iter()
            .filter(|t| t.active && !t.fired && t.window(pre, post).0 <= now)
            .min_by_key(|t| t.start_unix)
            .cloned();
        if let Some(t) = due {
            let chain = self.rec.info.timer_id.is_some()
                && self.rec.info.active
                && self.sched.timers.get(self.rec.info.timer_id.unwrap_or(0)).map(|cur| cur.same_service(&t)).unwrap_or(false);
            if self.state.recording && !chain {
                if self.sched.blocked != Some(t.id) {
                    self.sched.blocked = Some(t.id);
                    fx.events.push(self.timer_status(&t, TimerFireStatus::Blocked));
                }
            } else if self.state.device.is_none() {
                if let Some(x) = self.sched.timers.get_mut(t.id) {
                    x.active = false;
                    x.fired = true;
                }
                fx.events.push(self.timer_status(&t, TimerFireStatus::Failed));
                fx.events.push(AppEvent::Notice { level: NoticeLevel::Warn, text: format!("timer {}: no device", t.label()) });
                changed = true;
            } else {
                self.sched.blocked = None;
                fx.append(self.run_start(&t, now, post));
                changed = true;
            }
        }

        if changed {
            fx.append(self.timers_changed());
        }
        fx
    }

    /// Kanal abstimmen und den Feuer-Vorgang beginnen.
    fn run_start(&mut self, t: &Timer, now: i64, post: i64) -> Effects {
        let mut fx = Effects::default();
        let channel = if t.channel.is_empty() { self.state.channel.clone().unwrap_or_default() } else { t.channel.clone() };
        let same_channel = self.state.is_file_source()
            || self.state.channel.as_deref().map(|c| c.eq_ignore_ascii_case(&channel)).unwrap_or(false);
        if !same_channel && !channel.is_empty() {
            fx.append(self.set_channel(&channel));
        }
        if let Some(x) = self.sched.timers.get_mut(t.id) {
            x.fired = true;
        }
        self.sched.run = Some(TimerRun {
            id: t.id,
            channel,
            sid: t.sid,
            scids: t.scids,
            name: t.service.clone(),
            title: t.title.clone(),
            record: t.kind.is_record(),
            stop_at: t.end_unix().map(|e| e + post),
            selected: false,
            deadline: now + FIRE_TIMEOUT_S,
        });
        if same_channel {
            fx.append(self.run_try_select());
        }
        fx
    }

    /// Dienst in der Liste suchen; gefunden: waehlen (oder schon aktiv: fertig).
    fn run_try_select(&mut self) -> Effects {
        let Some(run) = self.sched.run.clone() else { return Effects::default() };
        if run.selected {
            return Effects::default();
        }
        let svc = if run.sid != 0 {
            self.state.service(run.sid, run.scids).cloned()
        } else {
            self.state.service_by_name(&run.name).cloned()
        };
        let Some(svc) = svc else { return Effects::default() };
        let already = self.state.current.as_ref().map(|c| c.sid == svc.sid && c.scids == svc.scids).unwrap_or(false);
        if let Some(r) = self.sched.run.as_mut() {
            r.sid = svc.sid;
            r.scids = svc.scids;
            r.name = svc.name.trim().to_string();
            r.selected = true;
        }
        // SId/Name aus dem Empfang nachtragen (v1-Import ohne SId)
        if let Some(x) = self.sched.timers.get_mut(run.id) {
            x.sid = svc.sid;
            x.scids = svc.scids;
            if let Some(e) = &self.state.ensemble {
                x.eid = e.eid;
            }
        }
        if already {
            return self.run_service_ready();
        }
        let mut fx = Effects::default();
        self.pending_clear();
        fx.commands.push(Command::SelectService { sid: svc.sid, scids: svc.scids, slot: ServiceSlot::Primary });
        fx
    }

    /// Dienst laeuft: umschalten fertig bzw. Aufnahme starten.
    fn run_service_ready(&mut self) -> Effects {
        let Some(run) = self.sched.run.take() else { return Effects::default() };
        let mut fx = Effects::default();
        let Some(t) = self.sched.timers.get(run.id).cloned() else { return fx };
        if run.record {
            let title = if run.title.trim().is_empty() { None } else { Some(run.title.as_str()) };
            match self.recording_start(title, Some((run.id, run.stop_at))) {
                Ok(f) => {
                    fx.append(f);
                    fx.events.push(self.timer_status(&t, TimerFireStatus::RecordingStarted));
                }
                Err(e) => {
                    if let Some(x) = self.sched.timers.get_mut(run.id) {
                        x.active = false;
                    }
                    fx.events.push(self.timer_status(&t, TimerFireStatus::Failed));
                    fx.events.push(AppEvent::Notice { level: NoticeLevel::Warn, text: format!("timer {}: {e}", t.label()) });
                }
            }
        } else {
            if let Some(x) = self.sched.timers.get_mut(run.id) {
                x.active = false;
            }
            fx.events.push(self.timer_status(&t, TimerFireStatus::Switched));
        }
        fx.append(self.timers_changed());
        fx
    }

    fn run_failed(&mut self, run: &TimerRun) -> Effects {
        self.sched.run = None;
        let mut fx = Effects::default();
        if let Some(x) = self.sched.timers.get_mut(run.id) {
            x.active = false;
            x.fired = true;
            let t = x.clone();
            fx.events.push(self.timer_status(&t, TimerFireStatus::Failed));
        }
        fx
    }

    /// Kern-Ereignisse fuer den laufenden Feuer-Vorgang.
    pub fn sched_on_event(&mut self, ev: &Event) -> Effects {
        let Some(run) = self.sched.run.clone() else { return Effects::default() };
        match ev {
            Event::ServiceAdded { service } if !run.selected => {
                let hit = if run.sid != 0 {
                    service.sid == run.sid && service.scids == run.scids
                } else {
                    service.name.trim().eq_ignore_ascii_case(run.name.trim())
                };
                if hit {
                    return self.run_try_select();
                }
                Effects::default()
            }
            Event::ServiceStarted { slot: ServiceSlot::Primary, sid, scids, .. } if run.selected && *sid == run.sid && *scids == run.scids => {
                self.run_service_ready()
            }
            Event::NoSignal { channel } if channel.eq_ignore_ascii_case(&run.channel) && !self.state.is_file_source() => {
                let mut fx = self.run_failed(&run);
                fx.append(self.timers_changed());
                fx
            }
            Event::Exiting { .. } | Event::DeviceClosed => {
                let mut fx = self.run_failed(&run);
                fx.append(self.timers_changed());
                fx
            }
            _ => Effects::default(),
        }
    }

    /// Naechster wartender Timer (fuer Statusleiste/Tests).
    pub fn next_timer(&self, now: i64) -> Option<Timer> {
        self.sched.timers.next_pending(now).cloned()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::recording::RecordingInfo;
    use crate::{Presets, Settings};
    use dab_api::{Codec, RecFormat, ServiceInfo};
    use std::time::Instant;

    fn tmp(name: &str) -> PathBuf {
        let nanos = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        std::env::temp_dir().join(format!("dabclassic-timer-{name}-{}-{nanos}", std::process::id()))
    }

    fn app() -> App {
        let dir = tmp("app");
        std::fs::create_dir_all(&dir).unwrap();
        let mut a = App::with(DataDirs::with_root(&dir, true), Settings::default(), Presets::default());
        a.state.device = Some(crate::state::DeviceState { kind: "hackrf".into(), ..Default::default() });
        a
    }

    fn svc(sid: u32, name: &str) -> ServiceInfo {
        ServiceInfo { sid, scids: 0, name: name.into(), is_audio: true, is_primary: true, sub_ch: 1, bitrate_kbps: 96, pty: 0 }
    }

    fn tune(a: &mut App, channel: &str, eid: u16, services: &[(u32, &str)]) {
        let now = Instant::now();
        a.handle_event(&Event::EnsembleFound { eid, name: "Ens".into(), channel: channel.into() }, now);
        for (sid, name) in services {
            a.handle_event(&Event::ServiceAdded { service: svc(*sid, name) }, now);
        }
    }

    fn started(a: &mut App, sid: u32) -> Effects {
        a.handle_event(
            &Event::ServiceStarted { slot: ServiceSlot::Primary, sid, scids: 0, codec: Codec::HeAac { sbr: true, ps: false, sample_rate: 48000 }, stereo: true },
            Instant::now(),
        )
    }

    fn timer(kind: TimerKind, channel: &str, sid: u32, service: &str, start: i64, dur: u32) -> Timer {
        Timer {
            id: 0,
            kind,
            active: true,
            fired: false,
            channel: channel.into(),
            eid: 0x10BC,
            sid,
            scids: 0,
            service: service.into(),
            title: String::new(),
            start_unix: start,
            duration_s: dur,
        }
    }

    const T0: i64 = 1_800_000_000; // weit in der Zukunft

    #[test]
    fn json_roundtrip_v2() {
        let mut ts = Timers::default();
        let mut t = timer(TimerKind::EpgRecord, "5C", 0xD210, "Dlf", T0, 3600);
        t.title = "Informationen am Mittag".into();
        ts.add(t);
        ts.add(timer(TimerKind::ManualSwitch, "11D", 0, "WDR 5", T0 - 10, 0));
        let path = tmp("rt").join("timers.json");
        ts.save(&path).unwrap();
        let json = std::fs::read_to_string(&path).unwrap();
        assert!(json.contains("\"type\": \"epg_record\""));
        assert!(json.contains("\"start_unix\""));
        let back = Timers::load(&path);
        assert_eq!(back, ts);
        assert_eq!(back.timers[0].service, "WDR 5", "nach Startzeit sortiert");
        assert_eq!(back.id_counter, 2);
        let _ = std::fs::remove_dir_all(path.parent().unwrap());
    }

    #[test]
    fn v1_import_local_time_and_units() {
        let v1 = r#"{
    "idCounter": 11,
    "timers": [
        { "active": true, "channel": "5C", "duration": 0, "fired": false, "id": 10,
          "service": "Dlf             ", "startTime": "2031-04-14T16:37:00", "title": "Forschung aktuell", "type": 2 },
        { "active": true, "channel": "", "duration": 45, "fired": false, "id": 11,
          "service": "WDR 5           ", "startTime": "2031-04-14T18:06:00", "title": "Politikum", "type": 3 },
        { "active": true, "channel": "5C", "duration": 0, "fired": false, "id": 3,
          "service": "Alt", "startTime": "2020-01-01T10:00:00", "type": 0 }
    ],
    "version": 1
}"#;
        let mut ts = Timers::parse(v1).unwrap();
        assert_eq!(ts.version, TIMERS_VERSION);
        assert_eq!(ts.id_counter, 11);
        assert_eq!(ts.timers.len(), 3);
        let dlf = ts.get(10).unwrap();
        assert_eq!(dlf.kind, TimerKind::EpgSwitch);
        assert_eq!(dlf.service, "Dlf", "Name getrimmt");
        assert_eq!(dlf.sid, 0, "SId unbekannt -> Namensaufloesung");
        let expected = Local.with_ymd_and_hms(2031, 4, 14, 16, 37, 0).unwrap().timestamp();
        assert_eq!(dlf.start_unix, expected, "lokale Zeit");
        let wdr = ts.get(11).unwrap();
        assert_eq!(wdr.kind, TimerKind::EpgRecord);
        assert_eq!(wdr.duration_s, 45 * 60, "Minuten -> Sekunden");
        assert_eq!(wdr.channel, "", "leerer Kanal = aktueller Kanal");
        // Ausduennen wie v1 loadFromFile: Vergangenes fliegt raus
        let removed = ts.prune(crate::state::unix_now(), 120, 300);
        assert_eq!(removed, 1);
        assert!(ts.get(3).is_none());
        // Zeit mit Zone
        assert_eq!(parse_v1_time("2031-04-14T16:37:00Z"), Some(DateTime::parse_from_rfc3339("2031-04-14T16:37:00Z").unwrap().timestamp()));
    }

    #[test]
    fn conflicts() {
        let mut ts = Timers::default();
        ts.add(timer(TimerKind::EpgRecord, "5C", 0xD210, "Dlf", T0, 3600));
        ts.add(timer(TimerKind::ManualSwitch, "5C", 0xD220, "Dlf Kultur", T0 + 7200, 0));
        let (pre, post) = (120, 300);
        let now = T0 - 10_000;
        // Vergangenheit
        assert_eq!(ts.conflict(&timer(TimerKind::ManualRecord, "5C", 1, "X", now - 1, 60), now, pre, post, false), Some(Conflict::Past));
        // Ueberschneidung anderer Dienst (Nachlauf des ersten reicht bis T0+3900, Vorlauf des neuen ab T0+3780)
        let c = ts.conflict(&timer(TimerKind::EpgRecord, "11D", 0xE1C0, "WDR 5", T0 + 3900, 600), now, pre, post, false);
        assert!(matches!(c, Some(Conflict::Overlap { ref other }) if other.service == "Dlf"), "{c:?}");
        // Gleicher Dienst direkt danach: kein Konflikt
        assert_eq!(ts.conflict(&timer(TimerKind::EpgRecord, "5C", 0xD210, "Dlf", T0 + 3600, 600), now, pre, post, false), None);
        // Umschalt-Timer in einem Aufnahmefenster anderen Dienstes: Konflikt
        assert!(matches!(ts.conflict(&timer(TimerKind::EpgSwitch, "5C", 0xD220, "Dlf Kultur", T0 + 60, 0), now, pre, post, false), Some(Conflict::Overlap { .. })));
        // Zwei Umschalt-Timer: kein Konflikt
        assert_eq!(ts.conflict(&timer(TimerKind::ManualSwitch, "5C", 5, "Y", T0 + 7200, 0), now, pre, post, false), None);
        // Nach dem Fenster: kein Konflikt
        assert_eq!(ts.conflict(&timer(TimerKind::ManualRecord, "11D", 6, "Z", T0 + 4100, 60), now, pre, post, false), None);
        // Laufende Aufnahme, Fenster beginnt sofort (Vorlauf)
        assert_eq!(ts.conflict(&timer(TimerKind::ManualRecord, "11D", 6, "Z", now + 60, 60), now, pre, post, true), Some(Conflict::RecordingActive));
        assert_eq!(ts.conflict(&timer(TimerKind::ManualRecord, "11D", 6, "Z", now + 600, 60), now, pre, post, true), None);
        // Inaktive Timer zaehlen nicht
        ts.timers[0].active = false;
        assert_eq!(ts.conflict(&timer(TimerKind::EpgRecord, "11D", 0xE1C0, "WDR 5", T0 + 3900, 600), now, pre, post, false), None);
    }

    #[test]
    fn due_window_with_pre_and_post() {
        let t = timer(TimerKind::EpgRecord, "5C", 0xD210, "Dlf", T0, 1800);
        assert_eq!(t.window(120, 300), (T0 - 120, Some(T0 + 1800 + 300)));
        let s = timer(TimerKind::EpgSwitch, "5C", 0xD210, "Dlf", T0, 0);
        assert_eq!(s.window(120, 300), (T0, Some(T0 + 1)));
        let open = timer(TimerKind::ManualRecord, "5C", 0xD210, "Dlf", T0, 0);
        assert_eq!(open.window(120, 300), (T0 - 120, None));
        assert!(!t.is_stale(T0 + 2099, 120, 300));
        assert!(t.is_stale(T0 + 2100, 120, 300));
        assert!(!s.is_stale(T0 + 299, 120, 300));
        assert!(s.is_stale(T0 + 300, 120, 300));
    }

    #[test]
    fn switch_timer_fires_over_effects() {
        let mut a = app();
        a.state.channel = Some("5C".into());
        tune(&mut a, "5C", 0x10BC, &[(0xD210, "Dlf")]);
        let (out, fx) = a.timer_add(timer(TimerKind::ManualSwitch, "11D", 0xE1C0, "WDR 5", T0, 0), false, T0 - 100).unwrap();
        let id = out.id.unwrap();
        assert!(matches!(fx.events[0], AppEvent::TimersChanged { .. }));
        assert!(a.dirs.timers_file().is_file(), "sofort gespeichert");
        // Noch nicht faellig
        assert!(a.scheduler_tick(T0 - 1).commands.is_empty());
        // Faellig: Gain-Standard + Kanalwechsel
        let fx = a.scheduler_tick(T0);
        assert_eq!(fx.commands, vec![Command::SetGain { gain: crate::DEFAULT_HACKRF_GAIN }, Command::SetChannel { channel: "11D".into() }]);
        assert!(a.sched.timers.get(id).unwrap().fired);
        assert!(a.sched.is_running());
        // Dienst erscheint -> select_service
        let fx = a.handle_event(&Event::EnsembleFound { eid: 0x1E1C, name: "WDR".into(), channel: "11D".into() }, Instant::now());
        assert!(fx.commands.is_empty());
        let fx = a.handle_event(&Event::ServiceAdded { service: svc(0xE1C0, "WDR 5") }, Instant::now());
        assert_eq!(fx.commands, vec![Command::SelectService { sid: 0xE1C0, scids: 0, slot: ServiceSlot::Primary }]);
        // Audio laeuft -> erledigt, einmaliger Timer deaktiviert
        let fx = started(&mut a, 0xE1C0);
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::TimerStatus { status: TimerFireStatus::Switched, .. })));
        let t = a.sched.timers.get(id).unwrap();
        assert!(t.fired && !t.active);
        assert!(!a.sched.is_running());
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn record_timer_starts_and_stops_recording() {
        let mut a = app();
        a.state.channel = Some("5C".into());
        tune(&mut a, "5C", 0x10BC, &[(0xD210, "Dlf"), (0xD220, "Dlf Kultur")]);
        started(&mut a, 0xD220);
        let mut t = timer(TimerKind::EpgRecord, "5C", 0xD210, "Dlf", T0, 600);
        t.title = "Informationen am Mittag".into();
        let (out, _) = a.timer_add(t, false, T0 - 1000).unwrap();
        let id = out.id.unwrap();
        // Vorlauf 120 s: bei T0-121 nichts, bei T0-120 select_service (gleicher Kanal)
        assert!(a.scheduler_tick(T0 - 121).commands.is_empty());
        let fx = a.scheduler_tick(T0 - 120);
        assert_eq!(fx.commands, vec![Command::SelectService { sid: 0xD210, scids: 0, slot: ServiceSlot::Primary }]);
        // Audio da -> start_recording mit v1-Dateinamen
        let fx = started(&mut a, 0xD210);
        let start = fx.commands.iter().find_map(|c| match c {
            Command::StartRecording { path, format, slot, .. } => Some((path.clone(), format.clone(), *slot)),
            _ => None,
        });
        let (path, format, slot) = start.expect("start_recording");
        assert_eq!(format, RecFormat::Wav);
        assert_eq!(slot, ServiceSlot::Primary);
        let name = path.file_name().unwrap().to_string_lossy().to_string();
        assert!(name.ends_with("_Dlf______________Informationen_am_Mittag.wav"), "{name}");
        assert_eq!(path.parent().unwrap(), a.dirs.recordings_dir());
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::TimerStatus { status: TimerFireStatus::RecordingStarted, .. })));
        assert!(a.state.recording, "Umschaltsperre greift sofort");
        assert_eq!(a.rec.info.timer_id, Some(id));
        // Kern bestaetigt
        a.handle_event(&Event::RecordingState { slot: ServiceSlot::Primary, sid: 0xD210, active: true, path: Some(path.clone()), bytes: 1000, seconds: 1.0 }, Instant::now());
        assert!(a.rec.info.active);
        assert_eq!(a.rec.info.bytes, 1000);
        // Umschalten waehrend der Aufnahme ist gesperrt
        assert_eq!(a.select_service(0xD220, 0).unwrap_err(), AppError::Recording);
        assert_eq!(a.command(Command::SetChannel { channel: "11D".into() }).unwrap_err(), AppError::Recording);
        // Nachlauf 300 s: bei T0+600+299 nichts, dann stop_recording
        assert!(a.scheduler_tick(T0 + 899).commands.is_empty());
        let fx = a.scheduler_tick(T0 + 900);
        assert_eq!(fx.commands, vec![Command::StopRecording { slot: ServiceSlot::Primary, sid: None }]);
        assert!(!a.sched.timers.get(id).unwrap().active);
        a.handle_event(&Event::RecordingState { slot: ServiceSlot::Primary, sid: 0xD210, active: false, path: Some(path), bytes: 5000, seconds: 900.0 }, Instant::now());
        assert!(!a.rec.info.active && !a.state.recording);
        assert_eq!(a.rec.info.timer_id, None);
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn timer_waits_while_manual_recording_runs() {
        let mut a = app();
        a.state.channel = Some("5C".into());
        tune(&mut a, "5C", 0x10BC, &[(0xD210, "Dlf"), (0xD220, "Dlf Kultur")]);
        started(&mut a, 0xD220);
        // Manuelle Aufnahme laeuft
        let fx = a.recording_start(None, None).unwrap();
        assert!(matches!(fx.commands[0], Command::StartRecording { .. }));
        a.handle_event(&Event::RecordingState { slot: ServiceSlot::Primary, sid: 0xD220, active: true, path: None, bytes: 0, seconds: 0.0 }, Instant::now());
        let (out, _) = a.timer_add(timer(TimerKind::ManualSwitch, "5C", 0xD210, "Dlf", T0, 0), false, T0 - 100).unwrap();
        let id = out.id.unwrap();
        // Faellig, aber blockiert: einmalige Meldung, nichts gesendet
        let fx = a.scheduler_tick(T0);
        assert!(fx.commands.is_empty());
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::TimerStatus { status: TimerFireStatus::Blocked, .. })));
        let fx = a.scheduler_tick(T0 + 1);
        assert!(fx.events.is_empty(), "keine Wiederholung der Meldung");
        assert!(!a.sched.timers.get(id).unwrap().fired);
        // Aufnahme endet -> Timer feuert nach
        a.recording_stop().unwrap();
        a.handle_event(&Event::RecordingState { slot: ServiceSlot::Primary, sid: 0xD220, active: false, path: None, bytes: 10, seconds: 3.0 }, Instant::now());
        let fx = a.scheduler_tick(T0 + 2);
        assert_eq!(fx.commands, vec![Command::SelectService { sid: 0xD210, scids: 0, slot: ServiceSlot::Primary }]);
        // Nach der Gnadenfrist waere er verpasst
        let mut b = app();
        b.state.channel = Some("5C".into());
        b.timer_add(timer(TimerKind::ManualSwitch, "5C", 0xD210, "Dlf", T0, 0), false, T0 - 100).unwrap();
        let fx = b.scheduler_tick(T0 + SWITCH_GRACE_S);
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::TimerStatus { status: TimerFireStatus::Missed, .. })));
        assert!(fx.commands.is_empty());
        let _ = std::fs::remove_dir_all(&a.dirs.root);
        let _ = std::fs::remove_dir_all(&b.dirs.root);
    }

    #[test]
    fn epg_contract_and_errors() {
        let mut a = app();
        let req = EpgTimerRequest {
            channel: "5C".into(),
            eid: 0x10BC,
            sid: 0xD210,
            service: "Dlf".into(),
            title: "Kalenderblatt".into(),
            start_unix: T0,
            duration_s: 300,
            kind: "record".into(),
        };
        let (id, _) = a.timer_add_from_epg(req.clone(), T0 - 100).unwrap();
        assert_eq!(a.sched.timers.get(id).unwrap().kind, TimerKind::EpgRecord);
        // Ueberschneidung anderer Dienst -> Schluessel
        let mut r2 = req.clone();
        r2.sid = 0xD220;
        r2.service = "Dlf Kultur".into();
        assert_eq!(a.timer_add_from_epg(r2, T0 - 100).unwrap_err().to_string(), "timer.conflict.overlap");
        let mut r3 = req.clone();
        r3.start_unix = T0 - 200;
        r3.kind = "switch".into();
        assert_eq!(a.timer_add_from_epg(r3, T0 - 100).unwrap_err().to_string(), "timer.conflict.past");
        // Loeschen und Umschalten aktiv/inaktiv
        a.timer_toggle_active(id).unwrap();
        assert!(!a.sched.timers.get(id).unwrap().active);
        a.timer_delete(id).unwrap();
        assert!(a.sched.timers.get(id).is_none());
        assert_eq!(a.timer_delete(id).unwrap_err().to_string(), "timer not found");
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn timer_delete_stops_its_recording() {
        let mut a = app();
        a.rec.info = RecordingInfo { active: true, timer_id: Some(7), ..Default::default() };
        a.state.recording = true;
        a.sched.timers.timers.push(timer(TimerKind::ManualRecord, "5C", 1, "X", T0, 60));
        a.sched.timers.timers[0].id = 7;
        let fx = a.timer_delete(7).unwrap();
        assert_eq!(fx.commands, vec![Command::StopRecording { slot: ServiceSlot::Primary, sid: None }]);
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }
}
