//! TII (Transmitter Identification Information) und Debug-Panel
//! (Entscheidung 25): Senderdatenbank `txdata.tii` (Rust-Port des Qt-DAB
//! `tii-reader`, GPL-Herkunft siehe `resources/tii/README.md`), Zuordnung der
//! vom Kern gemeldeten (mainId, subId) zu Sendestandorten, Entfernung und
//! Azimut zu den Heimatkoordinaten, DX-Protokoll `tii-files.csv` im
//! v1-Format sowie der Debug-Zustand (Scopes nur bei offenem Panel,
//! SNR-Verlauf, Fehlerzaehler, Frequenzversatz).
//!
//! Spektrum- und Konstellationsdaten (`spectrum`, `iq_samples`) laufen nicht
//! durch den AppState; die Shell verarbeitet sie direkt aus `dab://event`.

use crate::app::{App, AppEvent, Effects};
use crate::state::AppState;
use crate::DataDirs;
use dab_api::{Command, Event, ServiceSlot, TiiEntry};
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

/// Laenge des SNR-Verlaufs (1 Hz, 120 s).
pub const SNR_HISTORY_LEN: usize = 120;
/// Standardrate der Scopes (Protokoll `set_scopes`, 1..10).
pub const SCOPE_RATE_DEFAULT: u8 = 5;
/// Standardschwelle des TII-Detektors (Protokoll `set_tii`).
pub const TII_THRESHOLD_DEFAULT: i16 = 6;

// ---------------------------------------------------------------------------
// Datenbank
// ---------------------------------------------------------------------------

/// Ein Sender aus `txdata.tii`.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Transmitter {
    pub name: String,
    pub lat: f64,
    pub lon: f64,
    pub power_kw: f32,
    /// Standorthoehe ue. NN (Spalte `height`).
    pub altitude_m: f32,
    /// Antennenhoehe ueber Grund (Spalte `ant`).
    pub height_m: f32,
    pub polarization: String,
    /// Richtdiagramm-Angabe der Datenbank (z. B. "300-240"), leer = rund.
    pub direction: String,
    pub channel: String,
    pub ensemble: String,
    pub country: String,
    pub eid: u16,
    pub main_id: u8,
    pub sub_id: u8,
}

/// Geladene Datenbank mit Index (EId, mainId, subId) und (mainId, subId).
#[derive(Default, Debug)]
pub struct TiiDatabase {
    entries: Vec<Transmitter>,
    by_key: HashMap<(u16, u8, u8), Vec<usize>>,
    by_id: HashMap<(u8, u8), Vec<usize>>,
    pub source: Option<PathBuf>,
}

const SEPARATOR: char = ';';
// Spaltenindizes wie tii-reader.cpp
const COUNTRY: usize = 1;
const CHANNEL: usize = 2;
const ENSEMBLE: usize = 3;
const EID: usize = 4;
const TII: usize = 5;
const TRANSMITTERNAME: usize = 6;
const LATITUDE: usize = 7;
const LONGITUDE: usize = 8;
const ALTITUDE: usize = 9;
const HEIGHT: usize = 10;
const POLARIZATION: usize = 11;
const POWER: usize = 13;
const DIRECTION: usize = 14;
const NR_COLUMNS: usize = 15;

impl TiiDatabase {
    /// Dekodiert den Dateiinhalt: erstes Byte = Shift; `0xAA` → XOR, sonst
    /// Subtraktion. Zeilenumbrueche (0x0A) bleiben unveraendert (wie v1).
    pub fn decode(raw: &[u8]) -> Vec<u8> {
        let Some((&shift, body)) = raw.split_first() else { return Vec::new() };
        body.iter()
            .map(|&b| if b == b'\n' { b } else if shift == 0xAA { b ^ 0xAA } else { b.wrapping_sub(shift) })
            .collect()
    }

    /// Kodiert Text im XOR-Format (fuer Tests und eigene Dateien).
    pub fn encode_xor(text: &[u8]) -> Vec<u8> {
        let mut out = Vec::with_capacity(text.len() + 1);
        out.push(0xAA);
        out.extend(text.iter().map(|&b| if b == b'\n' { b } else { b ^ 0xAA }));
        out
    }

