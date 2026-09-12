//! Senderliste ueber alle Ensembles (`data/stations.json`): alle Dienste, die
//! ein Band-III-Scan gefunden hat oder die beim normalen Hoeren gemeldet
//! wurden – anders als `AppState::services` (nur das gerade abgestimmte
//! Ensemble). Quellen:
//!
//! * `scan_result` ersetzt die Eintraege des gemeldeten Kanals (kein Signal:
//!   Eintraege des Kanals weg);
//! * beim Hoeren: nach einer `service_added`-Serie (Ruhe [`STATIONS_SETTLE`])
//!   werden die Dienste des Kanals aktualisiert/ergaenzt (ein unvollstaendig
//!   dekodierter FIC loescht nichts; ein anderes Ensemble auf dem Kanal
//!   ersetzt die alten Eintraege); andere Kanaele bleiben.
//!
//! Umschalten (`tune_station`) laeuft ueber dieselbe Zustandsmaschine wie der
//! Preset-Aufruf ([`App::tune_to`]); die Umschaltsperre bei Aufnahme gilt.

use crate::app::{App, AppError, AppEvent, Effects, StoreResult};
use crate::state::unix_now;
use crate::Preset;
use dab_api::{Event, ServiceInfo};
use serde::{Deserialize, Serialize};
use std::path::Path;
use std::time::{Duration, Instant};

/// Ruhezeit nach dem letzten `service_added`/`ensemble_found`, bis die
/// Dienstliste des Kanals in die Senderliste uebernommen wird.
pub const STATIONS_SETTLE: Duration = Duration::from_secs(2);

/// Ein Dienst der Senderliste.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct StationEntry {
    /// Kanal, z. B. "5C" (Grossbuchstaben).
    pub channel: String,
    pub eid: u16,
    pub ensemble: String,
    pub sid: u32,
    #[serde(default)]
    pub scids: u8,
    pub name: String,
    pub is_audio: bool,
    #[serde(default)]
    pub bitrate_kbps: u16,
    #[serde(default)]
    pub pty: u8,
    /// Unix-Zeit (UTC) der letzten Meldung (Scan oder Empfang).
    #[serde(default)]
    pub last_seen_unix: i64,
}

impl StationEntry {
    pub fn from_service(channel: &str, eid: u16, ensemble: &str, s: &ServiceInfo, now: i64) -> Self {
        Self {
            channel: channel.trim().to_uppercase(),
            eid,
            ensemble: ensemble.trim().to_string(),
            sid: s.sid,
            scids: s.scids,
            name: s.name.trim().to_string(),
            is_audio: s.is_audio,
            bitrate_kbps: s.bitrate_kbps,
            pty: s.pty,
            last_seen_unix: now,
        }
    }

    /// Gleicher Dienst (ohne Zeitstempel)?
    fn same_content(&self, o: &StationEntry) -> bool {
        self.channel == o.channel
            && self.eid == o.eid
            && self.ensemble == o.ensemble
            && self.sid == o.sid
            && self.scids == o.scids
            && self.name == o.name
            && self.is_audio == o.is_audio
            && self.bitrate_kbps == o.bitrate_kbps
            && self.pty == o.pty
    }

    pub fn matches(&self, channel: &str, eid: u16, sid: u32, scids: u8) -> bool {
        self.channel.eq_ignore_ascii_case(channel.trim()) && self.eid == eid && self.sid == sid && self.scids == scids
    }
}

/// Dateiformat `stations.json`.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Stations {
    pub version: u32,
    pub entries: Vec<StationEntry>,
}

impl Default for Stations {
    fn default() -> Self {
        Self { version: 1, entries: Vec::new() }
    }
}

