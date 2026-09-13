//! Musik-Trennung (M4b, Entscheidungen 6, 7; Plan `M4b-Musiktrennung-Plan.md`
//! Abschnitt 2.2/2.3).
//!
//! Der [`MusicDetector`] hoert auf dieselben Kern-Ereignisse wie
//! [`crate::state::AppState::apply`] und schneidet daraus eine Vorschlagsliste:
//!
//! - `timeshift_state` liefert mit 2 Hz den Schreibzeiger `frame_index`; der
//!   zuletzt gesehene Wert wird an jede DL+/DLS-Aenderung geheftet (Aufloesung
//!   0,5 s, fuer Vor-/Nachlauf von Sekunden ausreichend). **Kein eigenes
//!   Protokollfeld noetig.**
//! - `dl_plus` (Primary): Item-Toggle-Flanke bzw. geaenderter `ITEM.TITLE`(1)/
//!   `ITEM.ARTIST`(4) oeffnet einen Kandidaten, `item_running == false` oder
//!   die naechste Flanke schliesst ihn (Spike 3: 10/14 Dienste im Bundesmux).
//! - `dls` (Primary) als Ersatzweg fuer Dienste, die nach [`DLS_FALLBACK_S`]
//!   noch nie DL+ gesendet haben (Spike 3: DokDeb, ERF Plus, radio horeb):
//!   Textwechsel = neuer Kandidat, Trennung an " - " nach v1-Konvention.
//! - Senderwechsel oder geleerter Ring (`buffered_s` faellt auf 0, z. B. nach
//!   der EWS-Umschaltung) verwerfen den offenen Kandidaten: der Ton steht im
//!   Ring nicht mehr zuverlaessig.
//!
//! Geschlossene Kandidaten landen in `AppState::music_candidates` (hoechstens
//! [`MUSIC_MAX`], aelteste zuerst raus) – oder gehen bei
//! `Settings::music_auto_save` sofort als Export an den Kern (Entscheidung 7).
//! Nichts davon wird gespeichert: der Ring ist ohnehin fluechtig.

use crate::app::{App, AppError, AppEvent, Effects};
use dab_api::{Command, Event, Id3Tags, RecFormat, ServiceSlot};
use dab_music::{split_dls, SplitConfig, TrackCandidate, FRAME_S};
use std::path::PathBuf;

/// Hoechstzahl der Vorschlaege (Plan 2.2); aelteste fallen hinten raus.
pub const MUSIC_MAX: usize = 20;
/// Erst nach dieser Zeit ohne ein einziges `dl_plus` greift der DLS-Ersatzweg.
pub const DLS_FALLBACK_S: i64 = 30;

/// Erkennung der Titelgrenzen. Haelt nur Zwischenwerte; die Ergebnisliste
/// liegt in [`crate::AppState`].
#[derive(Debug)]
pub struct MusicDetector {
    /// Vor-/Nachlauf und Laengenpruefung (Entscheidung 7).
    pub cfg: SplitConfig,
    /// Letzter Schreibzeiger aus `timeshift_state`.
    pub frame: u64,
    /// Laufender Kandidat (noch ohne `end_frame`).
    pub open: Option<TrackCandidate>,
    /// Hat der laufende Dienst jemals DL+ geliefert?
    dlplus_seen: bool,
    /// Unix-Zeit, seit der dieser Dienst laeuft (Frist fuer den DLS-Ersatzweg).
    service_since: i64,
    last_title: String,
    last_artist: String,
    last_dls: String,
    last_buffered_s: f64,
}

impl Default for MusicDetector {
    fn default() -> Self {
        Self {
            cfg: SplitConfig::default(),
            frame: 0,
            open: None,
            dlplus_seen: false,
            service_since: 0,
            last_title: String::new(),
            last_artist: String::new(),
            last_dls: String::new(),
            last_buffered_s: 0.0,
        }
    }
}