    fn line_to_string(line: &[u8]) -> String {
        match std::str::from_utf8(line) {
            Ok(s) => s.to_string(),
            // Datei ist teils Latin-1 (v1 las sie als UTF-8 und zeigte "�").
            Err(_) => line.iter().map(|&b| b as char).collect(),
        }
    }

    /// Liest die dekodierten Textzeilen ein; ungueltige Zeilen (Kopfzeile,
    /// mainId/subId 0 oder 255, leeres Ensemble) werden uebersprungen.
    pub fn parse(decoded: &[u8]) -> Self {
        let mut db = Self::default();
        for line in decoded.split(|&b| b == b'\n') {
            let text = Self::line_to_string(line);
            let text = text.trim_start_matches('\u{feff}');
            let cols: Vec<&str> = text.split(SEPARATOR).collect();
            if cols.len() < NR_COLUMNS {
                continue;
            }
            let tii: u16 = cols[TII].trim().parse().unwrap_or(0);
            let main_id = (tii / 100) as u8;
            let sub_id = (tii % 100) as u8;
            let ensemble = cols[ENSEMBLE].trim();
            if main_id == 0 || sub_id == 0 || main_id == 255 || sub_id == 255 || ensemble.is_empty() {
                continue;
            }
            let num = |i: usize| cols[i].trim().parse::<f64>().unwrap_or(0.0);
            let t = Transmitter {
                name: cols[TRANSMITTERNAME].trim().to_string(),
                lat: num(LATITUDE),
                lon: num(LONGITUDE),
                power_kw: num(POWER) as f32,
                altitude_m: num(ALTITUDE) as f32,
                height_m: num(HEIGHT) as f32,
                polarization: cols[POLARIZATION].trim().to_string(),
                direction: cols[DIRECTION].trim().to_string(),
                channel: cols[CHANNEL].trim().to_uppercase(),
                ensemble: ensemble.to_string(),
                country: cols[COUNTRY].trim().to_string(),
                eid: u16::from_str_radix(cols[EID].trim(), 16).unwrap_or(0),
                main_id,
                sub_id,
            };
            let idx = db.entries.len();
            db.by_key.entry((t.eid, main_id, sub_id)).or_default().push(idx);
            db.by_id.entry((main_id, sub_id)).or_default().push(idx);
            db.entries.push(t);
        }
        db
    }

    pub fn load(path: &Path) -> std::io::Result<Self> {
        let raw = std::fs::read(path)?;
        let mut db = Self::parse(&Self::decode(&raw));
        db.source = Some(path.to_path_buf());
        Ok(db)
    }

    /// Reihenfolge wie v1: `data/txdata.tii` (Override), dann die Ressource.
    pub fn locate(dirs: &DataDirs, resource: Option<&Path>) -> Option<PathBuf> {
        let over = dirs.root.join("txdata.tii");
        if over.is_file() {
            return Some(over);
        }
        resource.filter(|p| p.is_file()).map(Path::to_path_buf)
    }

    pub fn len(&self) -> usize { self.entries.len() }
    pub fn is_empty(&self) -> bool { self.entries.is_empty() }
    /// Wie v1 `has_tiiFile`: erst ab einer Handvoll Eintraegen brauchbar.
    pub fn is_usable(&self) -> bool { self.entries.len() > 10 }

    /// Sucht (EId, mainId, subId); bei mehreren Treffern bevorzugt den mit
    /// passendem Kanal. Ohne Treffer: Fallback ueber (mainId, subId) + Kanal.
    pub fn lookup(&self, eid: u16, main_id: u8, sub_id: u8, channel: Option<&str>) -> Option<&Transmitter> {
        let ch = channel.map(|c| c.trim().to_uppercase());
        if let Some(list) = self.by_key.get(&(eid, main_id, sub_id)) {
            let pick = ch
                .as_deref()
                .and_then(|c| list.iter().find(|&&i| self.entries[i].channel == c))
                .or_else(|| list.first());
            return pick.map(|&i| &self.entries[i]);
        }
        let ch = ch?;
        self.by_id
            .get(&(main_id, sub_id))?
            .iter()
            .map(|&i| &self.entries[i])
            .find(|t| t.channel == ch)
    }
}

// ---------------------------------------------------------------------------
// Geometrie
// ---------------------------------------------------------------------------

const EARTH_RADIUS_KM: f64 = 6371.0;

