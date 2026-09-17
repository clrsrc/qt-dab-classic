//! TPEG-Verkehrsmeldungen (Punkt 3, Auftrag Stefan 17.09.2026).
//!
//! Der Kern startet den TPEG-Paketdienst des Ensembles (FIG 0/13 Appl-Type 4,
//! DSCTy 5 TDC; in NRW "ARD TPEG" auf 11D "WDR NRW" und 9A "WDR Regional",
//! nicht im Bundesmux 5C) als Background-Slot und reicht jede geprueft
//! zusammengesetzte MSC-Datengruppe als `Event::TdcGroup` durch
//! (`Settings::tpeg_enabled`, Standard an - kein Internet, nur Broadcast).
//!
//! Hier wird daraus die Meldungsliste fuer das Verkehr-Panel:
//! `frame` (Transport-/Dienstrahmen, zlib) -> `sni` (welche SCID ist TEC) ->
//! `tec` (Meldungen mit Ereignis, Ursachen, Hinweisen) -> `olr` (Ort als
//! OpenLR-Koordinaten). Die ARD sendet einen bundesweiten Bestand (~350
//! Meldungen, Umlauf ~35 s, Ablaufzeiten wenige Minuten; gemessen an der
//! Senderuhr aus FIG 0/10, damit auch Mitschnitte funktionieren). Mit
//! Heimatkoordinaten (`Settings::home_lat/lon`) bekommt jede Meldung
//! Entfernung und Richtung und die Liste ist nach Entfernung sortiert,
//! sonst neueste zuerst. Strassennamen gibt es ohne Karte nicht - die App
//! zeigt Strassenklasse/-art, Koordinaten, Laenge und Richtung; fuer
//! Autobahnen ordnet `roads` (eingebaute OpenStreetMap-Tabelle) Nummer und
//! Anschlussstellen zu.
//!
//! TFP (Verkehrsfluss, SCID 2) wird erkannt, aber nicht ausgewertet.

pub mod frame;
pub mod olr;
pub mod roads;
pub mod sni;
pub mod tec;
pub mod ubcr;

use crate::app::{App, AppEvent, Effects};
use base64::Engine as _;
use dab_api::Event;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

pub use olr::{Location, LocationKind};
pub use roads::RoadInfo;
pub use tec::{TecAdvice, TecCause, TecEvent, TecMessage};

/// Nach Ablauf so lange noch anzeigen (Karussell-Luecken ueberbruecken).
const EXPIRY_GRACE_S: i64 = 120;
/// Hoechstzahl Stuetzpunkte je Meldung im Status (IPC-Groesse).
const MAX_POINTS: usize = 8;
/// Hoechstzahl Meldungen im Speicher.
const MAX_MESSAGES: usize = 1000;

/// Zustand des TPEG-Empfangs, wie ihn das Frontend sieht (`AppState::tpeg`).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
#[serde(default)]
pub struct TpegStatus {
    /// `Settings::tpeg_enabled`
    pub enabled: bool,
    /// Ein TPEG-Dienst ist im Ensemble (Datendienst mit "TPEG" im Namen bzw. erste Datengruppe).
    pub available: bool,
    pub sid: u32,
    pub service_name: String,
    pub description: String,
    pub provider: String,
    /// "TEC 3.2" aus der SNI-Versionstabelle
    pub tec_version: String,
    /// Zeitpunkt der letzten Datengruppe (Unix), 0 = noch keine
    pub last_unix: i64,
    pub groups: u32,
    pub frames: u32,
    pub tfp_seen: bool,
    /// Heimatkoordinaten bekannt -> `distance_km`/`direction_deg` gefuellt, Liste nach Entfernung
    pub home_known: bool,
    pub messages: Vec<TpegEntry>,
}

