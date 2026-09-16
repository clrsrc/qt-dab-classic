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
//! Kein TPEG-Decoder: der Paketdatendienst "ARD TPEG" (App-Typ 4 in FIG
//! 0/13) bleibt undekodiert, das waere ein eigenes Projekt.

use crate::app::{App, AppEvent, Effects};
use dab_api::Event;
use serde::{Deserialize, Serialize};

/// Hoechstzahl beendeter Durchsagen in `AppState::traffic_history`.
pub const TRAFFIC_HISTORY_MAX: usize = 50;

/// ASw-Bit 0 (Alarm) laeuft ueber den EWS-Pfad des Kerns und nicht hier.
const FLAG_ALARM: u16 = 0x0001;

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
}

impl App {
    /// Aus [`App::handle_event`]: Durchsagen protokollieren, optional
    /// umschalten; bei Kanalwechsel/Geraeteende die laufende Durchsage
    /// abschliessen.
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
            Event::EnsembleFound { .. } | Event::DeviceClosed | Event::Exiting { .. } => {
                self.traffic.seen_sids.clear();
                self.traffic.return_to = None;
                let fx = self.traffic_end_any(now_unix);
                self.traffic_refresh_supported();
                fx
            }
            Event::ServiceStarted { slot: dab_api::ServiceSlot::Primary, .. } => {
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
            fx.append(self.traffic_end_any_no_return(now_unix));
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
        };
        self.traffic.next_id += 1;

        // TA-Umschaltung: nur wenn die Durchsage den laufenden Dienst betrifft,
        // auf einem anderen Subkanal laeuft, der Zieldienst bekannt ist und
        // weder Aufnahme noch Notfallwarnung laufen.
        if self.settings.traffic_autoswitch && !self.state.recording && self.state.alert.is_none() {
            if let (Some(cur), Some(target)) = (self.state.current.clone(), announcing) {
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

    fn traffic_end_any_no_return(&mut self, now_unix: i64) -> Effects {
        self.traffic_end_any(now_unix)
    }

    fn traffic_close(&mut self, now_unix: i64, switch_back: bool) -> Effects {
        let mut fx = Effects::default();
        if let Some(mut a) = self.state.traffic_active.take() {
            a.ended_at = Some(now_unix);
            self.state.traffic_history.insert(0, a);
            self.state.traffic_history.truncate(TRAFFIC_HISTORY_MAX);
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{DataDirs, Presets, Settings};
    use dab_api::{Command, ServiceInfo, ServiceSlot};
    use std::time::Instant;

    fn svc(sid: u32, name: &str, sub_ch: u8) -> ServiceInfo {
        ServiceInfo { sid, scids: 0, name: name.into(), is_audio: true, is_primary: true, sub_ch, bitrate_kbps: 88, pty: 0 }
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
        a.state.current = Some(crate::state::CurrentService { sid: 0xD395, scids: 0, codec: None, stereo: true });
        a
    }

    fn ann(sid: u32, active: bool, sub_ch: u8) -> Event {
        Event::Announcement { kind: 0x0002, sub_ch, active, sid, cluster: 4 }
    }

    #[test]
    fn announcement_is_listed_and_moves_to_history_when_it_ends() {
        let mut a = app(false);
        let fx = a.handle_event(&ann(0xD395, true, 3), Instant::now());
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::Traffic { active: Some(_), .. })));
        let act = a.state.traffic_active.clone().expect("laufende Durchsage");
        assert_eq!(act.announcing_service.as_deref(), Some("WDR 2 RHEINLAND"));
        assert_eq!(act.services, vec!["WDR 5".to_string()]);
        assert!(!act.switched);
        assert!(a.state.traffic_supported, "aktueller Dienst gehoert zum Cluster");
        // Weiterer Dienst desselben Clusters: kein zweiter Eintrag.
        a.handle_event(&ann(0xD392, true, 3), Instant::now());
        assert!(a.state.traffic_history.is_empty());
        assert_eq!(a.state.traffic_active.as_ref().unwrap().services.len(), 2);
        // Ende.
        a.handle_event(&ann(0xD395, false, 3), Instant::now());
        assert!(a.state.traffic_active.is_none());
        assert_eq!(a.state.traffic_history.len(), 1);
        assert!(a.state.traffic_history[0].ended_at.is_some());
        // Ohne Autoswitch nie ein SelectService.
        assert!(!fx.commands.iter().any(|c| matches!(c, Command::SelectService { .. })));
    }

    #[test]
    fn autoswitch_goes_to_the_announcing_service_and_back() {
        let mut a = app(true);
        let fx = a.handle_event(&ann(0xD395, true, 3), Instant::now());
        assert!(fx.commands.iter().any(|c| matches!(c, Command::SelectService { sid: 0xD392, .. })), "auf WDR 2 (SubCh 3) umschalten: {:?}", fx.commands);
        assert!(a.state.traffic_active.as_ref().unwrap().switched);
        // Der Kern startet den Zieldienst.
        a.handle_event(&Event::ServiceStarted { slot: ServiceSlot::Primary, sid: 0xD392, scids: 0, codec: dab_api::Codec::Mp2 { sample_rate: 48000 }, stereo: true }, Instant::now());
        let fx = a.handle_event(&ann(0xD395, false, 3), Instant::now());
        assert!(fx.commands.iter().any(|c| matches!(c, Command::SelectService { sid: 0xD395, .. })), "zurueck zu WDR 5: {:?}", fx.commands);
        assert!(a.state.traffic_active.is_none());
    }

    #[test]
    fn no_autoswitch_while_recording_or_for_other_services() {
        let mut a = app(true);
        a.state.recording = true;
        let fx = a.handle_event(&ann(0xD395, true, 3), Instant::now());
        assert!(!fx.commands.iter().any(|c| matches!(c, Command::SelectService { .. })));
        assert!(a.state.traffic_active.is_some(), "protokolliert wird trotzdem");
        a.handle_event(&ann(0xD395, false, 3), Instant::now());

        // Durchsage fuer einen anderen Dienst als den laufenden: nur Liste.
        a.state.recording = false;
        let fx = a.handle_event(&ann(0xD392, true, 3), Instant::now());
        assert!(!fx.commands.iter().any(|c| matches!(c, Command::SelectService { .. })));
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
}