/// Grosskreisentfernung (Haversine) in km.
pub fn haversine_km(lat1: f64, lon1: f64, lat2: f64, lon2: f64) -> f64 {
    let (p1, p2) = (lat1.to_radians(), lat2.to_radians());
    let dp = p2 - p1;
    let dl = (lon2 - lon1).to_radians();
    let a = (dp / 2.0).sin().powi(2) + p1.cos() * p2.cos() * (dl / 2.0).sin().powi(2);
    2.0 * EARTH_RADIUS_KM * a.sqrt().asin()
}

/// Anfangs-Peilung von (lat1, lon1) nach (lat2, lon2) in Grad 0..360.
pub fn azimuth_deg(lat1: f64, lon1: f64, lat2: f64, lon2: f64) -> f64 {
    let (p1, p2) = (lat1.to_radians(), lat2.to_radians());
    let dl = (lon2 - lon1).to_radians();
    let y = dl.sin() * p2.cos();
    let x = p1.cos() * p2.sin() - p1.sin() * p2.cos() * dl.cos();
    (y.atan2(x).to_degrees() + 360.0) % 360.0
}

/// Himmelsrichtung (8 Sektoren) zu einem Azimut.
pub fn compass(azimuth: f64) -> &'static str {
    const DIRS: [&str; 8] = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"];
    let i = (((azimuth % 360.0 + 360.0) % 360.0 + 22.5) / 45.0) as usize % 8;
    DIRS[i]
}

// ---------------------------------------------------------------------------
// Zustand
// ---------------------------------------------------------------------------

/// Ein im Nullsymbol gesehener Sender, angereichert aus der Datenbank.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct TiiSeen {
    pub main_id: u8,
    pub sub_id: u8,
    pub strength: f32,
    pub transmitter: Option<Transmitter>,
    pub distance_km: Option<f32>,
    pub azimuth_deg: Option<f32>,
}

/// Zaehler der letzten Sekunde und Summen seit Dienststart (`service_stats`).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
pub struct ServiceStatsState {
    pub sid: u32,
    pub frame_errors: u16,
    pub rs_errors: u16,
    pub aac_errors: u16,
    pub rs_corrections: u16,
    pub total_frame_errors: u64,
    pub total_rs_errors: u64,
    pub total_aac_errors: u64,
    pub total_rs_corrections: u64,
    pub seconds: u64,
}

impl ServiceStatsState {
    fn add(&mut self, sid: u32, fe: u16, rs: u16, aac: u16, corr: u16) {
        if self.sid != sid {
            *self = Self { sid, ..Default::default() };
        }
        self.frame_errors = fe;
        self.rs_errors = rs;
        self.aac_errors = aac;
        self.rs_corrections = corr;
        self.total_frame_errors += fe as u64;
        self.total_rs_errors += rs as u64;
        self.total_aac_errors += aac as u64;
        self.total_rs_corrections += corr as u64;
        self.seconds += 1;
    }
}

/// Debug-Zustand (Teil des AppState-Snapshots, `debug_stats`-Ereignis).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
#[serde(default)]
pub struct DebugState {
    /// Panel offen → Kern liefert Spektrum/IQ (`set_scopes`).
    pub open: bool,
    pub scope_rate_hz: u8,
    /// SNR je Sekunde, aeltester Wert zuerst, hoechstens [`SNR_HISTORY_LEN`].
    pub snr_history: Vec<f32>,
    pub freq_offset_hz: i32,
    pub stats_primary: Option<ServiceStatsState>,
    pub stats_background: Option<ServiceStatsState>,
    /// Datenbank geladen (Eintraege) und Quelle.
    pub tii_db_entries: usize,
    pub tii_db_source: Option<String>,
}

/// Nicht-serialisierter Teil (Datenbank, Zeitgeber, DX-Protokoll).
#[derive(Default)]
pub struct TiiCtl {
    pub db: TiiDatabase,
    last_snr_push: Option<Instant>,
    /// Je Sitzung einmal ins CSV: (Kanal, EId, mainId, subId).
    logged: HashSet<(String, u16, u8, u8)>,
}