/// Eine TEC-Meldung fuer die Anzeige (Codes, Texte macht das Frontend per i18n).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
#[serde(default)]
pub struct TpegEntry {
    pub id: u32,
    pub version: u8,
    /// tec001 Wirkung (1..7), 0 = nur Verwaltung/ohne Ereignis
    pub effect: u8,
    pub causes: Vec<TecCause>,
    pub advices: Vec<TecAdvice>,
    pub start_unix: Option<i64>,
    pub stop_unix: Option<i64>,
    pub expiry_unix: i64,
    pub length_m: Option<u32>,
    pub delay_min: Option<u32>,
    pub speed_kmh: Option<u32>,
    pub tendency: Option<u8>,
    pub junction_closure: Option<u8>,
    /// Ort: erster Punkt, Stuetzpunkte (lon, lat), Strassenklasse/-art, Fahrtrichtung
    pub lat: Option<f64>,
    pub lon: Option<f64>,
    pub points: Vec<(f64, f64)>,
    pub frc: Option<u8>,
    pub fow: Option<u8>,
    pub bearing_deg: Option<u16>,
    pub tmc_code: Option<u16>,
    pub location_text: Vec<String>,
    /// Autobahn und Anschlussstellen aus der eingebauten OpenStreetMap-Tabelle
    /// (crate::tpeg::roads), falls der Ort auf einer Autobahn liegt.
    pub road: Option<RoadInfo>,
    /// vom Heimatort aus (nur mit Koordinaten)
    pub distance_km: Option<f64>,
    pub direction_deg: Option<u16>,
    pub first_seen: i64,
    pub updated: i64,
}

#[derive(Clone, Debug)]
struct Stored {
    msg: TecMessage,
    first_seen: i64,
    updated: i64,
}

/// TPEG-Modul in [`App`].
#[derive(Default, Debug)]
pub struct TpegCtl {
    eid: u16,
    sid: u32,
    available: bool,
    service_name: String,
    sni: Option<sni::SniInfo>,
    messages: BTreeMap<u32, Stored>,
    groups: u32,
    frames: u32,
    tfp_seen: bool,
    last_unix: i64,
    /// Zaehler fuer Tests/Diagnose: Datengruppen ohne brauchbaren Rahmen
    pub bad_groups: u32,
}

impl TpegCtl {
    fn clear(&mut self) {
        self.sid = 0;
        self.available = false;
        self.service_name.clear();
        self.sni = None;
        self.messages.clear();
        self.groups = 0;
        self.frames = 0;
        self.tfp_seen = false;
        self.last_unix = 0;
    }

    /// Eine Datengruppe einarbeiten; `true`, wenn sich der Bestand geaendert hat.
    pub fn push_group(&mut self, sid: u32, data: &[u8], now: i64) -> bool {
        self.groups += 1;
        self.last_unix = now;
        self.available = true;
        if self.sid == 0 {
            self.sid = sid;
        }
        let mut changed = false;
        let frames = frame::split_transport_frames(data);
        if frames.is_empty() {
            self.bad_groups += 1;
            return false;
        }
        for f in frames {
            if f.frame_type != frame::FRAME_SERVICE {
                continue;
            }
            let Some(sf) = frame::parse_service_frame(f.body) else { continue };
            self.frames += 1;
            for c in &sf.components {
                if c.scid == 0 {
                    if let Some(info) = sni::parse_sni(c.payload()) {
                        if self.sni.as_ref() != Some(&info) {
                            self.sni = Some(info);
                            changed = true;
                        }
                    }
                    continue;
                }
                // Ohne SNI gilt die ARD-Belegung (TEC = SCID 1, TFP = SCID 2).
                let aid = self.sni.as_ref().and_then(|s| s.aid_for_scid(c.scid)).unwrap_or(match c.scid {
                    1 => sni::AID_TEC,
                    2 => sni::AID_TFP,
                    _ => 0,
                });
                match aid {
                    sni::AID_TEC => {
                        let (_n, msgs) = tec::parse_tec_field(c.payload());
                        for m in msgs {
                            changed |= self.upsert(m, now);
                        }
                    }
                    sni::AID_TFP => self.tfp_seen = true,
                    _ => {}
                }
            }
        }
        changed |= self.prune(now);
        changed
    }

    fn upsert(&mut self, m: TecMessage, now: i64) -> bool {
        if m.cancel {
            return self.messages.remove(&m.message_id).is_some();
        }
        match self.messages.get_mut(&m.message_id) {
            Some(st) => {
                if st.msg.version == m.version && st.msg.expiry_unix == m.expiry_unix && st.msg.event == m.event {
                    st.updated = now;
                    return false;
                }
                // Neue Version (oder Wrap-around mit spaeterem Ablauf): ersetzen
                st.msg = m;
                st.updated = now;
                true
            }
            None => {
                if self.messages.len() >= MAX_MESSAGES {
                    // aeltestes Update raus
                    if let Some(k) = self.messages.iter().min_by_key(|(_, s)| s.updated).map(|(k, _)| *k) {
                        self.messages.remove(&k);
                    }
                }
                self.messages.insert(m.message_id, Stored { msg: m, first_seen: now, updated: now });
                true
            }
        }
    }