impl Stations {
    pub fn load(path: &Path) -> Self {
        match std::fs::read_to_string(path) {
            Ok(s) => match serde_json::from_str::<Stations>(&s) {
                Ok(mut st) => {
                    sort_stations(&mut st.entries);
                    st
                }
                Err(e) => {
                    log::warn!("stations.json unlesbar ({e}), leer gestartet");
                    Self::default()
                }
            },
            Err(_) => Self::default(),
        }
    }

    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        std::fs::write(path, serde_json::to_string_pretty(self).expect("serialisierbar"))
    }
}

/// Sortierschluessel eines Kanals: Nummer, dann Buchstabe ("5C" < "10A").
pub fn channel_key(ch: &str) -> (u32, String) {
    let ch = ch.trim().to_uppercase();
    let digits: String = ch.chars().take_while(|c| c.is_ascii_digit()).collect();
    let rest = ch[digits.len()..].to_string();
    (digits.parse().unwrap_or(0), rest)
}

/// Kanal numerisch, dann Ensemble, dann Dienstname (jeweils ohne Gross/Klein).
pub fn sort_stations(list: &mut [StationEntry]) {
    list.sort_by(|a, b| {
        channel_key(&a.channel)
            .cmp(&channel_key(&b.channel))
            .then_with(|| a.ensemble.to_lowercase().cmp(&b.ensemble.to_lowercase()))
            .then_with(|| a.eid.cmp(&b.eid))
            .then_with(|| a.name.to_lowercase().cmp(&b.name.to_lowercase()))
            .then_with(|| a.scids.cmp(&b.scids))
    });
}

/// Eintraege eines Kanals ersetzen (`found` = None: Kanal ohne Signal, nur
/// entfernen). Liefert `true`, wenn sich der Inhalt (ohne Zeitstempel)
/// geaendert hat; bei unveraendertem Inhalt wird nur `last_seen_unix` gesetzt.
pub fn replace_channel(list: &mut Vec<StationEntry>, channel: &str, found: Option<(u16, &str, &[ServiceInfo])>, now: i64) -> bool {
    let channel = channel.trim().to_uppercase();
    let mut fresh: Vec<StationEntry> = match found {
        Some((eid, ensemble, services)) => services.iter().map(|s| StationEntry::from_service(&channel, eid, ensemble, s, now)).collect(),
        None => Vec::new(),
    };
    sort_stations(&mut fresh);
    let old: Vec<&StationEntry> = list.iter().filter(|e| e.channel == channel).collect();
    let unchanged = old.len() == fresh.len() && old.iter().zip(fresh.iter()).all(|(a, b)| a.same_content(b));
    if unchanged {
        for e in list.iter_mut().filter(|e| e.channel == channel) {
            e.last_seen_unix = now;
        }
        return false;
    }
    list.retain(|e| e.channel != channel);
    list.extend(fresh);
    sort_stations(list);
    true
}

/// Dienste eines gehoerten Ensembles aktualisieren/ergaenzen: Eintraege des
/// Kanals mit anderer EId verschwinden, bekannte Dienste derselben EId, die
/// (noch) nicht gemeldet sind, bleiben. Liefert `true` bei geaendertem Inhalt.
pub fn merge_channel(list: &mut Vec<StationEntry>, channel: &str, eid: u16, ensemble: &str, services: &[ServiceInfo], now: i64) -> bool {
    let channel = channel.trim().to_uppercase();
    let before = list.len();
    list.retain(|e| e.channel != channel || e.eid == eid);
    let mut changed = list.len() != before;
    for s in services {
        let fresh = StationEntry::from_service(&channel, eid, ensemble, s, now);
        match list.iter_mut().find(|e| e.channel == channel && e.eid == eid && e.sid == s.sid && e.scids == s.scids) {
            Some(e) if e.same_content(&fresh) => e.last_seen_unix = now,
            Some(e) => {
                *e = fresh;
                changed = true;
            }
            None => {
                list.push(fresh);
                changed = true;
            }
        }
    }
    if changed {
        sort_stations(list);
    }
    changed
}

/// Nur Audio- bzw. alle Dienste.
pub fn filtered(list: &[StationEntry], include_data: bool) -> Vec<StationEntry> {
    list.iter().filter(|e| include_data || e.is_audio).cloned().collect()
}