impl MusicDetector {
    /// Neuer Dienst: alles vergessen (der Ring beginnt ebenfalls neu).
    fn reset_service(&mut self, now_unix: i64) {
        self.open = None;
        self.dlplus_seen = false;
        self.service_since = now_unix;
        self.last_title.clear();
        self.last_artist.clear();
        self.last_dls.clear();
    }

    /// Ersatzweg erlaubt? (Dienst laeuft lange genug und hat nie DL+ gesendet.)
    fn dls_fallback_active(&self, now_unix: i64) -> bool {
        !self.dlplus_seen && self.service_since > 0 && now_unix - self.service_since >= DLS_FALLBACK_S
    }
}

/// Ergebnis eines Ereignisses fuer die App-Schicht.
enum Step {
    None,
    /// Kandidat fertig (schon geschlossen).
    Closed(TrackCandidate),
    /// Offener Kandidat verworfen (Log-Zeile, kein Eintrag).
    Dropped(&'static str),
}

impl MusicDetector {
    /// Ein Kern-Ereignis verarbeiten. `now_unix` nur fuer die 30-s-Frist.
    fn step(&mut self, ev: &Event, now_unix: i64, station: Option<&str>) -> Step {
        match ev {
            Event::TimeshiftState { buffered_s, frame_index, .. } => {
                if *frame_index > 0 {
                    self.frame = *frame_index;
                }
                let emptied = *buffered_s <= 0.0 && self.last_buffered_s > 0.0;
                self.last_buffered_s = *buffered_s;
                if emptied && self.open.take().is_some() {
                    return Step::Dropped("Timeshift-Puffer geleert");
                }
                Step::None
            }
            Event::ServiceStarted { slot: ServiceSlot::Primary, .. } | Event::ServiceStopped { slot: ServiceSlot::Primary, .. } => {
                let had = self.open.is_some();
                self.reset_service(now_unix);
                self.last_buffered_s = 0.0;
                if had {
                    Step::Dropped("Senderwechsel")
                } else {
                    Step::None
                }
            }
            Event::DeviceClosed | Event::Exiting { .. } => {
                let had = self.open.is_some();
                self.reset_service(now_unix);
                self.last_buffered_s = 0.0;
                if had {
                    Step::Dropped("Kern beendet")
                } else {
                    Step::None
                }
            }
            Event::DlPlus { slot: ServiceSlot::Primary, item_running, tags, .. } => {
                self.dlplus_seen = true;
                // Ohne Schreibzeiger laeuft kein Ring: dann gibt es nichts zu schneiden.
                if self.frame == 0 {
                    return Step::None;
                }
                let tag = |ct: u8| tags.iter().find(|(t, _)| *t == ct).map(|(_, s)| s.trim().to_string()).unwrap_or_default();
                let title = tag(dab_music::dlplus::ITEM_TITLE);
                let artist = tag(dab_music::dlplus::ITEM_ARTIST);
                // Manche Encoder (z. B. Kommerzsender) kippen das Toggle-Bit auch
                // ohne Inhaltswechsel (Retransmission/Encoder-Eigenheit); massgeblich
                // ist deshalb ausschliesslich der tatsaechlich geaenderte Text, sonst
                // zerfaellt ein einzelner Titel in mehrere Vorschlagslisten-Eintraege.
                let text_changed = title != self.last_title || artist != self.last_artist;
                self.last_title = title.clone();
                self.last_artist = artist.clone();

                // IR = 0: Moderation/Nachrichten – laufenden Titel beenden, keinen neuen oeffnen.
                if !*item_running {
                    return match self.open.take() {
                        Some(mut c) => {
                            c.close(self.frame);
                            Step::Closed(c)
                        }
                        None => Step::None,
                    };
                }
                if title.is_empty() && artist.is_empty() {
                    return Step::None;
                }
                let is_new = match &self.open {
                    Some(_) => text_changed,
                    None => true,
                };
                if !is_new {
                    return Step::None;
                }
                let previous = self.open.take().map(|mut c| {
                    c.close(self.frame);
                    c
                });
                let mut next = TrackCandidate::opened(
                    self.frame,
                    Some(title).filter(|s| !s.is_empty()),
                    Some(artist).filter(|s| !s.is_empty()),
                    station.map(|s| s.to_string()),
                );
                next.item_running = true;
                self.open = Some(next);
                match previous {
                    Some(c) => Step::Closed(c),
                    None => Step::None,
                }
            }
            Event::Dls { slot: ServiceSlot::Primary, text, .. } => {
                if self.frame == 0 || !self.dls_fallback_active(now_unix) {
                    return Step::None;
                }
                let text = text.trim().to_string();
                if text == self.last_dls {
                    return Step::None;
                }
                self.last_dls = text.clone();
                let (artist, title) = split_dls(&text);
                if title.is_none() && artist.is_none() {
                    return Step::None;
                }
                let previous = self.open.take().map(|mut c| {
                    c.close(self.frame);
                    c
                });
                let mut next = TrackCandidate::opened(self.frame, title, artist, station.map(|s| s.to_string()));
                next.from_dls = true;
                self.open = Some(next);
                match previous {
                    Some(c) => Step::Closed(c),
                    None => Step::None,
                }
            }
            _ => Step::None,
        }
    }
}

impl App {
    /// Aus [`App::handle_event`](crate::App::handle_event): Ereignis in die
    /// Erkennung geben und fertige Kandidaten einsortieren bzw. exportieren.
    pub fn music_on_event(&mut self, ev: &Event, now_unix: i64) -> Effects {
        let station = self.current_service_name();
        let step = self.music.step(ev, now_unix, station.as_deref());
        match step {
            Step::None => Effects::default(),
            Step::Dropped(why) => {
                log::info!("Musik: offener Kandidat verworfen ({why})");
                Effects::default()
            }
            Step::Closed(c) => self.music_finish(c),
        }
    }