    /// Abgelaufene Meldungen (Ablauf + Karenz) entfernen.
    pub fn prune(&mut self, now: i64) -> bool {
        let before = self.messages.len();
        self.messages.retain(|_, s| s.msg.expiry_unix as i64 + EXPIRY_GRACE_S >= now);
        before != self.messages.len()
    }

    pub fn message_count(&self) -> usize {
        self.messages.len()
    }

    /// Status fuer Frontend/State bauen.
    pub fn status(&self, enabled: bool, home: Option<(f64, f64)>) -> TpegStatus {
        let mut messages: Vec<TpegEntry> = self.messages.values().map(|s| entry_of(s, home)).collect();
        if home.is_some() {
            messages.sort_by(|a, b| {
                a.distance_km
                    .unwrap_or(f64::MAX)
                    .partial_cmp(&b.distance_km.unwrap_or(f64::MAX))
                    .unwrap_or(std::cmp::Ordering::Equal)
                    .then(b.updated.cmp(&a.updated))
            });
        } else {
            messages.sort_by(|a, b| b.first_seen.cmp(&a.first_seen).then(b.id.cmp(&a.id)));
        }
        let (service_name, description, provider, tec_version) = match &self.sni {
            Some(s) => {
                let ver = s
                    .components
                    .iter()
                    .find(|c| c.aid == sni::AID_TEC)
                    .map(|c| format!("{} {}.{}", if c.name.is_empty() { "TEC" } else { &c.name }, c.major, c.minor))
                    .unwrap_or_default();
                (s.service_name.clone(), s.description.clone(), s.provider.clone(), ver)
            }
            None => (self.service_name.clone(), String::new(), String::new(), String::new()),
        };
        TpegStatus {
            enabled,
            available: self.available,
            sid: self.sid,
            service_name,
            description,
            provider,
            tec_version,
            last_unix: self.last_unix,
            groups: self.groups,
            frames: self.frames,
            tfp_seen: self.tfp_seen,
            home_known: home.is_some(),
            messages,
        }
    }
}

fn entry_of(s: &Stored, home: Option<(f64, f64)>) -> TpegEntry {
    let m = &s.msg;
    let mut e = TpegEntry {
        id: m.message_id,
        version: m.version,
        expiry_unix: m.expiry_unix as i64,
        first_seen: s.first_seen,
        updated: s.updated,
        ..Default::default()
    };
    if let Some(ev) = &m.event {
        e.effect = ev.effect;
        e.causes = ev.causes.clone();
        e.advices = ev.advices.clone();
        e.start_unix = ev.start_unix.map(i64::from);
        e.stop_unix = ev.stop_unix.map(i64::from);
        e.length_m = ev.length_m;
        e.delay_min = ev.delay_min;
        e.speed_kmh = ev.avg_speed_ms.map(|v| (v as u32 * 36 + 5) / 10);
        e.tendency = ev.tendency;
        e.junction_closure = ev.junction_closure;
    }
    if let Some(loc) = &m.location {
        if let Some((lon, lat)) = loc.first() {
            e.lon = Some(lon);
            e.lat = Some(lat);
        }
        e.points = loc.points.iter().copied().take(MAX_POINTS).collect();
        e.frc = loc.frc;
        e.fow = loc.fow;
        e.bearing_deg = loc.bearing_deg;
        e.tmc_code = loc.tmc_code;
        e.location_text = loc.description.clone();
        e.road = roads::lookup(&loc.points, loc.fow);
        if e.length_m.is_none() {
            e.length_m = loc.length_m.filter(|&l| l > 0);
        }
        if let (Some(h), Some(c)) = (home, loc.center()) {
            let (d, b) = olr::distance_bearing(h, c);
            e.distance_km = Some((d * 10.0).round() / 10.0);
            e.direction_deg = Some(b);
        }
    }
    e
}

impl App {
    fn tpeg_home(&self) -> Option<(f64, f64)> {
        match (self.settings.home_lat, self.settings.home_lon) {
            (Some(lat), Some(lon)) => Some((lon, lat)),
            _ => None,
        }
    }

