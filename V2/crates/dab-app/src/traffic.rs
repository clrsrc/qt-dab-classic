//! Verkehrs- und Sonderdurchsagen (EN 300 401 8.1.6): FIG 0/18 meldet je
//! Dienst, welche Durchsagearten er unterstuetzt und in welchen Clustern er
//! mithoert; FIG 0/19 schaltet eine Durchsage fuer einen Cluster ein/aus und
//! nennt den Subkanal, auf dem sie laeuft. Der Kern reicht das als
//! `Event::Announcement { sid, kind, sub_ch, active, cluster }` durch - ein
//! Ereignis je Dienst des Clusters. Hier wird daraus eine Liste wie die
//! EWF-Historie (Auftrag Stefan 16.09.2026): laufende Durchsage oben,
//! beendete dieser Sitzung darunter, plus optional die klassische
//! "TA"-Funktion eines Autoradios: waehrend der Durchsage auf den
//! Durchsage-Dienst umschalten und danach zurueck
//! (`Settings::traffic_autoswitch`, Standard aus).
//!
//! Mitschnitt (Auftrag Stefan 16.09.2026, Punkt 2): Durchsagen und
//! Notfallwarnungen werden unabhaengig vom gehoerten Programm aufgezeichnet -
//! der Durchsage-/Warndienst laeuft dafuer im Background-Slot des Kerns
//! (dekodiert ohne Audioausgabe) und schreibt eine MP3 mit ID3-Tags nach
//! `<Aufnahmeordner>/durchsagen/`. Die Datei haengt am Listeneintrag
//! (`TrafficEntry::file`, `EwsHistoryEntry::file`) und ist dort abspielbar.
//! `Settings::announcement_record` (Standard an) schaltet das ab.
//!
//! Kein TPEG-Decoder: der Paketdatendienst "ARD TPEG" (App-Typ 4 in FIG
//! 0/13) bleibt undekodiert, das waere ein eigenes Projekt.

use crate::app::{App, AppEvent, Effects};
use chrono::Local;
use dab_api::{Command, Event, EwsPhase, Id3Tags, RecFormat, ServiceInfo, ServiceSlot};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// Hoechstzahl beendeter Durchsagen in `AppState::traffic_history`.
pub const TRAFFIC_HISTORY_MAX: usize = 50;

/// ASw-Bit 0 (Alarm) laeuft ueber den EWS-Pfad des Kerns und nicht hier.
const FLAG_ALARM: u16 = 0x0001;

/// Unterordner des Aufnahmeordners fuer Durchsage-/Warnungs-Mitschnitte.
pub const ANNOUNCEMENT_DIR: &str = "durchsagen";

/// Kleinere Mitschnitte (Durchsage ohne dekodierbares Audio) werden verworfen.
const MIN_BYTES: u64 = 4096;

/// Eine Durchsage (laufend oder beendet).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
#[serde(default)]
pub struct TrafficEntry {
    pub id: u64,
    pub started_at: i64,
    pub ended_at: Option<i64>,
    pub channel: String,
    pub ensemble: String,
    /// ASw-Flags (8.1.6.2): Bit 1 Verkehr, 2 Nahverkehr, 3 Warnung, 4
    /// Nachrichten, 5 Wetter, 6 Veranstaltung, 7 Sonderereignis, 8
    /// Programmhinweis, 9 Sport, 10 Wirtschaft.
    pub flags: u16,
    pub cluster: u8,
    /// Subkanal, auf dem die Durchsage laeuft (aus FIG 0/19), und der
    /// Audiodienst dazu, falls im Ensemble bekannt.
    pub sub_ch: u8,
    pub announcing_service: Option<String>,
    /// Dienste, fuer die die Durchsage gilt (Mitglieder des Clusters, wie
    /// vom Kern je Ereignis gemeldet).
    pub services: Vec<String>,
    /// Die App hat fuer diese Durchsage umgeschaltet (`traffic_autoswitch`).
    pub switched: bool,
    /// Mitschnitt (MP3), sobald er laeuft bzw. fertig ist.
    pub file: Option<String>,
}

/// Laufender Mitschnitt im Background-Slot.
#[derive(Clone, Debug, PartialEq)]
struct Capture {
    sid: u32,
    scids: u8,
    path: PathBuf,
    /// Wird nach dem Stop-Echo des Kerns ausgewertet: zu kleine Datei -> weg.
    confirmed: bool,
}