    /// Name des laufenden Dienstes (fuer Album-Tag und Dateiname).
    pub(crate) fn current_service_name(&self) -> Option<String> {
        let c = self.state.current.as_ref()?;
        Some(
            self.state
                .service(c.sid, c.scids)
                .map(|s| s.name.trim().to_string())
                .unwrap_or_else(|| format!("{:04X}", c.sid)),
        )
    }

    /// Fertigen Kandidaten einsortieren: automatisch sichern (Entscheidung 7)
    /// oder in die Vorschlagsliste legen.
    fn music_finish(&mut self, c: TrackCandidate) -> Effects {
        let dur = c.duration_s().unwrap_or(0.0);
        if self.settings.music_auto_save && c.length_ok(&self.music.cfg) {
            match self.music_export_candidate(&c) {
                Ok((path, fx)) => {
                    log::info!("Musik: \"{}\" ({dur:.0} s) wird gesichert: {}", c.label(), path.display());
                    return fx;
                }
                Err(e) => log::warn!("Musik: Export von \"{}\" nicht moeglich ({e})", c.label()),
            }
        }
        if !c.length_ok(&self.music.cfg) {
            // Bleibt in der Liste (der Nutzer entscheidet), wird aber nie
            // automatisch gesichert: min_len_s/max_len_s sind nur Plausibilitaet.
            log::debug!("Musik: \"{}\" ausserhalb der Laengenpruefung ({dur:.0} s)", c.label());
        }
        self.state.music_candidates.push(c);
        while self.state.music_candidates.len() > MUSIC_MAX {
            self.state.music_candidates.remove(0);
        }
        let mut fx = Effects::default();
        fx.events.push(AppEvent::MusicCandidates { candidates: self.state.music_candidates.clone() });
        fx
    }

    pub fn music_candidates(&self) -> &[TrackCandidate] {
        &self.state.music_candidates
    }

    /// Vor-/Nachlauf eines Kandidaten verschieben (Stepper im Musik-Panel,
    /// Plan M4b Abschnitt 5). Wirkt nur auf den naechsten Export dieses
    /// Kandidaten (`to_export_range`), nicht rueckwirkend auf bereits
    /// uebernommene.
    pub fn music_adjust(&mut self, index: usize, pre_roll_s: f64, post_roll_s: f64) -> Result<Effects, AppError> {
        let c = self.state.music_candidates.get_mut(index).ok_or(AppError::Slot)?;
        c.set_roll(pre_roll_s, post_roll_s);
        let mut fx = Effects::default();
        fx.events.push(AppEvent::MusicCandidates { candidates: self.state.music_candidates.clone() });
        Ok(fx)
    }