    fn tpeg_notify(&mut self) -> Effects {
        self.state.tpeg = self.tpeg.status(self.settings.tpeg_enabled, self.tpeg_home());
        let mut fx = Effects::default();
        fx.events.push(AppEvent::Tpeg { status: self.state.tpeg.clone() });
        fx
    }

    /// Aus [`App::handle_event`]: Datengruppen dekodieren, Bestand pflegen,
    /// bei Ensemble-/Geraetewechsel leeren.
    pub fn tpeg_on_event(&mut self, ev: &Event, now_unix: i64) -> Effects {
        match ev {
            Event::TdcGroup { sid, data_b64, .. } => {
                if !self.settings.tpeg_enabled {
                    return Effects::default();
                }
                let Ok(data) = base64::engine::general_purpose::STANDARD.decode(data_b64) else {
                    return Effects::default();
                };
                // Ablaufzeiten gegen die Senderuhr (FIG 0/10, `clock_time`, im
                // Sekundentakt) pruefen, nicht gegen die Wanduhr: so stimmen sie
                // auch bei der Wiedergabe eines Mitschnitts, und live ist die
                // Senderuhr die Referenz des Dienstanbieters.
                let now = self.state.clock_utc.unwrap_or(now_unix);
                let changed = self.tpeg.push_group(*sid, &data, now);
                // Zaehler/Zeitstempel aendern sich immer; Meldungsliste nur bei Bedarf
                // vollstaendig neu - die Groesse (bis ~350 Eintraege) ist bei einer
                // Datengruppe alle ~5 s vertretbar.
                let _ = changed;
                self.tpeg_notify()
            }
            Event::EnsembleFound { eid, .. } => {
                if *eid != self.tpeg.eid {
                    self.tpeg.eid = *eid;
                    self.tpeg.clear();
                    return self.tpeg_notify();
                }
                Effects::default()
            }
            Event::ServiceAdded { service } if !service.is_audio && service.name.to_ascii_uppercase().contains("TPEG") => {
                self.tpeg.available = true;
                if self.tpeg.sid == 0 {
                    self.tpeg.sid = service.sid;
                }
                if self.tpeg.service_name.is_empty() {
                    self.tpeg.service_name = service.name.trim().to_string();
                }
                self.tpeg_notify()
            }
            Event::DeviceClosed | Event::Exiting { .. } => {
                self.tpeg.eid = 0;
                self.tpeg.clear();
                self.tpeg_notify()
            }
            _ => Effects::default(),
        }
    }