/// Zustand des Verkehrsfunk-Moduls in [`App`].
#[derive(Default, Debug)]
pub struct TrafficCtl {
    next_id: u64,
    /// Dienst (sid, scids), zu dem nach der Durchsage zurueckgeschaltet wird.
    return_to: Option<(u32, u8)>,
    /// SIds, fuer die in diesem Ensemble schon Durchsagen gemeldet wurden
    /// (Naeherung fuer "Dienst unterstuetzt Durchsagen", solange der Kern
    /// FIG 0/18 nicht eigens meldet).
    seen_sids: Vec<u32>,
    /// Mitschnitt der laufenden Verkehrsdurchsage.
    capture: Option<Capture>,
    /// Mitschnitt der laufenden Notfallwarnung (iid, Aufnahme).
    ews_capture: Option<(u16, Capture)>,
}

impl App {
    /// Aus [`App::handle_event`]: Durchsagen protokollieren, optional
    /// umschalten, Mitschnitte fuehren; bei Kanalwechsel/Geraeteende die
    /// laufende Durchsage abschliessen.
    pub fn traffic_on_event(&mut self, ev: &Event, now_unix: i64) -> Effects {
        match ev {
            Event::Announcement { kind, sub_ch, active, sid, cluster } => {
                if *kind & !FLAG_ALARM == 0 && *active {
                    // Reiner Alarm (EWS) oder leere Flags: kein Verkehrsfunk.
                    return Effects::default();
                }
                if *active {
                    self.traffic_start(*sid, *kind, *sub_ch, *cluster, now_unix)
                } else {
                    self.traffic_end(*cluster, now_unix, true)
                }
            }
            Event::EwsAlert { phase, sub_ch, iid, relevant, .. } => self.ews_capture_on_alert(*phase, *sub_ch, *iid, *relevant, now_unix),
            Event::RecordingState { slot: ServiceSlot::Background, active, path: Some(path), bytes, .. } => self.capture_on_state(path, *active, *bytes),
            Event::EnsembleFound { .. } | Event::DeviceClosed | Event::Exiting { .. } => {
                self.traffic.seen_sids.clear();
                self.traffic.return_to = None;
                // Mitschnitte enden mit dem Ensemble; der Kern stoppt die
                // Background-Dienste beim Kanalwechsel selbst.
                self.traffic.capture = None;
                self.traffic.ews_capture = None;
                let fx = self.traffic_end_any(now_unix);
                self.traffic_refresh_supported();
                fx
            }
            Event::ServiceStarted { slot: ServiceSlot::Primary, .. } => {
                let before = self.state.traffic_supported;
                self.traffic_refresh_supported();
                if before != self.state.traffic_supported {
                    self.traffic_notify()
                } else {
                    Effects::default()
                }
            }
            _ => Effects::default(),
        }
    }