/// Steuerzustand im [`App`]: ausstehende Uebernahme der Dienstliste.
#[derive(Debug, Default)]
pub struct StationsCtl {
    /// Zeitpunkt des letzten `service_added`/`ensemble_found` beim Hoeren.
    pub live_dirty: Option<Instant>,
    /// Zeitstempel noch nicht auf Platte (nur `last_seen_unix` geaendert).
    pub unsaved: bool,
}

impl App {
    pub(crate) fn stations_load(&mut self) {
        self.state.stations = Stations::load(&self.dirs.stations_file()).entries;
    }

    pub(crate) fn stations_save(&mut self) {
        let st = Stations { version: 1, entries: self.state.stations.clone() };
        if let Err(e) = st.save(&self.dirs.stations_file()) {
            log::warn!("stations.json: {e}");
        }
        self.stations_ctl.unsaved = false;
    }

    fn stations_changed(&mut self) -> Effects {
        self.stations_save();
        let mut fx = Effects::default();
        fx.events.push(AppEvent::StationsChanged { stations: self.state.stations.clone() });
        fx
    }

    /// Senderliste (sortiert), `include_data` = false: nur Audiodienste.
    pub fn stations(&self, include_data: bool) -> Vec<StationEntry> {
        filtered(&self.state.stations, include_data)
    }

    pub fn station(&self, channel: &str, eid: u16, sid: u32, scids: u8) -> Option<&StationEntry> {
        self.state.stations.iter().find(|e| e.matches(channel, eid, sid, scids))
    }

    pub fn stations_clear(&mut self) -> Effects {
        self.state.stations.clear();
        self.stations_ctl.live_dirty = None;
        self.stations_changed()
    }

    /// Kern-Ereignisse: Scan-Ergebnisse sofort, Hoeren mit Ruhezeit.
    pub fn stations_on_event(&mut self, ev: &Event, now: Instant) -> Effects {
        match ev {
            Event::ScanResult { channel, eid, ensemble, services, .. } => {
                let found = eid.map(|e| (e, ensemble.as_deref().unwrap_or(""), services.as_slice()));
                // Ensemble erkannt, aber (noch) keine Dienste: bekannte Eintraege desselben Ensembles behalten.
                if let Some((e, _, svcs)) = found {
                    let ch = channel.trim().to_uppercase();
                    if svcs.is_empty() && self.state.stations.iter().any(|x| x.channel == ch && x.eid == e) {
                        return Effects::default();
                    }
                }
                if replace_channel(&mut self.state.stations, channel, found, unix_now()) {
                    return self.stations_changed();
                }
                self.stations_ctl.unsaved = true;
            }
            Event::ServiceAdded { .. } | Event::EnsembleFound { .. } | Event::StateSnapshot { .. } => {
                if !self.state.scan.active && self.state.ensemble.is_some() {
                    self.stations_ctl.live_dirty = Some(now);
                }
            }
            Event::ScanProgress { .. } | Event::DeviceClosed | Event::Exiting { .. } => {
                self.stations_ctl.live_dirty = None;
            }
            _ => {}
        }
        Effects::default()
    }

    /// Nach der Ruhezeit: Dienstliste des Kanals uebernehmen.
    pub fn stations_tick(&mut self, now: Instant) -> Effects {
        let Some(t) = self.stations_ctl.live_dirty else { return Effects::default() };
        if now.duration_since(t) < STATIONS_SETTLE {
            return Effects::default();
        }
        self.stations_ctl.live_dirty = None;
        if self.state.scan.active || self.state.services.is_empty() {
            return Effects::default();
        }
        let Some(ens) = self.state.ensemble.clone() else { return Effects::default() };
        let channel = if ens.channel.is_empty() { self.state.channel.clone().unwrap_or_default() } else { ens.channel.clone() };
        if channel.is_empty() {
            return Effects::default();
        }
        let services = self.state.services.clone();
        if merge_channel(&mut self.state.stations, &channel, ens.eid, &ens.name, &services, unix_now()) {
            return self.stations_changed();
        }
        self.stations_ctl.unsaved = true;
        Effects::default()
    }