fn lookup_seen(db: &TiiDatabase, st: &AppState, home: Option<(f64, f64)>, e: &TiiEntry) -> TiiSeen {
    let eid = st.ensemble.as_ref().map(|x| x.eid).unwrap_or(0);
    let channel = st.channel.as_deref().or_else(|| st.ensemble.as_ref().map(|x| x.channel.as_str()));
    let transmitter = if eid != 0 { db.lookup(eid, e.main_id, e.sub_id, channel) } else { channel.and_then(|_| db.lookup(0, e.main_id, e.sub_id, channel)) };
    let (distance_km, azimuth_deg) = match (transmitter, home) {
        (Some(t), Some((hlat, hlon))) if t.lat != 0.0 && t.lon != 0.0 => (
            Some(haversine_km(hlat, hlon, t.lat, t.lon) as f32),
            Some(azimuth_deg(hlat, hlon, t.lat, t.lon) as f32),
        ),
        _ => (None, None),
    };
    TiiSeen { main_id: e.main_id, sub_id: e.sub_id, strength: e.strength, transmitter: transmitter.cloned(), distance_km, azimuth_deg }
}

/// Eine Zeile im v1-Format von `tii-files.csv` (Qt-DAB `addtoLogFile`).
pub fn csv_line(now: &str, channel: &str, ensemble: &str, s: &TiiSeen) -> String {
    let t = s.transmitter.as_ref();
    let name = t.map(|t| t.name.as_str()).unwrap_or("not in database");
    let (lat, lon) = t.map(|t| (t.lat, t.lon)).unwrap_or((0.0, 0.0));
    let dist = s.distance_km.map(|d| format!("{d:.1}")).unwrap_or_else(|| "nan".into());
    let az = s.azimuth_deg.map(|a| format!("{a:.1}")).unwrap_or_else(|| "0.0".into());
    format!(
        "{now};\"{channel}\";\"{ensemble}\";\"{name}\"; \"({lat:.6}, {lon:.6})\";\"{}\";\"{}\";\"{dist} km\";\"{az} \"  ;\"{:.1} kW\";\"{} m\";\"{} m\";\"{}\"\n",
        s.main_id,
        s.sub_id,
        t.map(|t| t.power_kw).unwrap_or(0.0),
        t.map(|t| t.altitude_m as i32).unwrap_or(0),
        t.map(|t| t.height_m as i32).unwrap_or(0),
        t.map(|t| t.direction.as_str()).unwrap_or(""),
    )
}

fn fx_ev(e: AppEvent) -> Effects {
    Effects { commands: Vec::new(), events: vec![e] }
}

const CSV_HEADER: &str = "date; channel; ensemble; transmitter; coords; mainId; subId;distance (km);azimuth;Power;altitude;height;direction\n\n";

fn append_csv(path: &Path, lines: &[String]) -> std::io::Result<()> {
    use std::io::Write;
    if lines.is_empty() {
        return Ok(());
    }
    let existed = path.is_file();
    let mut f = std::fs::OpenOptions::new().create(true).append(true).open(path)?;
    if !existed {
        f.write_all(CSV_HEADER.as_bytes())?;
    }
    for l in lines {
        f.write_all(l.as_bytes())?;
    }
    Ok(())
}

impl App {
    // -----------------------------------------------------------------------
    // Datenbank
    // -----------------------------------------------------------------------

    /// Datenbank laden: `data/txdata.tii` vor der Ressource (`resource` =
    /// `resources/tii/txdata.tii` der Tauri-App, None im Test/CLI).
    pub fn tii_load(&mut self, resource: Option<&Path>) {
        let Some(path) = TiiDatabase::locate(&self.dirs, resource) else {
            log::warn!("txdata.tii nicht gefunden (Ressource und {})", self.dirs.root.join("txdata.tii").display());
            return;
        };
        match TiiDatabase::load(&path) {
            Ok(db) => {
                log::info!("TII-Datenbank: {} Sender aus {}", db.len(), path.display());
                self.state.debug.tii_db_entries = db.len();
                self.state.debug.tii_db_source = Some(path.display().to_string());
                self.tii.db = db;
            }
            Err(e) => log::warn!("txdata.tii {}: {e}", path.display()),
        }
    }

    fn home(&self) -> Option<(f64, f64)> {
        match (self.settings.home_lat, self.settings.home_lon) {
            (Some(a), Some(b)) if a != 0.0 || b != 0.0 => Some((a, b)),
            _ => None,
        }
    }