    fn traffic_start(&mut self, sid: u32, flags: u16, sub_ch: u8, cluster: u8, now_unix: i64) -> Effects {
        if !self.traffic.seen_sids.contains(&sid) {
            self.traffic.seen_sids.push(sid);
        }
        let name = self.state.services.iter().find(|s| s.sid == sid).map(|s| s.name.trim().to_string()).unwrap_or_else(|| format!("{sid:04X}"));
        let mut fx = Effects::default();

        // Laeuft schon eine Durchsage desselben Clusters/Subkanals, kommt nur
        // ein weiterer Dienst des Clusters dazu.
        if let Some(a) = self.state.traffic_active.as_mut() {
            if a.cluster == cluster && a.sub_ch == sub_ch {
                if !a.services.contains(&name) {
                    a.services.push(name);
                }
                a.flags |= flags;
                return self.traffic_notify();
            }
            // Andere Durchsage: die alte gilt als beendet (ohne Rueckschaltung,
            // die neue entscheidet gleich selbst).
            fx.append(self.traffic_close(now_unix, false));
        }

        let announcing = self.state.services.iter().find(|s| s.is_audio && s.sub_ch == sub_ch).cloned();
        let mut entry = TrafficEntry {
            id: self.traffic.next_id,
            started_at: now_unix,
            ended_at: None,
            channel: self.state.channel.clone().unwrap_or_default(),
            ensemble: self.state.ensemble.as_ref().map(|e| e.name.trim().to_string()).unwrap_or_default(),
            flags,
            cluster,
            sub_ch,
            announcing_service: announcing.as_ref().map(|s| s.name.trim().to_string()),
            services: vec![name],
            switched: false,
            file: None,
        };
        self.traffic.next_id += 1;

        // TA-Umschaltung: nur wenn die Durchsage den laufenden Dienst betrifft,
        // auf einem anderen Subkanal laeuft, der Zieldienst bekannt ist und
        // weder Aufnahme noch Notfallwarnung laufen.
        if self.settings.traffic_autoswitch && !self.state.recording && self.state.alert.is_none() {
            if let (Some(cur), Some(target)) = (self.state.current.clone(), announcing.clone()) {
                let cur_sub = self.state.service(cur.sid, cur.scids).map(|s| s.sub_ch);
                if cur.sid == sid && cur_sub != Some(sub_ch) && self.traffic.return_to.is_none() {
                    match self.select_service(target.sid, target.scids) {
                        Ok(sel) => {
                            fx.append(sel);
                            self.traffic.return_to = Some((cur.sid, cur.scids));
                            entry.switched = true;
                            log::info!("Verkehrsfunk: Durchsage auf SubCh {sub_ch} ({}), umgeschaltet von {:04X}", target.name.trim(), cur.sid);
                        }
                        Err(e) => log::warn!("Verkehrsfunk: Umschalten nicht moeglich: {e:?}"),
                    }
                }
            }
        }

        // Mitschnitt im Background-Slot, unabhaengig vom gehoerten Programm.
        if self.settings.announcement_record {
            if let Some(target) = announcing {
                let label = kind_label(flags);
                if let Some((cap, cmds)) = self.capture_start(&target, &label, "Verkehrsfunk", now_unix) {
                    entry.file = Some(cap.path.display().to_string());
                    self.traffic.capture = Some(cap);
                    fx.append(cmds);
                }
            }
        }
        self.state.traffic_active = Some(entry);
        self.traffic_refresh_supported();
        fx.append(self.traffic_notify());
        fx
    }

    /// Ende einer Durchsage des Clusters; `switch_back` = zurueck zum
    /// vorherigen Dienst, falls fuer diese Durchsage umgeschaltet wurde.
    fn traffic_end(&mut self, cluster: u8, now_unix: i64, switch_back: bool) -> Effects {
        let Some(a) = self.state.traffic_active.as_ref() else { return Effects::default() };
        if a.cluster != cluster {
            return Effects::default();
        }
        self.traffic_close(now_unix, switch_back)
    }

    fn traffic_end_any(&mut self, now_unix: i64) -> Effects {
        if self.state.traffic_active.is_none() {
            return Effects::default();
        }
        self.traffic_close(now_unix, false)
    }

    fn traffic_close(&mut self, now_unix: i64, switch_back: bool) -> Effects {
        let mut fx = Effects::default();
        if let Some(mut a) = self.state.traffic_active.take() {
            a.ended_at = Some(now_unix);
            self.state.traffic_history.insert(0, a);
            self.state.traffic_history.truncate(TRAFFIC_HISTORY_MAX);
        }
        if let Some(cap) = self.traffic.capture.as_ref() {
            fx.append(capture_stop_commands(cap));
        }
        if let Some((sid, scids)) = self.traffic.return_to.take() {
            if switch_back && !self.state.recording && self.state.alert.is_none() {
                match self.select_service(sid, scids) {
                    Ok(sel) => {
                        fx.append(sel);
                        log::info!("Verkehrsfunk: Durchsage beendet, zurueck zu {sid:04X}");
                    }
                    Err(e) => log::warn!("Verkehrsfunk: Rueckschalten nicht moeglich: {e:?}"),
                }
            }
        }
        fx.append(self.traffic_notify());
        fx
    }

    // -----------------------------------------------------------------------
    // Mitschnitt (Background-Slot)
    // -----------------------------------------------------------------------

    /// Zielordner fuer Durchsage-Mitschnitte (`<Aufnahmeordner>/durchsagen`).
    pub fn announcement_dir(&self) -> PathBuf {
        self.recording_dir().join(ANNOUNCEMENT_DIR)
    }