    /// Aus dem Settings-Wechsel: Schalter uebernehmen (aus -> Liste leeren).
    pub fn tpeg_set_enabled(&mut self, enabled: bool) -> Effects {
        if !enabled {
            self.tpeg.messages.clear();
            self.tpeg.sni = None;
        }
        self.tpeg_notify()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Drei Datengruppen von "ARD TPEG" (11D, 17.09.2026), Format [u16 BE Laenge][Bytes].
    const FIXTURE: &[u8] = include_bytes!("../../tests/data/tpeg/wdr-11d-3dg.bin");

    pub fn fixture_groups() -> Vec<Vec<u8>> {
        let mut out = Vec::new();
        let mut i = 0;
        while i + 2 <= FIXTURE.len() {
            let n = u16::from_be_bytes([FIXTURE[i], FIXTURE[i + 1]]) as usize;
            out.push(FIXTURE[i + 2..i + 2 + n].to_vec());
            i += 2 + n;
        }
        out
    }

    #[test]
    fn mitschnitt_liefert_sni_und_meldungen() {
        let groups = fixture_groups();
        assert_eq!(groups.len(), 3);
        let mut ctl = TpegCtl::default();
        let now = 0x6aaba2b5_i64 - 300; // kurz vor den Ablaufzeiten des Mitschnitts
        let mut changed = false;
        for (k, g) in groups.iter().enumerate() {
            changed |= ctl.push_group(0xE0D01006, g, now + k as i64);
        }
        assert!(changed);
        assert_eq!(ctl.bad_groups, 0);
        let st = ctl.status(true, None);
        assert_eq!(st.service_name, "ARD TPEG");
        assert_eq!(st.tec_version, "TEC 3.2");
        assert!(st.tfp_seen);
        assert_eq!(st.groups, 3);
        assert!(st.frames >= 12, "frames {}", st.frames);
        // 3 Datengruppen x 4-5 Rahmen x ~10 Meldungen, davon viele verschieden
        assert!(st.messages.len() >= 60, "{} Meldungen", st.messages.len());
        assert!(st.messages.iter().all(|m| m.effect >= 1 && m.effect <= 7), "Wirkung ausserhalb tec001");
        // fast alle Meldungen haben einen Ort (ein paar Verwaltungs-/Ereignismeldungen ohne LRC)
        let with_loc = st.messages.iter().filter(|m| m.lat.is_some()).count();
        assert!(with_loc * 100 >= st.messages.len() * 90, "{with_loc} von {} mit Ort", st.messages.len());
        // TMC-Code kommt bei den meisten dazu
        assert!(st.messages.iter().filter(|m| m.tmc_code.is_some()).count() * 2 >= st.messages.len());
        // alle Orte in Deutschland
        assert!(st.messages.iter().filter(|m| m.lat.is_some()).all(|m| (47.0..55.5).contains(&m.lat.unwrap()) && (5.5..15.5).contains(&m.lon.unwrap())), "Ort ausserhalb Deutschlands");
        assert!(st.messages.iter().any(|m| !m.causes.is_empty()));
        assert!(st.messages.iter().any(|m| !m.advices.is_empty()));
        // ohne Heimatort: keine Entfernung, neueste zuerst
        assert!(st.messages[0].distance_km.is_none());
        // mit Heimatort Koeln: sortiert nach Entfernung
        let st = ctl.status(true, Some((6.96, 50.94)));
        assert!(st.home_known);
        let d: Vec<f64> = st.messages.iter().filter_map(|m| m.distance_km).collect();
        assert!(d.len() >= 60);
        assert!(d.windows(2).all(|w| w[0] <= w[1]), "nicht nach Entfernung sortiert");
        assert!(d[0] < 100.0, "naechste Meldung {} km", d[0]);
    }

    #[test]
    fn wiederholung_aendert_nichts_und_ablauf_raeumt_auf() {
        let groups = fixture_groups();
        let mut ctl = TpegCtl::default();
        let now = 0x6aaba2b5_i64 - 300;
        ctl.push_group(1, &groups[0], now);
        let n = ctl.message_count();
        assert!(n > 0);
        // gleiche Gruppe noch einmal: keine Aenderung
        assert!(!ctl.push_group(1, &groups[0], now + 1));
        assert_eq!(ctl.message_count(), n);
        // weit nach dem Ablauf: alles weg
        assert!(ctl.prune(0x6aaba2b5_i64 + 3600));
        assert_eq!(ctl.message_count(), 0);
        // Muell: kein Rahmen
        assert!(!ctl.push_group(1, &[1, 2, 3, 4, 5, 6, 7, 8], now));
        assert_eq!(ctl.bad_groups, 1);
    }

    /// Stichprobe der Strassenzuordnung: `cargo test -p dab-app zeige_strassen -- --ignored --nocapture`
    #[test]
    #[ignore]
    fn zeige_strassen() {
        let mut ctl = TpegCtl::default();
        for g in fixture_groups() {
            ctl.push_group(1, &g, 0x6aaba2b5_i64 - 300);
        }
        let st = ctl.status(true, Some((6.96, 50.94)));
        let with_road = st.messages.iter().filter(|m| m.road.is_some()).count();
        println!("{} von {} Meldungen mit Autobahn", with_road, st.messages.len());
        for m in st.messages.iter().take(25) {
            println!("{:>6.1} km  fow {:?} frc {:?}  {:?}  {:?}", m.distance_km.unwrap_or(-1.0), m.fow, m.frc, m.road.as_ref().map(|r| format!("{} {:?} -> {:?} ({} m)", r.road, r.from, r.to, r.dist_m)), (m.lat, m.lon));
        }
    }

    #[test]
    fn stornierung_entfernt_meldung() {
        let mut ctl = TpegCtl::default();
        let m = TecMessage { message_id: 7, version: 0, expiry_unix: 1000, ..Default::default() };
        assert!(ctl.upsert(m.clone(), 10));
        let c = TecMessage { cancel: true, ..m };
        assert!(ctl.upsert(c, 11));
        assert_eq!(ctl.message_count(), 0);
    }
}