    // -----------------------------------------------------------------------
    // Kommandos an den Kern
    // -----------------------------------------------------------------------

    fn scopes_cmd(&self) -> Command {
        let open = self.settings.panels.debug;
        Command::SetScopes { spectrum: open, iq: open, rate_hz: self.settings.scope_rate_hz.clamp(1, 10) }
    }

    fn tii_cmd(&self) -> Command {
        Command::SetTii { enabled: self.settings.tii_enabled, threshold: self.settings.tii_threshold, dx_mode: self.settings.tii_dx_mode }
    }

    /// Nach `ready`: TII-Einstellungen immer, Scopes nur bei offenem Panel
    /// (aus = Kernstandard, kein Datenfluss).
    pub fn debug_startup(&mut self) -> Effects {
        self.state.debug.open = self.settings.panels.debug;
        self.state.debug.scope_rate_hz = self.settings.scope_rate_hz;
        let mut fx = Effects::default();
        fx.commands.push(self.tii_cmd());
        if self.settings.panels.debug {
            fx.commands.push(self.scopes_cmd());
        }
        fx
    }

    /// Nach `update_settings`: Unterschiede mit Kern-Wirkung senden.
    pub fn debug_on_settings(&mut self, old: &crate::Settings) -> Effects {
        let s = &self.settings;
        let mut fx = Effects::default();
        if old.panels.debug != s.panels.debug || old.scope_rate_hz != s.scope_rate_hz {
            self.state.debug.open = s.panels.debug;
            self.state.debug.scope_rate_hz = s.scope_rate_hz;
            fx.commands.push(self.scopes_cmd());
            if s.panels.debug {
                fx.events.push(AppEvent::DebugStats { debug: self.state.debug.clone() });
            }
        }
        if old.tii_enabled != s.tii_enabled || old.tii_threshold != s.tii_threshold || old.tii_dx_mode != s.tii_dx_mode {
            fx.commands.push(self.tii_cmd());
            if !s.tii_enabled {
                self.state.tii.clear();
                fx.events.push(AppEvent::TiiUpdated { tii: Vec::new() });
            }
        }
        if old.home_lat != s.home_lat || old.home_lon != s.home_lon {
            self.tii_recompute();
            fx.events.push(AppEvent::TiiUpdated { tii: self.state.tii.clone() });
        }
        fx
    }

    /// Panel oeffnen/schliessen (persistiert in `settings.panels.debug`).
    pub fn debug_set_open(&mut self, open: bool) -> Effects {
        let mut s = self.settings.clone();
        s.panels.debug = open;
        self.update_settings(s)
    }

    pub fn debug_set_rate(&mut self, rate_hz: u8) -> Effects {
        let mut s = self.settings.clone();
        s.scope_rate_hz = rate_hz.clamp(1, 10);
        self.update_settings(s)
    }

    pub fn tii_set(&mut self, enabled: bool, threshold: i16, dx_mode: bool) -> Effects {
        let mut s = self.settings.clone();
        s.tii_enabled = enabled;
        s.tii_threshold = threshold;
        s.tii_dx_mode = dx_mode;
        self.update_settings(s)
    }

    /// Heimatkoordinaten (None/None = loeschen).
    pub fn home_set(&mut self, lat: Option<f64>, lon: Option<f64>) -> Effects {
        let mut s = self.settings.clone();
        s.home_lat = lat;
        s.home_lon = lon;
        self.update_settings(s)
    }

    // -----------------------------------------------------------------------
    // Ereignisse
    // -----------------------------------------------------------------------

    fn tii_recompute(&mut self) {
        let home = self.home();
        let list: Vec<TiiEntry> = self.state.tii.iter().map(|s| TiiEntry { main_id: s.main_id, sub_id: s.sub_id, strength: s.strength }).collect();
        self.state.tii = list.iter().map(|e| lookup_seen(&self.tii.db, &self.state, home, e)).collect();
    }

    fn tii_apply(&mut self, transmitters: &[TiiEntry]) -> Effects {
        let home = self.home();
        let mut seen: Vec<TiiSeen> = transmitters.iter().map(|e| lookup_seen(&self.tii.db, &self.state, home, e)).collect();
        seen.sort_by(|a, b| b.strength.partial_cmp(&a.strength).unwrap_or(std::cmp::Ordering::Equal));
        if self.settings.tii_dx_mode {
            self.tii_log(&seen);
        }
        self.state.tii = seen;
        fx_ev(AppEvent::TiiUpdated { tii: self.state.tii.clone() })
    }