    /// Dienst im Background-Slot starten und als MP3 aufzeichnen. `label`
    /// (z. B. "Verkehrsdurchsage") wird Titel und Dateiname, `album`
    /// ("Verkehrsfunk"/"Notfallwarnung") das ID3-Album.
    fn capture_start(&mut self, target: &ServiceInfo, label: &str, album: &str, now_unix: i64) -> Option<(Capture, Effects)> {
        let dir = self.announcement_dir();
        if let Err(e) = std::fs::create_dir_all(&dir) {
            log::warn!("Durchsage-Mitschnitt: {} nicht anlegbar: {e}", dir.display());
            return None;
        }
        let now = Local::now();
        let service = target.name.trim().to_string();
        let file = crate::recording::file_name(&now, &service, label).replace(".wav", ".mp3");
        let path = dir.join(file);
        let cmds = Effects {
            commands: vec![
                Command::SelectService { sid: target.sid, scids: target.scids, slot: ServiceSlot::Background },
                Command::StartRecording {
                    path: path.clone(),
                    format: RecFormat::Mp3 {
                        kbps: self.settings.music_mp3_kbps,
                        id3: Some(Id3Tags {
                            title: Some(format!("{label} {}", now.format("%d.%m.%Y %H:%M"))),
                            artist: Some(service),
                            album: Some(album.to_string()),
                            date: Some(now.format("%Y-%m-%d").to_string()),
                            genre: None,
                            cover_png_b64: None,
                        }),
                    },
                    slot: ServiceSlot::Background,
                    sid: Some(target.sid),
                    pre_s: 0.0,
                },
            ],
            events: Vec::new(),
        };
        let _ = now_unix;
        Some((Capture { sid: target.sid, scids: target.scids, path, confirmed: false }, cmds))
    }

    /// Stop-Echo des Kerns fuer einen Background-Mitschnitt: Datei bestaetigen
    /// oder (zu klein, kein Audio angekommen) verwerfen.
    fn capture_on_state(&mut self, path: &Path, active: bool, bytes: u64) -> Effects {
        if active {
            return Effects::default();
        }
        let keep = bytes >= MIN_BYTES;
        if !keep {
            let _ = std::fs::remove_file(path);
        }
        let shown = path.display().to_string();
        let mut changed = false;
        if self.traffic.capture.as_ref().map(|c| c.path == path).unwrap_or(false) {
            self.traffic.capture = None;
            changed = true;
        }
        if self.traffic.ews_capture.as_ref().map(|(_, c)| c.path == path).unwrap_or(false) {
            self.traffic.ews_capture = None;
            changed = true;
        }
        if !keep {
            for e in self.state.traffic_history.iter_mut().chain(self.state.traffic_active.iter_mut()) {
                if e.file.as_deref() == Some(shown.as_str()) {
                    e.file = None;
                    changed = true;
                }
            }
            for e in self.state.ews_history.iter_mut() {
                if e.file.as_deref() == Some(shown.as_str()) {
                    e.file = None;
                    changed = true;
                }
            }
        }
        if !changed {
            return Effects::default();
        }
        let mut fx = self.traffic_notify();
        fx.events.push(AppEvent::EwsHistory { history: self.state.ews_history.clone() });
        fx
    }

    /// Notfallwarnung: ab Trigger/Sustain (sofern der Kern sie fuer den
    /// eigenen Standort als relevant ansieht) den Warndienst im Background
    /// mitschneiden; bei End stoppen und die Datei an den Historien-Eintrag
    /// haengen (den `AppState::apply` gerade angelegt hat).
    fn ews_capture_on_alert(&mut self, phase: EwsPhase, sub_ch: u8, iid: u16, relevant: Option<bool>, now_unix: i64) -> Effects {
        let mut fx = Effects::default();
        match phase {
            EwsPhase::End => {
                if let Some((cap_iid, cap)) = self.traffic.ews_capture.take() {
                    fx.append(capture_stop_commands(&cap));
                    if let Some(e) = self.state.ews_history.iter_mut().find(|e| e.iid == cap_iid) {
                        e.file = Some(cap.path.display().to_string());
                        fx.events.push(AppEvent::EwsHistory { history: self.state.ews_history.clone() });
                    }
                    // Bis zum Stop-Echo bleibt die Datei zur Groessenpruefung
                    // vorgemerkt.
                    self.traffic.ews_capture = Some((cap_iid, Capture { confirmed: true, ..cap }));
                }
            }
            _ => {
                if !self.settings.announcement_record || relevant == Some(false) {
                    return fx;
                }
                if let Some((cap_iid, cap)) = self.traffic.ews_capture.as_ref() {
                    if *cap_iid == iid && !cap.confirmed {
                        return fx; // laeuft schon
                    }
                }
                let Some(target) = self.state.services.iter().find(|s| s.is_audio && s.sub_ch == sub_ch).cloned() else { return fx };
                if let Some((cap, cmds)) = self.capture_start(&target, "Notfallwarnung", "Notfallwarnung", now_unix) {
                    fx.append(cmds);
                    self.traffic.ews_capture = Some((iid, cap));
                }
            }
        }
        fx
    }