    /// Dienst aus der Senderliste hoeren – auch auf einem anderen Kanal
    /// (Kanalwechsel, warten auf `service_added`, dann `select_service`).
    pub fn tune_station(&mut self, channel: &str, eid: u16, sid: u32, scids: u8, now: Instant) -> Result<Effects, AppError> {
        let name = self.station(channel, eid, sid, scids).map(|e| e.name.clone()).unwrap_or_default();
        self.tune_to(None, channel, sid, scids, &name, now)
    }

    /// Speicher mit einem Eintrag der Senderliste belegen (Drag/Kontextmenue,
    /// auch aus einem anderen Ensemble). Verhalten wie [`App::preset_store_service`].
    pub fn preset_store_station(&mut self, slot: usize, channel: &str, eid: u16, sid: u32, scids: u8, force: bool) -> Result<(StoreResult, Effects), AppError> {
        if slot >= crate::PRESET_SLOTS {
            return Err(AppError::Slot);
        }
        let entry = self.station(channel, eid, sid, scids).cloned().ok_or_else(|| AppError::Other("station not found".into()))?;
        if self.presets.is_occupied(slot) && !force {
            return Ok((StoreResult { stored: false, previous: self.presets.get(slot).cloned() }, Effects::default()));
        }
        let mut preset = Preset {
            channel: entry.channel.clone(),
            eid: entry.eid,
            sid: entry.sid,
            scids: entry.scids,
            name: entry.name.clone(),
            logo_path: None,
            logo_data_url: None,
            stored_at: unix_now(),
        };
        self.decorate_preset(&mut preset);
        let previous = self.presets.set(slot, preset);
        let fx = self.save_presets();
        Ok((StoreResult { stored: true, previous }, fx))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::app::{PresetStatus, DEFAULT_HACKRF_GAIN};
    use crate::{DataDirs, Presets, Settings};
    use dab_api::{Command, ServiceSlot};

    fn tmp(name: &str) -> std::path::PathBuf {
        let nanos = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        std::env::temp_dir().join(format!("dabclassic-stations-{name}-{}-{nanos}", std::process::id()))
    }

    fn app(name: &str) -> App {
        let mut a = App::with(DataDirs::with_root(tmp(name), true), Settings::default(), Presets::default());
        a.state.device = Some(crate::state::DeviceState { kind: "hackrf".into(), ..Default::default() });
        a
    }

    fn svc(sid: u32, name: &str, audio: bool) -> ServiceInfo {
        ServiceInfo { sid, scids: 0, name: name.into(), is_audio: audio, is_primary: true, sub_ch: 1, bitrate_kbps: 96, pty: 0 }
    }

    fn scan(channel: &str, eid: Option<u16>, ensemble: &str, services: Vec<ServiceInfo>) -> Event {
        Event::ScanResult { channel: channel.into(), eid, ensemble: eid.map(|_| ensemble.to_string()), services, snr: 12.0 }
    }

    fn names(list: &[StationEntry]) -> Vec<String> {
        list.iter().map(|e| format!("{} {}", e.channel, e.name)).collect()
    }

    #[test]
    fn scan_result_replaces_per_channel() {
        let now = Instant::now();
        let mut a = app("scan");
        a.state.scan.active = true;
        let fx = a.handle_event(&scan("5C", Some(0x10BC), "DR Deutschland", vec![svc(0xD210, "Dlf", true), svc(0xD220, "Dlf Kultur", true)]), now);
        assert!(matches!(fx.events[0], AppEvent::StationsChanged { .. }));
        a.handle_event(&scan("11D", Some(0x1E1C), "WDR", vec![svc(0xE1C0, "WDR 5", true), svc(0xE1C1, "1LIVE", true)]), now);
        assert_eq!(a.state.stations.len(), 4);
        // Zweiter Scan: 5C hat jetzt nur noch einen Dienst -> alter zweiter Eintrag weg, 11D bleibt
        a.handle_event(&scan("5C", Some(0x10BC), "DR Deutschland", vec![svc(0xD210, "Dlf", true)]), now);
        assert_eq!(names(&a.state.stations), vec!["5C Dlf", "11D 1LIVE", "11D WDR 5"]);
        // Gleicher Inhalt erneut: kein Ereignis
        let fx = a.handle_event(&scan("5C", Some(0x10BC), "DR Deutschland", vec![svc(0xD210, "Dlf", true)]), now);
        assert!(fx.events.iter().all(|e| !matches!(e, AppEvent::StationsChanged { .. })));
        // Ensemble ohne Dienste (zu kurze Verweilzeit): bekannte Eintraege bleiben
        a.handle_event(&scan("11D", Some(0x1E1C), "WDR", vec![]), now);
        assert_eq!(a.state.stations.len(), 3);
        assert!(std::fs::read_to_string(a.dirs.stations_file()).unwrap().contains("WDR 5"));
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn no_signal_removes_channel() {
        let now = Instant::now();
        let mut a = app("nosig");
        a.state.scan.active = true;
        a.handle_event(&scan("5C", Some(0x10BC), "DR Deutschland", vec![svc(0xD210, "Dlf", true)]), now);
        a.handle_event(&scan("11D", Some(0x1E1C), "WDR", vec![svc(0xE1C0, "WDR 5", true)]), now);
        let fx = a.handle_event(&scan("11D", None, "", vec![]), now);
        assert!(matches!(fx.events[0], AppEvent::StationsChanged { .. }));
        assert_eq!(names(&a.state.stations), vec!["5C Dlf"]);
        // Kanal, der nie drin war: nichts passiert
        let fx = a.handle_event(&scan("7B", None, "", vec![]), now);
        assert!(fx.events.is_empty());
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn sorted_by_channel_ensemble_name() {
        let mut list = Vec::new();
        let now = 1;
        replace_channel(&mut list, "11D", Some((0x1E1C, "WDR", &[svc(2, "wdr 2", true), svc(1, "1LIVE", true)])), now);
        replace_channel(&mut list, "5C", Some((0x10BC, "DR Deutschland", &[svc(3, "Dlf Nova", true), svc(4, "Dlf", true)])), now);
        replace_channel(&mut list, "10B", Some((0x1234, "Antenne", &[svc(5, "Radio A", true)])), now);
        replace_channel(&mut list, "5A", Some((0x2222, "Test", &[svc(6, "Zed", true)])), now);
        assert_eq!(names(&list), vec!["5A Zed", "5C Dlf", "5C Dlf Nova", "10B Radio A", "11D 1LIVE", "11D wdr 2"]);
        assert_eq!(channel_key("13F"), (13, "F".into()));
        assert!(channel_key("5D") < channel_key("10A"));
        // Datendienste ausblenden
        replace_channel(&mut list, "5C", Some((0x10BC, "DR Deutschland", &[svc(4, "Dlf", true), svc(7, "EPG", false)])), now);
        assert_eq!(names(&list), vec!["5A Zed", "5C Dlf", "5C EPG", "10B Radio A", "11D 1LIVE", "11D wdr 2"]);
        assert_eq!(filtered(&list, false).len(), 5);
        assert_eq!(filtered(&list, true).len(), 6);
    }

    #[test]
    fn json_roundtrip() {
        let path = tmp("json").join("stations.json");
        let mut st = Stations::default();
        replace_channel(&mut st.entries, "5c", Some((0x10BC, "DR Deutschland ", &[svc(0xD210, "Dlf ", true)])), 1_700_000_000);
        st.save(&path).unwrap();
        let back = Stations::load(&path);
        assert_eq!(back, st);
        let e = &back.entries[0];
        assert_eq!((e.channel.as_str(), e.ensemble.as_str(), e.name.as_str(), e.last_seen_unix), ("5C", "DR Deutschland", "Dlf", 1_700_000_000));
        // Aeltere/handgeschriebene Datei ohne optionale Felder bleibt lesbar
        let s: Stations = serde_json::from_str(r#"{"version":1,"entries":[{"channel":"5C","eid":4284,"ensemble":"DR","sid":1,"name":"X","is_audio":true}]}"#).unwrap();
        assert_eq!(s.entries[0].bitrate_kbps, 0);
        let _ = std::fs::remove_dir_all(path.parent().unwrap());
    }

    #[test]
    fn listening_fills_channel_after_settle() {
        let now = Instant::now();
        let mut a = app("live");
        a.state.channel = Some("5C".into());
        a.handle_event(&Event::EnsembleFound { eid: 0x10BC, name: "DR Deutschland".into(), channel: "5C".into() }, now);
        a.handle_event(&Event::ServiceAdded { service: svc(0xD210, "Dlf", true) }, now);
        let fx = a.handle_event(&Event::ServiceAdded { service: svc(0xD220, "Dlf Kultur", true) }, now + Duration::from_millis(500));
        assert!(a.state.stations.is_empty(), "erst nach der Ruhezeit");
        assert!(fx.events.iter().all(|e| !matches!(e, AppEvent::StationsChanged { .. })));
        assert!(a.tick(now + Duration::from_millis(2400)).events.is_empty(), "Ruhezeit laeuft ab dem letzten service_added");
        let fx = a.tick(now + Duration::from_millis(2600));
        assert!(matches!(fx.events[0], AppEvent::StationsChanged { .. }));
        assert_eq!(names(&a.state.stations), vec!["5C Dlf", "5C Dlf Kultur"]);
        assert_eq!(a.state.stations[0].ensemble, "DR Deutschland");
        // Anderer Kanal per Scan dazu; danach 11D gehoert, aber der FIC liefert erst einen Teil:
        // bekannte Dienste bleiben, ein neuer kommt dazu, ein geaenderter (PTY) wird aktualisiert
        a.state.scan.active = true;
        a.handle_event(&scan("11D", Some(0x1E1C), "WDR", vec![svc(0xE1C0, "WDR 5", true), svc(0xE1C1, "1LIVE", true)]), now);
        a.handle_event(&Event::ScanFinished, now);
        a.handle_event(&Event::EnsembleFound { eid: 0x1E1C, name: "WDR".into(), channel: "11D".into() }, now);
        let mut live = svc(0xE1C1, "1LIVE", true);
        live.pty = 10;
        a.handle_event(&Event::ServiceAdded { service: live }, now);
        a.handle_event(&Event::ServiceAdded { service: svc(0xE1C2, "WDR 2", true) }, now);
        let fx = a.tick(now + Duration::from_secs(3));
        assert!(matches!(fx.events[0], AppEvent::StationsChanged { .. }));
        assert_eq!(names(&a.state.stations), vec!["5C Dlf", "5C Dlf Kultur", "11D 1LIVE", "11D WDR 2", "11D WDR 5"]);
        assert_eq!(a.state.stations.iter().find(|e| e.sid == 0xE1C1).unwrap().pty, 10);
        // Unveraenderte Meldung: kein Ereignis
        a.handle_event(&Event::ServiceAdded { service: svc(0xE1C2, "WDR 2", true) }, now);
        assert!(a.tick(now + Duration::from_secs(6)).events.is_empty());
        // Auf 5C ein anderes Ensemble gehoert: 5C ersetzt, 11D bleibt
        a.handle_event(&Event::EnsembleFound { eid: 0x10FF, name: "Neu".into(), channel: "5C".into() }, now);
        a.handle_event(&Event::ServiceAdded { service: svc(0x1, "Neuer Dienst", true) }, now);
        a.tick(now + Duration::from_secs(9));
        assert_eq!(names(&a.state.stations), vec!["5C Neuer Dienst", "11D 1LIVE", "11D WDR 2", "11D WDR 5"]);
        // Leeren
        let fx = a.stations_clear();
        assert!(matches!(fx.events[0], AppEvent::StationsChanged { stations: ref s } if s.is_empty()));
        assert!(App::with(a.dirs.clone(), Settings::default(), Presets::default()).state.stations.is_empty());
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn tune_station_switches_channel_and_selects() {
        let now = Instant::now();
        let mut a = app("tune");
        a.state.scan.active = true;
        a.handle_event(&scan("5C", Some(0x10BC), "DR Deutschland", vec![svc(0xD210, "Dlf", true), svc(0xD220, "Dlf Kultur", true)]), now);
        a.handle_event(&scan("11D", Some(0x1E1C), "WDR", vec![svc(0xE1C0, "WDR 5", true)]), now);
        a.handle_event(&Event::ScanFinished, now);
        a.state.channel = Some("5C".into());
        a.handle_event(&Event::EnsembleFound { eid: 0x10BC, name: "DR Deutschland".into(), channel: "5C".into() }, now);
        a.handle_event(&Event::ServiceAdded { service: svc(0xD220, "Dlf Kultur", true) }, now);
        // Gleicher Kanal, Dienst bekannt: sofort
        let fx = a.tune_station("5C", 0x10BC, 0xD220, 0, now).unwrap();
        assert_eq!(fx.commands, vec![Command::SelectService { sid: 0xD220, scids: 0, slot: ServiceSlot::Primary }]);
        assert!(matches!(fx.events[0], AppEvent::PresetStatus { slot: None, status: PresetStatus::Selected, .. }));
        // Anderer Kanal: Gain, Kanalwechsel, warten, dann select_service
        let fx = a.tune_station("11D", 0x1E1C, 0xE1C0, 0, now).unwrap();
        assert_eq!(fx.commands, vec![Command::SetGain { gain: DEFAULT_HACKRF_GAIN }, Command::SetChannel { channel: "11D".into() }]);
        assert!(matches!(&fx.events[0], AppEvent::PresetStatus { slot: None, status: PresetStatus::Tuning, name, channel } if name == "WDR 5" && channel == "11D"));
        assert!(a.is_pending());
        a.handle_event(&Event::EnsembleFound { eid: 0x1E1C, name: "WDR".into(), channel: "11D".into() }, now);
        let fx = a.handle_event(&Event::ServiceAdded { service: svc(0xE1C1, "1LIVE", true) }, now);
        assert!(fx.commands.is_empty());
        let fx = a.handle_event(&Event::ServiceAdded { service: svc(0xE1C0, "WDR 5", true) }, now);
        assert_eq!(fx.commands, vec![Command::SelectService { sid: 0xE1C0, scids: 0, slot: ServiceSlot::Primary }]);
        assert!(!a.is_pending());
        // Umschaltsperre bei Aufnahme
        a.state.recording = true;
        assert_eq!(a.tune_station("5C", 0x10BC, 0xD210, 0, now).unwrap_err(), AppError::Recording);
        a.state.recording = false;
        // Speicher aus der Senderliste belegen (anderes Ensemble als das aktuelle)
        let (r, fx) = a.preset_store_station(2, "5C", 0x10BC, 0xD210, 0, false).unwrap();
        assert!(r.stored);
        assert!(matches!(fx.events[0], AppEvent::PresetsChanged { .. }));
        let p = a.presets.get(2).unwrap();
        assert_eq!((p.channel.as_str(), p.eid, p.sid, p.name.as_str()), ("5C", 0x10BC, 0xD210, "Dlf"));
        let (r, _) = a.preset_store_station(2, "11D", 0x1E1C, 0xE1C0, 0, false).unwrap();
        assert!(!r.stored && r.previous.unwrap().name == "Dlf");
        assert!(a.preset_store_station(3, "7B", 1, 2, 0, false).is_err());
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }
}