    /// DX-Modus: jede in dieser Sitzung neu gesehene Kombination anhaengen.
    fn tii_log(&mut self, seen: &[TiiSeen]) {
        let Some(ens) = self.state.ensemble.clone() else { return };
        let channel = self.state.channel.clone().unwrap_or_else(|| ens.channel.clone());
        let now = chrono::Local::now().format("%a %b %-d %H:%M:%S %Y").to_string();
        let mut lines = Vec::new();
        for s in seen {
            let key = (channel.clone(), ens.eid, s.main_id, s.sub_id);
            if self.tii.logged.insert(key) {
                lines.push(csv_line(&now, &channel, &ens.name, s));
            }
        }
        if let Err(e) = append_csv(&self.tii_csv_path(), &lines) {
            log::warn!("tii-files.csv: {e}");
        }
    }

    pub fn tii_csv_path(&self) -> PathBuf {
        self.dirs.root.join("tii-files.csv")
    }

    /// Kern-Ereignisse fuer TII und Debug-Zaehler.
    pub fn debug_on_event(&mut self, ev: &Event, now: Instant) -> Effects {
        match ev {
            Event::Tii { transmitters } => return self.tii_apply(transmitters),
            Event::FrequencyOffset { hz } => self.state.debug.freq_offset_hz = *hz,
            Event::ServiceStats { slot, sid, frame_errors, rs_errors, aac_errors, rs_corrections } => {
                let slot_state = match slot {
                    ServiceSlot::Primary => &mut self.state.debug.stats_primary,
                    ServiceSlot::Background => &mut self.state.debug.stats_background,
                };
                slot_state.get_or_insert_with(Default::default).add(*sid, *frame_errors, *rs_errors, *aac_errors, *rs_corrections);
            }
            Event::ServiceStopped { slot, sid } => {
                let slot_state = match slot {
                    ServiceSlot::Primary => &mut self.state.debug.stats_primary,
                    ServiceSlot::Background => &mut self.state.debug.stats_background,
                };
                if slot_state.as_ref().map(|s| s.sid == *sid).unwrap_or(false) {
                    *slot_state = None;
                }
            }
            Event::DeviceClosed | Event::Exiting { .. } | Event::ScanProgress { .. } | Event::NoSignal { .. } => {
                self.state.debug.stats_primary = None;
                self.state.debug.stats_background = None;
                self.state.debug.freq_offset_hz = 0;
                if !self.state.tii.is_empty() {
                    self.state.tii.clear();
                    return fx_ev(AppEvent::TiiUpdated { tii: Vec::new() });
                }
            }
            Event::EnsembleFound { .. } => {
                // Namen/Entfernungen ggf. erst jetzt aufloesbar (EId bekannt).
                if !self.state.tii.is_empty() && self.state.tii.iter().any(|t| t.transmitter.is_none()) {
                    self.tii_recompute();
                    return fx_ev(AppEvent::TiiUpdated { tii: self.state.tii.clone() });
                }
            }
            Event::Ready { .. } => {
                self.tii.last_snr_push = Some(now);
            }
            _ => {}
        }
        Effects::default()
    }

    /// 1-Hz-Zeitgeber: SNR in den Ring, bei offenem Panel `debug_stats`.
    pub fn debug_tick(&mut self, now: Instant) -> Effects {
        let due = self.tii.last_snr_push.map(|t| now.duration_since(t) >= Duration::from_secs(1)).unwrap_or(true);
        if !due {
            return Effects::default();
        }
        self.tii.last_snr_push = Some(now);
        let snr = if self.state.synced { self.state.snr } else { 0.0 };
        let h = &mut self.state.debug.snr_history;
        h.push(snr);
        if h.len() > SNR_HISTORY_LEN {
            let cut = h.len() - SNR_HISTORY_LEN;
            h.drain(..cut);
        }
        if self.settings.panels.debug {
            fx_ev(AppEvent::DebugStats { debug: self.state.debug.clone() })
        } else {
            Effects::default()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Presets, Settings};

    fn resource_path() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../apps/desktop/src-tauri/resources/tii/txdata.tii")
    }