    fn traffic_refresh_supported(&mut self) {
        self.state.traffic_supported = match &self.state.current {
            Some(c) => self.traffic.seen_sids.contains(&c.sid),
            None => false,
        };
    }

    fn traffic_notify(&self) -> Effects {
        let mut fx = Effects::default();
        fx.events.push(AppEvent::Traffic {
            active: self.state.traffic_active.clone(),
            history: self.state.traffic_history.clone(),
            supported: self.state.traffic_supported,
        });
        fx
    }
}

/// Aufnahme stoppen und den Background-Dienst wieder freigeben.
fn capture_stop_commands(cap: &Capture) -> Effects {
    Effects {
        commands: vec![
            Command::StopRecording { slot: ServiceSlot::Background, sid: Some(cap.sid) },
            Command::StopService { slot: ServiceSlot::Background, sid: Some(cap.sid) },
        ],
        events: Vec::new(),
    }
}

/// Deutscher Kurzname der Durchsageart fuer Titel/Dateiname (erstes gesetztes Bit).
fn kind_label(flags: u16) -> String {
    const NAMES: [&str; 11] = [
        "Alarm",
        "Verkehrsdurchsage",
        "Nahverkehrsdurchsage",
        "Warndurchsage",
        "Nachrichten",
        "Wetterdurchsage",
        "Veranstaltungshinweis",
        "Sonderdurchsage",
        "Programmhinweis",
        "Sportmeldung",
        "Wirtschaftsmeldung",
    ];
    (1..NAMES.len()).find(|&b| flags & (1 << b) != 0).map(|b| NAMES[b]).unwrap_or("Durchsage").to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{DataDirs, Presets, Settings};
    use dab_api::{Command, ServiceInfo, ServiceSlot};
    use std::time::Instant;

    fn svc(sid: u32, name: &str, sub_ch: u8) -> ServiceInfo {
        ServiceInfo { sid, scids: 0, name: name.into(), is_audio: true, is_primary: true, sub_ch, bitrate_kbps: 88, pty: 0, short_name: String::new(), language: 0 }
    }

    fn app(autoswitch: bool) -> App {
        let nanos = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        let tmp = std::env::temp_dir().join(format!("dabclassic-traffic-{}-{nanos}", std::process::id()));
        let mut settings = Settings::default();
        settings.traffic_autoswitch = autoswitch;
        let mut a = App::with(DataDirs::with_root(&tmp, true), settings, Presets::default());
        a.state.device = Some(crate::state::DeviceState { kind: "hackrf".into(), ..Default::default() });
        a.state.channel = Some("11D".into());
        a.state.ensemble = Some(crate::state::EnsembleState { eid: 0x10EC, name: "WDR NRW".into(), channel: "11D".into() });
        a.state.services.push(svc(0xD395, "WDR 5", 5));
        a.state.services.push(svc(0xD392, "WDR 2 RHEINLAND", 3));
        a.state.services.push(svc(0x10C4, "ASA DE", 1));
        a.state.current = Some(crate::state::CurrentService { sid: 0xD395, scids: 0, codec: None, stereo: true });
        a
    }

    fn ann(sid: u32, active: bool, sub_ch: u8) -> Event {
        Event::Announcement { kind: 0x0002, sub_ch, active, sid, cluster: 4 }
    }

    fn has_cmd(fx: &Effects, f: impl Fn(&Command) -> bool) -> bool {
        fx.commands.iter().any(f)
    }

    #[test]
    fn announcement_is_listed_recorded_in_background_and_moves_to_history() {
        let mut a = app(false);
        let fx = a.handle_event(&ann(0xD395, true, 3), Instant::now());
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::Traffic { active: Some(_), .. })));
        let act = a.state.traffic_active.clone().expect("laufende Durchsage");
        assert_eq!(act.announcing_service.as_deref(), Some("WDR 2 RHEINLAND"));
        assert_eq!(act.services, vec!["WDR 5".to_string()]);
        assert!(!act.switched);
        assert!(a.state.traffic_supported, "aktueller Dienst gehoert zum Cluster");
        // Mitschnitt: Durchsage-Dienst im Background-Slot + MP3 im Unterordner.
        assert!(has_cmd(&fx, |c| matches!(c, Command::SelectService { sid: 0xD392, slot: ServiceSlot::Background, .. })), "{:?}", fx.commands);
        assert!(has_cmd(&fx, |c| matches!(c, Command::StartRecording { slot: ServiceSlot::Background, sid: Some(0xD392), format: RecFormat::Mp3 { .. }, .. })));
        let file = act.file.clone().expect("Dateiname vorgemerkt");
        assert!(file.contains(ANNOUNCEMENT_DIR) && file.ends_with(".mp3") && file.contains("Verkehrsdurchsage"), "{file}");
        // Kein Primary-SelectService (kein Autoswitch).
        assert!(!has_cmd(&fx, |c| matches!(c, Command::SelectService { slot: ServiceSlot::Primary, .. })));
        // Weiterer Dienst desselben Clusters: kein zweiter Eintrag, keine zweite Aufnahme.
        let fx2 = a.handle_event(&ann(0xD392, true, 3), Instant::now());
        assert!(a.state.traffic_history.is_empty());
        assert_eq!(a.state.traffic_active.as_ref().unwrap().services.len(), 2);
        assert!(fx2.commands.is_empty());
        // Ende: Aufnahme stoppen, Background-Dienst freigeben.
        let fx3 = a.handle_event(&ann(0xD395, false, 3), Instant::now());
        assert!(has_cmd(&fx3, |c| matches!(c, Command::StopRecording { slot: ServiceSlot::Background, sid: Some(0xD392) })));
        assert!(has_cmd(&fx3, |c| matches!(c, Command::StopService { slot: ServiceSlot::Background, sid: Some(0xD392) })));
        assert!(a.state.traffic_active.is_none());
        assert_eq!(a.state.traffic_history.len(), 1);
        assert_eq!(a.state.traffic_history[0].file.as_deref(), Some(file.as_str()));
        // Stop-Echo mit brauchbarer Groesse: Datei bleibt am Eintrag.
        a.handle_event(&Event::RecordingState { slot: ServiceSlot::Background, sid: 0xD392, active: false, path: Some(PathBuf::from(&file)), bytes: 200_000, seconds: 25.0 }, Instant::now());
        assert_eq!(a.state.traffic_history[0].file.as_deref(), Some(file.as_str()));
    }

    #[test]
    fn empty_capture_is_dropped_from_the_entry() {
        let mut a = app(false);
        a.handle_event(&ann(0xD395, true, 3), Instant::now());
        let file = a.state.traffic_active.as_ref().unwrap().file.clone().unwrap();
        a.handle_event(&ann(0xD395, false, 3), Instant::now());
        a.handle_event(&Event::RecordingState { slot: ServiceSlot::Background, sid: 0xD392, active: false, path: Some(PathBuf::from(&file)), bytes: 12, seconds: 0.1 }, Instant::now());
        assert_eq!(a.state.traffic_history[0].file, None, "zu kleine Datei -> kein Abspielknopf");
    }

    #[test]
    fn recording_can_be_switched_off() {
        let mut a = app(false);
        a.settings.announcement_record = false;
        let fx = a.handle_event(&ann(0xD395, true, 3), Instant::now());
        assert!(!has_cmd(&fx, |c| matches!(c, Command::StartRecording { .. })));
        assert_eq!(a.state.traffic_active.as_ref().unwrap().file, None);
    }

    #[test]
    fn autoswitch_goes_to_the_announcing_service_and_back() {
        let mut a = app(true);
        let fx = a.handle_event(&ann(0xD395, true, 3), Instant::now());
        assert!(has_cmd(&fx, |c| matches!(c, Command::SelectService { sid: 0xD392, slot: ServiceSlot::Primary, .. })), "auf WDR 2 (SubCh 3) umschalten: {:?}", fx.commands);
        assert!(a.state.traffic_active.as_ref().unwrap().switched);
        a.handle_event(&Event::ServiceStarted { slot: ServiceSlot::Primary, sid: 0xD392, scids: 0, codec: dab_api::Codec::Mp2 { sample_rate: 48000 }, stereo: true }, Instant::now());
        let fx = a.handle_event(&ann(0xD395, false, 3), Instant::now());
        assert!(has_cmd(&fx, |c| matches!(c, Command::SelectService { sid: 0xD395, slot: ServiceSlot::Primary, .. })), "zurueck zu WDR 5: {:?}", fx.commands);
        assert!(a.state.traffic_active.is_none());
    }

    #[test]
    fn no_autoswitch_while_recording_or_for_other_services() {
        let mut a = app(true);
        a.state.recording = true;
        let fx = a.handle_event(&ann(0xD395, true, 3), Instant::now());
        assert!(!has_cmd(&fx, |c| matches!(c, Command::SelectService { slot: ServiceSlot::Primary, .. })));
        assert!(a.state.traffic_active.is_some(), "protokolliert wird trotzdem");
        a.handle_event(&ann(0xD395, false, 3), Instant::now());

        a.state.recording = false;
        let fx = a.handle_event(&ann(0xD392, true, 3), Instant::now());
        assert!(!has_cmd(&fx, |c| matches!(c, Command::SelectService { slot: ServiceSlot::Primary, .. })));
    }

    #[test]
    fn pure_alarm_flags_are_left_to_the_ews_path_and_channel_change_closes() {
        let mut a = app(false);
        a.handle_event(&Event::Announcement { kind: 0x0001, sub_ch: 1, active: true, sid: 0xD395, cluster: 0xFF }, Instant::now());
        assert!(a.state.traffic_active.is_none());
        a.handle_event(&ann(0xD395, true, 3), Instant::now());
        a.handle_event(&Event::DeviceClosed, Instant::now());
        assert!(a.state.traffic_active.is_none());
        assert_eq!(a.state.traffic_history.len(), 1);
        assert!(!a.state.traffic_supported);
    }

    #[test]
    fn emergency_alert_is_captured_and_attached_to_the_history() {
        let mut a = app(false);
        let alert = |phase| Event::EwsAlert { phase, sub_ch: 1, stage: 1, stage_raw: 0x81, iid: 7, locations: vec![], is_test: false, relevant: None };
        let fx = a.handle_event(&alert(EwsPhase::Trigger), Instant::now());
        assert!(has_cmd(&fx, |c| matches!(c, Command::SelectService { sid: 0x10C4, slot: ServiceSlot::Background, .. })), "{:?}", fx.commands);
        assert!(has_cmd(&fx, |c| matches!(c, Command::StartRecording { slot: ServiceSlot::Background, sid: Some(0x10C4), .. })));
        // Sustain: keine zweite Aufnahme.
        let fx = a.handle_event(&alert(EwsPhase::Sustain), Instant::now());
        assert!(!has_cmd(&fx, |c| matches!(c, Command::StartRecording { .. })));
        // Ende: stoppen, Datei am Historien-Eintrag.
        let fx = a.handle_event(&alert(EwsPhase::End), Instant::now());
        assert!(has_cmd(&fx, |c| matches!(c, Command::StopRecording { slot: ServiceSlot::Background, sid: Some(0x10C4) })));
        let file = a.state.ews_history[0].file.clone().expect("Mitschnitt am Alarm");
        assert!(file.contains("Notfallwarnung"), "{file}");
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::EwsHistory { .. })));
    }

    #[test]
    fn irrelevant_alert_is_not_captured() {
        let mut a = app(false);
        let ev = Event::EwsAlert { phase: EwsPhase::Trigger, sub_ch: 1, stage: 1, stage_raw: 0x81, iid: 8, locations: vec![], is_test: false, relevant: Some(false) };
        let fx = a.handle_event(&ev, Instant::now());
        assert!(!has_cmd(&fx, |c| matches!(c, Command::StartRecording { .. })));
    }
}