    /// Vorschlagsliste leeren.
    pub fn music_clear(&mut self) -> Effects {
        self.state.music_candidates.clear();
        let mut fx = Effects::default();
        fx.events.push(AppEvent::MusicCandidates { candidates: Vec::new() });
        fx
    }

    /// "uebernehmen": Kandidat aus der Liste als MP3 aus dem Ring sichern.
    /// Der Eintrag bleibt als erledigt (`taken`) stehen; Fertigmeldung kommt
    /// wie bei der Aufnahme als `recording_state` mit diesem Pfad.
    pub fn music_export(&mut self, index: usize) -> Result<(PathBuf, Effects), AppError> {
        let c = self.state.music_candidates.get(index).cloned().ok_or(AppError::Slot)?;
        let (path, mut fx) = self.music_export_candidate(&c)?;
        if let Some(e) = self.state.music_candidates.get_mut(index) {
            e.taken = true;
        }
        fx.events.push(AppEvent::MusicCandidates { candidates: self.state.music_candidates.clone() });
        Ok((path, fx))
    }

    /// Gemeinsamer Weg fuer "uebernehmen" und "automatisch speichern".
    fn music_export_candidate(&mut self, c: &TrackCandidate) -> Result<(PathBuf, Effects), AppError> {
        if self.state.is_file_source() {
            return Err(AppError::Other("timeshift unavailable".into()));
        }
        let (from_s, to_s) = c
            .to_export_range(self.music.frame, &self.music.cfg)
            .ok_or_else(|| AppError::Other("range invalid".into()))?;
        let station = c.station.clone().or_else(|| self.current_service_name()).unwrap_or_default();
        let dir = self.recording_dir();
        if let Err(e) = std::fs::create_dir_all(&dir) {
            return Err(AppError::Other(format!("{}: {e}", dir.display())));
        }
        let now = chrono::Local::now();
        let path = dir.join(music_file_name(&now, &station, c));
        let id3 = Id3Tags {
            title: c.title.clone(),
            artist: c.artist.clone(),
            album: Some(station).filter(|s| !s.is_empty()),
            date: Some(now.format("%Y-%m-%d").to_string()),
            cover_png_b64: self.music_cover_png_b64(),
        };
        let format = RecFormat::Mp3 { kbps: self.settings.music_mp3_kbps, id3: Some(id3) };
        let mut fx = Effects::default();
        fx.commands.push(Command::ExportTimeshiftRange { from_s, to_s, path: path.clone(), format });
        Ok((path, fx))
    }