    #[test]
    fn decode_xor_roundtrip_and_parse() {
        let text = "\u{feff}id;country;block;ensemblelabel;eid;tii;location;latidec;longidec;height;ant;pol;frequency;erp;dirdeg\r\n\
391735;DE;5C;DR Deutschland;10BC;2004;Langenberg/Hordtberg;51.356256;7.134128;239;292;h;178.352;10.000000;\r\n\
390772;DE;5C;DR Deutschland;10BC;2002;Duesseldorf/Rheinturm;51.217964;6.761675;37;224;v;178.352;10.000000;\r\n\
2001434;DE;5C;DR Deutschland;10BC;;Balderschwang;47.458444;10.144522;1417;8;v;178.352;0.200000;;\r\n\
1;DE;5C;;10BC;2003;Ohne Ensemble;1;1;1;1;v;178.352;1;\r\n";
        let raw = TiiDatabase::encode_xor(text.as_bytes());
        assert_eq!(raw[0], 0xAA);
        assert_eq!(TiiDatabase::decode(&raw), text.as_bytes());
        let db = TiiDatabase::parse(&TiiDatabase::decode(&raw));
        assert_eq!(db.len(), 2, "Kopfzeile, TII leer und leeres Ensemble sind ungueltig");
        let t = db.lookup(0x10BC, 20, 4, Some("5C")).unwrap();
        assert_eq!(t.name, "Langenberg/Hordtberg");
        assert_eq!((t.altitude_m, t.height_m, t.power_kw), (239.0, 292.0, 10.0));
        assert_eq!(t.polarization, "h");
        assert!(db.lookup(0x10BC, 20, 3, None).is_none());
        // Fallback ohne EId ueber den Kanal
        assert_eq!(db.lookup(0, 20, 2, Some("5C")).unwrap().name, "Duesseldorf/Rheinturm");
        assert!(db.lookup(0, 20, 2, Some("11D")).is_none());
        // Shift-Variante
        let mut shifted = vec![3u8];
        shifted.extend(text.bytes().map(|b| if b == b'\n' { b } else { b.wrapping_add(3) }));
        assert_eq!(TiiDatabase::decode(&shifted), text.as_bytes());
    }

    #[test]
    fn real_database_langenberg() {
        let db = TiiDatabase::load(&resource_path()).expect("resources/tii/txdata.tii");
        assert!(db.len() > 5000, "{} Eintraege", db.len());
        let t = db.lookup(0x10BC, 20, 4, Some("5C")).unwrap();
        assert_eq!(t.name, "Langenberg/Hordtberg");
        assert!((t.lat - 51.356256).abs() < 1e-5 && (t.lon - 7.134128).abs() < 1e-5);
        assert!(db.lookup(0x10BC, 20, 2, None).unwrap().name.ends_with("/Rheinturm"));
        assert!(db.lookup(0x10BC, 20, 1, None).unwrap().name.ends_with("/Colonius"));
    }

    #[test]
    fn haversine_and_azimuth() {
        // Rheinturm -> Langenberg: ~30 km, Richtung ENE
        let d = haversine_km(51.217964, 6.761675, 51.356256, 7.134128);
        assert!((29.0..31.5).contains(&d), "{d}");
        let a = azimuth_deg(51.217964, 6.761675, 51.356256, 7.134128);
        assert!((55.0..65.0).contains(&a), "{a}");
        assert_eq!(compass(a), "NE");
        assert_eq!(compass(0.0), "N");
        assert_eq!(compass(270.0), "W");
        assert_eq!(compass(350.0), "N");
        assert!(haversine_km(50.0, 6.0, 50.0, 6.0).abs() < 1e-9);
    }

