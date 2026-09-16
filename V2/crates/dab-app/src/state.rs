//! App-Zustand ("Truth", Entscheidung 14 / Analyse 4.4): wird aus den
//! Kern-Ereignissen im Rust-Thread gepflegt und dem Frontend als Snapshot
//! (`get_state`) geliefert; danach spiegelt das Frontend dieselben
//! Delta-Ereignisse. Alles hier ist reine Datenhaltung ohne Nebenwirkungen.

use dab_api::{Codec, Event, EwsPhase, Gain, ServiceInfo, ServiceSlot, SourceKind};
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
pub struct DeviceState {
    /// "hackrf", "rtlsdr" oder "file" (Schluessel fuer die Gain-Saetze).
    pub kind: String,
    pub name: String,
    pub serial: String,
    /// Referenztakt ("extern"/"intern"), sobald der Kern ihn gemeldet hat.
    #[serde(default)]
    pub clock: Option<String>,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct EnsembleState {
    pub eid: u16,
    pub name: String,
    pub channel: String,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct CurrentService {
    pub sid: u32,
    pub scids: u8,
    pub codec: Option<Codec>,
    pub stereo: bool,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
pub struct DlPlusState {
    pub item_running: bool,
    pub item_toggle: bool,
    /// (content_type, text) – alle Tags des letzten DL+-Kommandos.
    pub tags: Vec<(u8, String)>,
}

impl DlPlusState {
    pub fn tag(&self, content_type: u8) -> Option<&str> {
        self.tags.iter().find(|(t, _)| *t == content_type).map(|(_, s)| s.as_str())
    }
    /// ITEM.TITLE (1)
    pub fn title(&self) -> Option<&str> { self.tag(1) }
    /// ITEM.ARTIST (4)
    pub fn artist(&self) -> Option<&str> { self.tag(4) }
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct SlideState {
    pub sid: u32,
    pub mime: String,
    pub name: String,
    pub data_b64: String,
    /// Unix-Zeit (s) des Eingangs; das Frontend zeigt "MOT" gruen, solange frisch.
    pub received_at: i64,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
pub struct ScanState {
    pub active: bool,
    pub channel: String,
    pub index: u16,
    pub total: u16,
    pub results: Vec<ScanResultState>,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct ScanResultState {
    pub channel: String,
    pub eid: Option<u16>,
    pub ensemble: Option<String>,
    pub services: Vec<ServiceInfo>,
    pub snr: f32,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct AlertState {
    pub phase: EwsPhase,
    pub sub_ch: u8,
    pub stage: u8,
    /// Rohes Status-Byte der FIG 0/15 (Warntag 2026: 0x01).
    pub stage_raw: u8,
    pub iid: u16,
    pub locations: Vec<String>,
    /// Ortscodes uebersetzt (Mittelpunkt, Entfernung/Richtung von zu Hause);
    /// wird nach `apply()` in `App::handle_event` befuellt (braucht die
    /// Heimatkoordinaten aus den Settings, die hier nicht vorliegen).
    pub location_info: Vec<crate::ews_location::LocationInfo>,
    pub is_test: bool,
    /// Geofencing-Urteil des Kerns (`Event::EwsAlert.relevant`, ETSI TS 104 089
    /// Klausel 7.5/7.6): `None` = keine Heimatkoordinaten im Kern, jeder Alarm
    /// gilt als relevant; `Some(true)` = ein Ortscode deckt den eigenen Standort
    /// ab; `Some(false)` = keiner (z. B. der Eiffelturm-Testalarm, aus
    /// Deutschland gesehen). Nur zur Anzeige/UI-Steuerung - die Umschaltung
    /// entscheidet der Kern selbst, hier wird nichts nachgerechnet.
    pub relevant: Option<bool>,
    /// Vom Nutzer per "Verstanden" quittiert (Banner aus, Alarm laeuft weiter).
    pub dismissed: bool,
}

/// Abgeschlossener Alarm, fuer die Sitzungs-Historie (Bugfixes.txt #10).
/// Nur im Speicher (wie der Timeshift-Ring), keine Datei wie beim EPG-Cache -
/// reicht, um einen Alarm nachtraeglich anzusehen, waehrend die App laeuft.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct EwsHistoryEntry {
    /// Unix-Zeit, zu der die "End"-Meldung kam.
    pub ended_at: i64,
    pub sub_ch: u8,
    pub stage: u8,
    pub stage_raw: u8,
    pub iid: u16,
    pub locations: Vec<String>,
    pub location_info: Vec<crate::ews_location::LocationInfo>,
    pub is_test: bool,
    /// Geofencing-Urteil des Kerns, siehe [`AlertState::relevant`].
    pub relevant: Option<bool>,
    /// Mitschnitt des Warndienstes (MP3, crate::traffic), falls aufgezeichnet.
    #[serde(default)]
    pub file: Option<String>,
}

/// Hoechstzahl der Eintraege in `AppState::ews_history`; aelteste fallen raus.
pub const EWS_HISTORY_MAX: usize = 20;

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct FileState {
    pub path: String,
    pub r#loop: bool,
    pub position_s: f64,
    pub length_s: f64,
    pub ended: bool,
}

/// Laufender Preset-Aufruf bzw. Wiederherstellung des letzten Dienstes.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct PendingState {
    /// Slot 0..9 bei Preset-Aufruf, None bei Start-Wiederherstellung.
    pub slot: Option<usize>,
    pub channel: String,
    pub name: String,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
#[serde(default)]
pub struct AppState {
    pub core_alive: bool,
    pub core_version: String,
    pub core_restarts: u32,
    pub device: Option<DeviceState>,
    pub device_error: Option<String>,
    pub channel: Option<String>,
    pub synced: bool,
    pub snr: f32,
    pub fic_ok: u16,
    pub fic_total: u16,
    pub ensemble: Option<EnsembleState>,
    /// Nach Name sortiert; `service_added` ersetzt Eintraege mit gleichem (sid, scids).
    pub services: Vec<ServiceInfo>,
    pub current: Option<CurrentService>,
    pub dls: String,
    pub dl_plus: Option<DlPlusState>,
    pub slide: Option<SlideState>,
    pub level: (f32, f32),
    pub gain: Gain,
    pub agc: bool,
    pub volume: u8,
    pub muted: bool,
    pub scan: ScanState,
    pub ews_present: bool,
    pub alert: Option<AlertState>,
    /// Dienst, von dem der Kern beim Alarm weggeschaltet hat (Hinweis in der UI).
    pub ews_switched_from: Option<u32>,
    pub recording: bool,
    pub file: Option<FileState>,
    pub audio_devices: Vec<String>,
    pub audio_device_current: Option<u32>,
    pub pending: Option<PendingState>,
    pub clock_utc: Option<i64>,
    pub log_tail: Vec<String>,
    /// Logo des aktuellen Dienstes (128x128) als `data:`-URL (crate::logos).
    pub logo_data_url: Option<String>,
    /// Laufende/naechste Sendung des aktuellen Dienstes (crate::epg).
    pub now_next: Option<crate::epg::NowNext>,
    /// TII-Sender im Nullsymbol, nach Staerke sortiert (crate::tii).
    pub tii: Vec<crate::tii::TiiSeen>,
    /// Debug-Panel: SNR-Verlauf, Fehlerzaehler, Frequenzversatz (crate::tii).
    pub debug: crate::tii::DebugState,
    /// Senderliste ueber alle Ensembles, sortiert Kanal/Ensemble/Name (crate::stations).
    pub stations: Vec<crate::stations::StationEntry>,
    /// Timeshift-Puffer des Primary-Slots (crate::timeshift, Entscheidung 4).
    pub timeshift: crate::timeshift::TimeshiftInfo,
    /// Vorschlagsliste der Musik-Trennung (crate::music, Entscheidungen 6, 7);
    /// hoechstens `crate::music::MUSIC_MAX` Eintraege, aelteste zuerst raus.
    pub music_candidates: Vec<dab_music::TrackCandidate>,
    /// Abgeschlossene Alarme dieser Sitzung, neueste zuerst (Bugfixes.txt #10,
    /// [`EWS_HISTORY_MAX`] Eintraege); nicht persistiert.
    pub ews_history: Vec<EwsHistoryEntry>,
    /// Verkehrs-/Sonderdurchsagen (crate::traffic): laufende Durchsage,
    /// beendete dieser Sitzung (neueste zuerst), ob der laufende Dienst zu
    /// einem Durchsage-Cluster gehoert.
    pub traffic_active: Option<crate::traffic::TrafficEntry>,
    pub traffic_history: Vec<crate::traffic::TrafficEntry>,
    pub traffic_supported: bool,
}

pub fn unix_now() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

impl AppState {
    /// Alle empfangsbezogenen Felder leeren (Kanalwechsel, Geraet zu).
    pub fn clear_reception(&mut self) {
        self.synced = false;
        self.snr = 0.0;
        self.fic_ok = 0;
        self.fic_total = 0;
        self.ensemble = None;
        self.services.clear();
        // Kanalwechsel/Geraet zu: der Kern leert den Timeshift-Ring (Entscheidung 4).
        self.timeshift.reset();
        self.clear_service();
    }

    pub fn clear_service(&mut self) {
        self.current = None;
        self.dls.clear();
        self.dl_plus = None;
        self.slide = None;
        self.level = (0.0, 0.0);
        self.logo_data_url = None;
        self.now_next = None;
    }

    pub fn service(&self, sid: u32, scids: u8) -> Option<&ServiceInfo> {
        self.services.iter().find(|s| s.sid == sid && s.scids == scids)
    }

    pub fn service_by_name(&self, name: &str) -> Option<&ServiceInfo> {
        let wanted = name.trim();
        self.services.iter().find(|s| s.name.trim().eq_ignore_ascii_case(wanted))
    }

    pub fn current_service(&self) -> Option<&ServiceInfo> {
        let c = self.current.as_ref()?;
        self.service(c.sid, c.scids)
    }

    pub fn upsert_service(&mut self, service: ServiceInfo) {
        if let Some(e) = self.services.iter_mut().find(|s| s.sid == service.sid && s.scids == service.scids) {
            *e = service;
        } else {
            self.services.push(service);
        }
        self.services.sort_by(|a, b| a.name.trim().to_lowercase().cmp(&b.name.trim().to_lowercase()).then(a.scids.cmp(&b.scids)));
    }

    /// Hoerbare Dienste in Listenreihenfolge (fuer Prev/Next).
    pub fn audio_services(&self) -> impl Iterator<Item = &ServiceInfo> {
        self.services.iter().filter(|s| s.is_audio)
    }

    pub fn note_source(&mut self, source: &SourceKind) {
        let kind = match source {
            SourceKind::HackRf { .. } => "hackrf",
            SourceKind::RtlSdr { .. } => "rtlsdr",
            SourceKind::File { .. } => "file",
        };
        self.device = Some(DeviceState { kind: kind.into(), ..Default::default() });
        self.device_error = None;
        self.file = match source {
            SourceKind::File { path, r#loop, .. } => Some(FileState {
                path: path.display().to_string(),
                r#loop: *r#loop,
                position_s: 0.0,
                length_s: 0.0,
                ended: false,
            }),
            _ => None,
        };
    }

    pub fn device_kind(&self) -> Option<&str> {
        self.device.as_ref().map(|d| d.kind.as_str())
    }

    pub fn is_file_source(&self) -> bool {
        self.device_kind() == Some("file")
    }

    /// Wendet ein Kern-Ereignis auf den Zustand an. Gibt `true` zurueck, wenn
    /// sich etwas Sichtbares geaendert hat (Latest-wins-Werte zaehlen nicht).
    pub fn apply(&mut self, ev: &Event) {
        match ev {
            Event::Ready { core_version, .. } => {
                self.core_alive = true;
                self.core_version = core_version.clone();
            }
            Event::DeviceOpened { name, serial, .. } => {
                let kind = self.device_kind().unwrap_or("hackrf").to_string();
                self.device = Some(DeviceState { kind, name: name.clone(), serial: serial.clone(), clock: None });
                self.device_error = None;
            }
            Event::ClockSource { source } => {
                if let Some(d) = self.device.as_mut() {
                    d.clock = Some(source.clone());
                }
            }
            Event::DeviceClosed => {
                self.device = None;
                self.file = None;
                self.clear_reception();
            }
            Event::DeviceError { message } => self.device_error = Some(message.clone()),
            Event::GainChanged { lna, vga, amp, agc } => {
                self.gain = Gain { lna: *lna, vga: *vga, amp: *amp };
                self.agc = *agc;
            }
            Event::FileProgress { position_s, length_s } => {
                if let Some(f) = self.file.as_mut() {
                    f.position_s = *position_s;
                    f.length_s = *length_s;
                }
            }
            Event::FileEnded => {
                if let Some(f) = self.file.as_mut() {
                    f.ended = true;
                }
            }
            Event::Synced { synced } => self.synced = *synced,
            Event::NoSignal { channel } => {
                self.synced = false;
                self.channel = Some(channel.clone());
            }
            Event::Snr { db } => self.snr = *db,
            Event::FicQuality { ok, total } => {
                self.fic_ok = *ok;
                self.fic_total = *total;
            }
            Event::EnsembleFound { eid, name, channel } => {
                let changed = self.ensemble.as_ref().map(|e| e.eid != *eid).unwrap_or(true);
                if changed {
                    self.services.clear();
                    self.clear_service();
                }
                self.ensemble = Some(EnsembleState { eid: *eid, name: name.clone(), channel: channel.clone() });
                self.channel = Some(channel.clone());
            }
            Event::ServiceAdded { service } => self.upsert_service(service.clone()),
            Event::EnsembleReconfigured => {
                self.services.clear();
            }
            Event::ClockTime { unix_utc, .. } => self.clock_utc = Some(*unix_utc),
            Event::ServiceStarted { slot: ServiceSlot::Primary, sid, scids, codec, stereo } => {
                let same = self.current.as_ref().map(|c| c.sid == *sid && c.scids == *scids).unwrap_or(false);
                if !same {
                    self.clear_service();
                }
                self.current = Some(CurrentService { sid: *sid, scids: *scids, codec: Some(codec.clone()), stereo: *stereo });
            }
            Event::ServiceStopped { slot: ServiceSlot::Primary, sid } => {
                if self.current.as_ref().map(|c| c.sid == *sid).unwrap_or(false) {
                    self.clear_service();
                }
            }
            Event::Dls { slot: ServiceSlot::Primary, text, .. } => self.dls = text.clone(),
            Event::DlPlus { slot: ServiceSlot::Primary, item_toggle, item_running, tags, .. } => {
                self.dl_plus = Some(DlPlusState { item_running: *item_running, item_toggle: *item_toggle, tags: tags.clone() });
            }
            Event::MotSlide { slot: ServiceSlot::Primary, sid, mime, name, data_b64 } => {
                self.slide = Some(SlideState {
                    sid: *sid,
                    mime: mime.clone(),
                    name: name.clone(),
                    data_b64: data_b64.clone(),
                    received_at: unix_now(),
                });
            }
            Event::AudioLevel { left, right } => self.level = (*left, *right),
            Event::AudioDevices { names, current } => {
                self.audio_devices = names.clone();
                self.audio_device_current = *current;
            }
            Event::EwsPresent => self.ews_present = true,
            Event::EwsAlert { phase, sub_ch, stage, stage_raw, iid, locations, is_test, relevant } => {
                if *phase == EwsPhase::End {
                    if let Some(a) = self.alert.take() {
                        self.ews_history.insert(0, EwsHistoryEntry {
                            ended_at: unix_now(),
                            sub_ch: a.sub_ch,
                            stage: a.stage,
                            stage_raw: a.stage_raw,
                            iid: a.iid,
                            locations: a.locations,
                            location_info: a.location_info,
                            is_test: a.is_test,
                            relevant: a.relevant,
                            file: None,
                        });
                        self.ews_history.truncate(EWS_HISTORY_MAX);
                    }
                    self.ews_switched_from = None;
                } else {
                    let dismissed = self
                        .alert
                        .as_ref()
                        .map(|a| a.dismissed && a.iid == *iid && a.sub_ch == *sub_ch)
                        .unwrap_or(false);
                    self.alert = Some(AlertState {
                        phase: *phase,
                        sub_ch: *sub_ch,
                        stage: *stage,
                        stage_raw: *stage_raw,
                        iid: *iid,
                        locations: locations.clone(),
                        location_info: Vec::new(),
                        is_test: *is_test,
                        relevant: *relevant,
                        dismissed,
                    });
                }
            }
            Event::EwsSwitched { from_sid, .. } => self.ews_switched_from = *from_sid,
            // `recording_state`: NICHT hier blind uebernehmen - der Kern meldet
            // darunter auch Export-Enden und das Ende einer verdraengten
            // Kettenaufnahme. `App::recording_on_event` (recording.rs) ordnet
            // das Ereignis ueber den Dateipfad zu und setzt `recording` selbst
            // (Review 16.09.2026, Befund 1).
            Event::ScanProgress { channel, index, total } => {
                if !self.scan.active {
                    self.scan.results.clear();
                }
                // Anzeige je Kanal frisch: Ensemble/Dienste des vorigen Kanals weg.
                self.clear_reception();
                self.scan.active = true;
                self.scan.channel = channel.clone();
                self.scan.index = *index;
                self.scan.total = *total;
                self.channel = Some(channel.clone());
            }
            Event::ScanResult { channel, eid, ensemble, services, snr } => {
                self.scan.results.retain(|r| r.channel != *channel);
                self.scan.results.push(ScanResultState {
                    channel: channel.clone(),
                    eid: *eid,
                    ensemble: ensemble.clone(),
                    services: services.clone(),
                    snr: *snr,
                });
            }
            Event::ScanFinished => self.scan.active = false,
            Event::Log { level, text } => {
                if matches!(level, dab_api::LogLevel::Error | dab_api::LogLevel::Warn) {
                    self.log_tail.push(text.clone());
                    if self.log_tail.len() > 20 {
                        self.log_tail.remove(0);
                    }
                }
            }
            Event::StateSnapshot { state } => {
                self.gain = state.gain;
                self.agc = state.agc;
                self.synced = state.synced;
                self.volume = state.volume_percent;
                self.muted = state.muted;
                self.recording = state.recording;
                if let Some(ch) = &state.channel {
                    self.channel = Some(ch.clone());
                }
                if let Some((eid, name)) = &state.ensemble {
                    let channel = state.channel.clone().unwrap_or_default();
                    self.ensemble = Some(EnsembleState { eid: *eid, name: name.clone(), channel });
                }
                for s in &state.services {
                    self.upsert_service(s.clone());
                }
                if let Some((sid, scids)) = state.primary {
                    if self.current.is_none() {
                        self.current = Some(CurrentService { sid, scids, codec: None, stereo: false });
                    }
                }
            }
            Event::Exiting { .. } => {
                self.core_alive = false;
                self.device = None;
                self.scan.active = false;
                self.clear_reception();
            }
            _ => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn svc(sid: u32, name: &str) -> ServiceInfo {
        ServiceInfo { sid, scids: 0, name: name.into(), is_audio: true, is_primary: true, sub_ch: 1, bitrate_kbps: 96, pty: 0 }
    }

    #[test]
    fn service_added_replaces_and_sorts() {
        let mut st = AppState::default();
        st.apply(&Event::EnsembleFound { eid: 0x10BC, name: "DR Deutschland".into(), channel: "5C".into() });
        st.apply(&Event::ServiceAdded { service: svc(2, "Dlf Kultur") });
        st.apply(&Event::ServiceAdded { service: svc(1, "Dlf") });
        let mut again = svc(2, "Dlf Kultur");
        again.pty = 7;
        st.apply(&Event::ServiceAdded { service: again });
        assert_eq!(st.services.len(), 2);
        assert_eq!(st.services[0].name, "Dlf");
        assert_eq!(st.services[1].pty, 7);
        assert_eq!(st.channel.as_deref(), Some("5C"));
    }

    #[test]
    fn alert_phases_and_dismiss() {
        let mut st = AppState::default();
        let alert = |phase| Event::EwsAlert { phase, sub_ch: 1, stage: 1, stage_raw: 0x81, iid: 7, locations: vec![], is_test: false, relevant: None };
        st.apply(&alert(EwsPhase::Trigger));
        assert!(st.alert.is_some());
        st.alert.as_mut().unwrap().dismissed = true;
        st.apply(&alert(EwsPhase::Sustain));
        assert!(st.alert.as_ref().unwrap().dismissed, "Quittierung bleibt fuer denselben Alarm");
        st.apply(&alert(EwsPhase::End));
        assert!(st.alert.is_none());
    }

    #[test]
    fn primary_only_for_pad() {
        let mut st = AppState::default();
        st.apply(&Event::Dls { slot: ServiceSlot::Background, sid: 1, text: "bg".into() });
        assert_eq!(st.dls, "");
        st.apply(&Event::Dls { slot: ServiceSlot::Primary, sid: 1, text: "fg".into() });
        assert_eq!(st.dls, "fg");
    }
}