    /// Cover fuer das APIC-Frame: aktuelles Diaschau-Bild, sonst das Logo aus
    /// dem Cache (crate::logos). Der Kern schreibt nur PNG (`cover_png_b64`),
    /// darum werden JPEG-Bilder uebersprungen statt falsch ausgezeichnet.
    fn music_cover_png_b64(&self) -> Option<String> {
        if let Some(sl) = &self.state.slide {
            if sl.mime.eq_ignore_ascii_case("image/png") && !sl.data_b64.is_empty() {
                return Some(sl.data_b64.clone());
            }
        }
        let eid = self.state.ensemble.as_ref()?.eid;
        let c = self.state.current.as_ref()?;
        let url = self.logos.data_url(eid, c.sid, crate::LogoSize::Large)?;
        url.strip_prefix("data:image/png;base64,").map(|s| s.to_string())
    }
}

/// Dateiname wie bei der Aufnahme (crate::recording, v1-Schema), aber `.mp3`
/// und mit "Interpret - Titel" als Titelteil.
pub fn music_file_name(at: &chrono::DateTime<chrono::Local>, service: &str, c: &TrackCandidate) -> String {
    let base = crate::recording::file_name(at, service, &c.label());
    match base.strip_suffix(".wav") {
        Some(stem) => format!("{stem}.mp3"),
        None => format!("{base}.mp3"),
    }
}

/// Alter eines Kandidaten in Sekunden (fuer "vor 3:20" in der Oberflaeche).
pub fn age_s(start_frame: u64, current_frame: u64) -> f64 {
    current_frame.saturating_sub(start_frame) as f64 * FRAME_S
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{DataDirs, Presets, Settings};
    use dab_api::{Codec, ServiceInfo, TimeshiftMode};
    use std::time::Instant;

    fn app() -> App {
        let nanos = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        let tmp = std::env::temp_dir().join(format!("dabclassic-music-{}-{nanos}", std::process::id()));
        let mut a = App::with(DataDirs::with_root(&tmp, true), Settings::default(), Presets::default());
        a.state.device = Some(crate::state::DeviceState { kind: "hackrf".into(), ..Default::default() });
        a.state.channel = Some("5C".into());
        a.state.ensemble = Some(crate::state::EnsembleState { eid: 0x10BC, name: "DR Deutschland".into(), channel: "5C".into() });
        a.state.services.push(ServiceInfo { sid: 0xD210, scids: 0, name: "Dlf".into(), is_audio: true, is_primary: true, sub_ch: 1, bitrate_kbps: 104, pty: 0 });
        a.state.current = Some(crate::state::CurrentService { sid: 0xD210, scids: 0, codec: None, stereo: true });
        a.music.service_since = 1000;
        a
    }

    fn ts(frame: u64) -> Event {
        Event::TimeshiftState { mode: TimeshiftMode::Playing, buffered_s: 1800.0, offset_s: 0.0, capacity_s: 3600.0, frame_index: frame, live_unix: 1_789_194_000 }
    }

    fn dlp(toggle: bool, running: bool, title: &str, artist: &str) -> Event {
        Event::DlPlus {
            slot: ServiceSlot::Primary,
            sid: 0xD210,
            item_toggle: toggle,
            item_running: running,
            tags: vec![(1, title.into()), (4, artist.into())],
        }
    }

    fn dls(text: &str) -> Event {
        Event::Dls { slot: ServiceSlot::Primary, sid: 0xD210, text: text.into() }
    }

    /// Ereignis durch die volle App-Schicht (wie im Betrieb).
    fn feed(a: &mut App, ev: &Event, now_unix: i64) -> Effects {
        a.state.apply(ev);
        a.music_on_event(ev, now_unix)
    }

    fn cleanup(a: &App) {
        let _ = std::fs::remove_dir_all(&a.dirs.root);
    }

    #[test]
    fn dlplus_toggle_creates_candidates() {
        let mut a = app();
        feed(&mut a, &ts(1_000), 2000);
        feed(&mut a, &dlp(false, true, "Enjoy the Silence", "Depeche Mode"), 2000);
        assert!(a.music.open.is_some(), "erster Titel laeuft");
        assert!(a.state.music_candidates.is_empty());
        // 200 s spaeter (1000 + 200/0.024 Rahmen) kippt der Toggle
        feed(&mut a, &ts(9_333), 2200);
        let fx = feed(&mut a, &dlp(true, true, "Blue Monday", "New Order"), 2200);
        assert_eq!(a.state.music_candidates.len(), 1);
        let c = &a.state.music_candidates[0];
        assert_eq!(c.title.as_deref(), Some("Enjoy the Silence"));
        assert_eq!(c.artist.as_deref(), Some("Depeche Mode"));
        assert_eq!(c.station.as_deref(), Some("Dlf"));
        assert_eq!((c.start_frame, c.end_frame), (1_000, Some(9_333)));
        assert!((c.duration_s().unwrap() - 199.992).abs() < 0.01, "{:?}", c.duration_s());
        assert!(c.length_ok(&a.music.cfg));
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::MusicCandidates { .. })));
        assert!(a.music.open.is_some(), "zweiter Titel laeuft");
        cleanup(&a);
    }

    #[test]
    fn toggle_flap_without_text_change_stays_one_candidate() {
        // Manche Encoder (z. B. Kommerzsender) kippen das Toggle-Bit auch ohne
        // Inhaltswechsel; das darf keine neuen Eintraege erzeugen (Fehlerbild:
        // 4 Eintraege fuer ein einziges Lied).
        let mut a = app();
        feed(&mut a, &ts(1_000), 2000);
        feed(&mut a, &dlp(false, true, "It Must Be Love", "Madness"), 2000);
        feed(&mut a, &ts(2_000), 2024);
        feed(&mut a, &dlp(true, true, "It Must Be Love", "Madness"), 2024);
        feed(&mut a, &ts(3_000), 2048);
        feed(&mut a, &dlp(false, true, "It Must Be Love", "Madness"), 2048);
        feed(&mut a, &ts(4_000), 2072);
        feed(&mut a, &dlp(true, true, "It Must Be Love", "Madness"), 2072);
        assert!(a.state.music_candidates.is_empty(), "kein Titelwechsel, kein Eintrag");
        assert_eq!(a.music.open.as_ref().unwrap().start_frame, 1_000, "derselbe Kandidat laeuft weiter");
        // Echter Titelwechsel schliesst ihn weiterhin korrekt
        feed(&mut a, &ts(9_333), 2200);
        feed(&mut a, &dlp(true, true, "Blue Monday", "New Order"), 2200);
        assert_eq!(a.state.music_candidates.len(), 1);
        assert_eq!(a.state.music_candidates[0].title.as_deref(), Some("It Must Be Love"));
        cleanup(&a);
    }

    #[test]
    fn item_running_false_closes_and_blocks() {
        let mut a = app();
        feed(&mut a, &ts(1_000), 2000);
        feed(&mut a, &dlp(false, true, "Titel", "Artist"), 2000);
        feed(&mut a, &ts(6_000), 2120);
        // Moderation/Nachrichten: IR = 0 schliesst und oeffnet nichts Neues
        feed(&mut a, &dlp(false, false, "Nachrichten", ""), 2120);
        assert_eq!(a.state.music_candidates.len(), 1);
        assert!(a.music.open.is_none(), "kein Kandidat bei IR = 0");
        feed(&mut a, &ts(7_000), 2140);
        feed(&mut a, &dlp(false, false, "Wetter", ""), 2140);
        assert_eq!(a.state.music_candidates.len(), 1, "IR = 0 erzeugt keine Eintraege");
        cleanup(&a);
    }

    #[test]
    fn service_change_and_empty_buffer_drop_open_candidate() {
        let mut a = app();
        feed(&mut a, &ts(1_000), 2000);
        feed(&mut a, &dlp(false, true, "Titel", "Artist"), 2000);
        let ev = Event::ServiceStarted { slot: ServiceSlot::Primary, sid: 0xD220, scids: 0, codec: Codec::Mp2 { sample_rate: 48000 }, stereo: true };
        feed(&mut a, &ev, 2100);
        assert!(a.music.open.is_none());
        assert!(a.state.music_candidates.is_empty(), "verworfen, nicht eingetragen");

        // Puffer leert sich (EWS-Umschaltung): offener Kandidat ist weg
        a.state.current = Some(crate::state::CurrentService { sid: 0xD210, scids: 0, codec: None, stereo: true });
        feed(&mut a, &ts(2_000), 2200);
        feed(&mut a, &dlp(true, true, "Titel 2", "Artist 2"), 2200);
        assert!(a.music.open.is_some());
        let empty = Event::TimeshiftState { mode: TimeshiftMode::Live, buffered_s: 0.0, offset_s: 0.0, capacity_s: 3600.0, frame_index: 2_100, live_unix: 0 };
        feed(&mut a, &empty, 2210);
        assert!(a.music.open.is_none());
        assert!(a.state.music_candidates.is_empty());
        cleanup(&a);
    }

    #[test]
    fn dls_fallback_only_without_dlplus_and_after_30s() {
        let mut a = app();
        a.music.service_since = 1000;
        feed(&mut a, &ts(1_000), 1010);
        // Zu frueh: noch keine 30 s ohne DL+
        feed(&mut a, &dls("Gruppe X - Lied A"), 1010);
        assert!(a.music.open.is_none());
        // Nach 30 s greift der Ersatzweg
        feed(&mut a, &dls("Gruppe X - Lied A"), 1040);
        let open = a.music.open.clone().expect("Kandidat aus DLS");
        assert_eq!(open.artist.as_deref(), Some("Gruppe X"));
        assert_eq!(open.title.as_deref(), Some("Lied A"));
        assert!(open.from_dls);
        // Textwechsel schliesst und oeffnet
        feed(&mut a, &ts(9_000), 1240);
        feed(&mut a, &dls("Gruppe Y - Lied B"), 1240);
        assert_eq!(a.state.music_candidates.len(), 1);
        assert_eq!(a.state.music_candidates[0].title.as_deref(), Some("Lied A"));
        // Sobald DL+ kommt, ist der Ersatzweg aus
        feed(&mut a, &dlp(false, true, "Lied C", "Gruppe Z"), 1250);
        feed(&mut a, &dls("Irgendein Lauftext"), 1300);
        assert_eq!(a.music.open.as_ref().unwrap().title.as_deref(), Some("Lied C"));
        cleanup(&a);
    }

    #[test]
    fn list_is_capped_at_twenty() {
        let mut a = app();
        feed(&mut a, &ts(0), 2000);
        for i in 0..25u64 {
            feed(&mut a, &ts(i * 5_000), 2000 + i as i64 * 120);
            feed(&mut a, &dlp(i % 2 == 0, true, &format!("Titel {i}"), "A"), 2000 + i as i64 * 120);
        }
        assert_eq!(a.state.music_candidates.len(), MUSIC_MAX);
        assert_eq!(a.state.music_candidates[0].title.as_deref(), Some("Titel 4"), "aelteste fallen raus");
        cleanup(&a);
    }

    #[test]
    fn export_builds_mp3_command_with_tags() {
        let mut a = app();
        feed(&mut a, &ts(1_000), 2000);
        feed(&mut a, &dlp(false, true, "Enjoy the Silence", "Depeche Mode"), 2000);
        feed(&mut a, &ts(9_333), 2200);
        feed(&mut a, &dlp(true, true, "Blue Monday", "New Order"), 2200);
        feed(&mut a, &ts(10_000), 2220);
        let (path, fx) = a.music_export(0).unwrap();
        assert!(path.to_string_lossy().ends_with(".mp3"), "{}", path.display());
        assert!(path.to_string_lossy().contains("Depeche_Mode_-_Enjoy_the_Silence"), "{}", path.display());
        let cmd = fx.commands.first().expect("ein Kommando");
        match cmd {
            Command::ExportTimeshiftRange { from_s, to_s, format, .. } => {
                // (10000-1000)*0,024 + 8 = 224 ; (10000-9333)*0,024 - 3 = 13,008
                assert!((from_s - 224.0).abs() < 1e-6, "{from_s}");
                assert!((to_s - 13.008).abs() < 1e-6, "{to_s}");
                match format {
                    RecFormat::Mp3 { kbps, id3 } => {
                        assert_eq!(*kbps, a.settings.music_mp3_kbps);
                        let id3 = id3.as_ref().expect("ID3-Tags");
                        assert_eq!(id3.title.as_deref(), Some("Enjoy the Silence"));
                        assert_eq!(id3.artist.as_deref(), Some("Depeche Mode"));
                        assert_eq!(id3.album.as_deref(), Some("Dlf"));
                        assert_eq!(id3.date.as_deref(), Some(chrono::Local::now().format("%Y-%m-%d").to_string().as_str()));
                    }
                    other => panic!("MP3 erwartet, nicht {other:?}"),
                }
            }
            other => panic!("export_timeshift_range erwartet, nicht {other:?}"),
        }
        assert!(a.state.music_candidates[0].taken, "Eintrag ist erledigt");
        assert!(a.music_export(9).is_err(), "Index ausserhalb der Liste");
        cleanup(&a);
    }

    #[test]
    fn adjust_shifts_the_export_range_of_a_candidate() {
        let mut a = app();
        feed(&mut a, &ts(1_000), 2000);
        feed(&mut a, &dlp(false, true, "Enjoy the Silence", "Depeche Mode"), 2000);
        feed(&mut a, &ts(9_333), 2200);
        feed(&mut a, &dlp(true, true, "Blue Monday", "New Order"), 2200);
        feed(&mut a, &ts(10_000), 2220);
        let fx = a.music_adjust(0, 2.0, 6.0).unwrap();
        assert_eq!(a.state.music_candidates[0].pre_roll_s, Some(2.0));
        assert_eq!(a.state.music_candidates[0].post_roll_s, Some(6.0));
        assert!(matches!(fx.events.first(), Some(AppEvent::MusicCandidates { .. })), "Liste wird neu gemeldet");
        let (path, fx) = a.music_export(0).unwrap();
        let cmd = fx.commands.first().expect("ein Kommando");
        match cmd {
            // (10000-1000)*0,024 + 2 = 218 ; (10000-9333)*0,024 - 6 = 10,008
            Command::ExportTimeshiftRange { from_s, to_s, .. } => {
                assert!((from_s - 218.0).abs() < 1e-6, "{from_s}");
                assert!((to_s - 10.008).abs() < 1e-6, "{to_s}");
            }
            other => panic!("export_timeshift_range erwartet, nicht {other:?}"),
        }
        let _ = path;
        assert!(a.music_adjust(9, 1.0, 1.0).is_err(), "Index ausserhalb der Liste");
        // Geklemmt auf [MIN_ROLL_S, MAX_ROLL_S]
        a.state.music_candidates.push(TrackCandidate::opened(0, None, None, None));
        let last = a.state.music_candidates.len() - 1;
        a.music_adjust(last, -5.0, 999.0).unwrap();
        assert_eq!(a.state.music_candidates[last].pre_roll_s, Some(dab_music::MIN_ROLL_S));
        assert_eq!(a.state.music_candidates[last].post_roll_s, Some(dab_music::MAX_ROLL_S));
        cleanup(&a);
    }

    #[test]
    fn auto_save_exports_instead_of_listing() {
        let mut a = app();
        a.settings.music_auto_save = true;
        feed(&mut a, &ts(1_000), 2000);
        feed(&mut a, &dlp(false, true, "Titel", "Artist"), 2000);
        feed(&mut a, &ts(9_333), 2200);
        let fx = feed(&mut a, &dlp(true, true, "Titel 2", "Artist 2"), 2200);
        assert!(a.state.music_candidates.is_empty(), "automatisch gesichert statt gelistet");
        assert!(matches!(fx.commands.first(), Some(Command::ExportTimeshiftRange { .. })));
        // Zu kurz: kein automatischer Export, aber Eintrag in der Liste
        feed(&mut a, &ts(9_500), 2205);
        let fx = feed(&mut a, &dlp(false, true, "Titel 3", "Artist 3"), 2205);
        assert!(fx.commands.is_empty());
        assert_eq!(a.state.music_candidates.len(), 1);
        cleanup(&a);
    }

    #[test]
    fn handle_event_drives_the_detector() {
        // Der Weg ueber App::handle_event muss dieselbe Wirkung haben.
        let mut a = app();
        let now = Instant::now();
        // Ohne Schreibzeiger (Kern ohne Timeshift) entsteht kein Kandidat
        a.handle_event(&dlp(false, true, "Titel", "Artist"), now);
        assert!(a.music.open.is_none());
        a.handle_event(&ts(1_000), now);
        assert_eq!(a.music.frame, 1_000);
        a.handle_event(&dlp(false, true, "Titel", "Artist"), now);
        assert!(a.music.open.is_some());
        cleanup(&a);
    }
}