    #[test]
    fn tii_event_sorted_and_logged_in_dx_mode() {
        let tmp = std::env::temp_dir().join(format!("dabclassic-tii-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&tmp);
        std::fs::create_dir_all(&tmp).unwrap();
        let mut settings = Settings::default();
        settings.home_lat = Some(51.217964);
        settings.home_lon = Some(6.761675);
        settings.tii_dx_mode = true;
        let mut a = App::with(DataDirs::with_root(&tmp, true), settings, Presets::default());
        a.tii_load(Some(&resource_path()));
        assert!(a.tii.db.is_usable());
        let now = Instant::now();
        a.handle_event(&Event::EnsembleFound { eid: 0x10BC, name: "DR Deutschland".into(), channel: "5C".into(), ecc: 0 }, now);
        let ev = Event::Tii {
            transmitters: vec![
                TiiEntry { main_id: 20, sub_id: 1, strength: 0.12 },
                TiiEntry { main_id: 20, sub_id: 4, strength: 0.30 },
                TiiEntry { main_id: 20, sub_id: 2, strength: 0.25 },
            ],
        };
        let fx = a.handle_event(&ev, now);
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::TiiUpdated { tii } if tii.len() == 3)));
        let names: Vec<&str> = a.state.tii.iter().map(|t| t.transmitter.as_ref().unwrap().name.as_str()).collect();
        assert_eq!(names[0], "Langenberg/Hordtberg");
        assert!(a.state.tii[1].distance_km.unwrap() < 1.0, "Rheinturm ist die Heimat");
        assert!((29.0..31.5).contains(&a.state.tii[0].distance_km.unwrap()));
        // CSV: Kopf + 3 Zeilen, zweites Ereignis haengt nichts Neues an
        a.handle_event(&ev, now);
        let csv = std::fs::read_to_string(a.tii_csv_path()).unwrap();
        assert!(csv.starts_with("date; channel; ensemble; transmitter;"));
        assert_eq!(csv.lines().filter(|l| l.contains("\"5C\";\"DR Deutschland\"")).count(), 3);
        assert!(csv.contains("\"Langenberg/Hordtberg\"; \"(51.356256, 7.134128)\";\"20\";\"4\";"));
        // Leere Liste
        let fx = a.handle_event(&Event::Tii { transmitters: vec![] }, now);
        assert!(a.state.tii.is_empty());
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::TiiUpdated { tii } if tii.is_empty())));
        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    fn scopes_only_while_panel_open() {
        let tmp = std::env::temp_dir().join(format!("dabclassic-dbg-{}", std::process::id()));
        let mut a = App::with(DataDirs::with_root(&tmp, true), Settings::default(), Presets::default());
        let now = Instant::now();
        let fx = a.startup(now);
        assert!(fx.commands.contains(&Command::SetTii { enabled: true, threshold: 6, dx_mode: false }));
        assert!(!fx.commands.iter().any(|c| matches!(c, Command::SetScopes { .. })), "Panel zu: keine Scopes");
        let fx = a.debug_set_open(true);
        assert!(fx.commands.contains(&Command::SetScopes { spectrum: true, iq: true, rate_hz: 5 }));
        assert!(a.settings.panels.debug && a.state.debug.open);
        let fx = a.debug_set_rate(12);
        assert!(fx.commands.contains(&Command::SetScopes { spectrum: true, iq: true, rate_hz: 10 }));
        let fx = a.debug_set_open(false);
        assert!(fx.commands.contains(&Command::SetScopes { spectrum: false, iq: false, rate_hz: 10 }));
        // SNR-Ring: 1 Hz, begrenzt
        a.handle_event(&Event::Synced { synced: true }, now);
        for i in 0..130u64 {
            a.handle_event(&Event::Snr { db: i as f32 }, now + Duration::from_secs(i + 1));
        }
        assert_eq!(a.state.debug.snr_history.len(), SNR_HISTORY_LEN);
        assert_eq!(*a.state.debug.snr_history.last().unwrap(), 129.0);
        // Zaehler je Slot mit Summen
        let stats = |sid, fe| Event::ServiceStats { slot: ServiceSlot::Primary, sid, frame_errors: fe, rs_errors: 1, aac_errors: 0, rs_corrections: 5 };
        a.handle_event(&stats(0xD210, 2), now);
        a.handle_event(&stats(0xD210, 3), now);
        let p = a.state.debug.stats_primary.clone().unwrap();
        assert_eq!((p.frame_errors, p.total_frame_errors, p.total_rs_corrections, p.seconds), (3, 5, 10, 2));
        a.handle_event(&stats(0xD220, 1), now);
        assert_eq!(a.state.debug.stats_primary.as_ref().unwrap().total_frame_errors, 1, "neuer Dienst: Summen neu");
        let _ = std::fs::remove_dir_all(&tmp);
    }
}
