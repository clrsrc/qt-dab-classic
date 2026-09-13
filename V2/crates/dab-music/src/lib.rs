//! DAB Classic – `dab-music`: Musikaufnahme mit Titel-Trennung (M4b).
//!
//! Datenmodell der Schnittliste (Entscheidungen 6, 7, Plan M4b Abschnitt 2.1):
//! - Titelerkennung aus `Event::DlPlus` (ETSI TS 102 980, Item-Toggle/Running),
//!   Ersatzweg ueber DLS "Artist - Titel" (`dab-app::music`).
//! - Schnittliste mit Vor-/Nachlauf, Schnitt aus dem Timeshift-Puffer des Kerns.
//! - Vorschlagsliste oder automatisches Speichern (`Settings::music_auto_save`).
//! - MP3 (LAME) mit ID3v2.4 inkl. Cover schreibt der Kern
//!   (`Command::ExportTimeshiftRange` mit `RecFormat::Mp3`).
//!
//! **Massgeblich fuer den Export sind die Rahmenindizes** (`start_frame`,
//! `end_frame`): `from_s`/`to_s` des Kerns zaehlen "Sekunden hinter *jetzt*"
//! und aendern sich mit jeder Sekunde, waehrend der Kandidat im Ring wartet.
//! `start_s`/`end_s` dienen nur der Anzeige.

use serde::{Deserialize, Serialize};

/// Dauer eines DAB-Logikrahmens in Sekunden (24 ms); `frame_index` des Kerns
/// zaehlt in dieser Einheit (`Event::TimeshiftState`).
pub const FRAME_S: f64 = 0.024;

/// Grenzen fuer den individuellen Vor-/Nachlauf-Stepper je Kandidat (Plan
/// M4b Abschnitt 5: "+/- Sekunden-Stepper statt Drag-Wellenform").
pub const MIN_ROLL_S: f64 = 0.0;
pub const MAX_ROLL_S: f64 = 30.0;

/// DL+-Inhaltstypen, die einen Titel beschreiben (TS 102 980, Tabelle 3).
pub mod dlplus {
    pub const ITEM_TITLE: u8 = 1;
    pub const ITEM_ALBUM: u8 = 2;
    pub const ITEM_TRACKNUMBER: u8 = 3;
    pub const ITEM_ARTIST: u8 = 4;
    pub const ITEM_COMPOSITION: u8 = 5;
    pub const ITEM_MOVEMENT: u8 = 6;
    pub const ITEM_CONDUCTOR: u8 = 7;
    pub const ITEM_COMPOSER: u8 = 8;
    pub const ITEM_BAND: u8 = 9;
    pub const ITEM_COMMENT: u8 = 10;
    pub const ITEM_GENRE: u8 = 11;
    pub const STATIONNAME_SHORT: u8 = 31;
    pub const STATIONNAME_LONG: u8 = 32;
    pub const PROGRAMME_NOW: u8 = 33;
}

/// Ein erkannter Titel im Timeshift-Puffer.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct TrackCandidate {
    /// Rahmenindex des Kerns beim Beginn des Titels (massgeblich, s. Modulkopf).
    pub start_frame: u64,
    /// Rahmenindex beim Ende; `None` = Kandidat laeuft noch.
    pub end_frame: Option<u64>,
    /// Nur Anzeige: `start_frame` in Sekunden (Rahmenzeit, nicht Uhrzeit).
    pub start_s: f64,
    /// Nur Anzeige: `end_frame` in Sekunden.
    pub end_s: Option<f64>,
    pub title: Option<String>,
    pub artist: Option<String>,
    pub album: Option<String>,
    pub genre: Option<String>,
    pub station: Option<String>,
    /// `false`, wenn IR=0 (Moderation/Nachrichten) – Kandidat wird ausgeschlossen.
    pub item_running: bool,
    /// Aus DLS geraten (kein DL+ vorhanden): Titel/Interpret sind unsicher.
    pub from_dls: bool,
    /// Bereits exportiert ("uebernommen"); bleibt zur Rueckmeldung in der Liste.
    pub taken: bool,
    /// Vorlauf individuell verschoben (Stepper); `None` = `SplitConfig::pre_roll_s`.
    pub pre_roll_s: Option<f64>,
    /// Nachlauf individuell verschoben (Stepper); `None` = `SplitConfig::post_roll_s`.
    pub post_roll_s: Option<f64>,
}

impl Default for TrackCandidate {
    fn default() -> Self {
        Self {
            start_frame: 0,
            end_frame: None,
            start_s: 0.0,
            end_s: None,
            title: None,
            artist: None,
            album: None,
            genre: None,
            station: None,
            item_running: true,
            from_dls: false,
            taken: false,
            pre_roll_s: None,
            post_roll_s: None,
        }
    }
}

impl TrackCandidate {
    /// Neuer offener Kandidat ab `start_frame`.
    pub fn opened(start_frame: u64, title: Option<String>, artist: Option<String>, station: Option<String>) -> Self {
        Self {
            start_frame,
            start_s: start_frame as f64 * FRAME_S,
            title: title.filter(|s| !s.trim().is_empty()),
            artist: artist.filter(|s| !s.trim().is_empty()),
            station,
            ..Default::default()
        }
    }

    /// Kandidat bei `end_frame` schliessen (Anzeigewerte mitfuehren).
    pub fn close(&mut self, end_frame: u64) {
        let end = end_frame.max(self.start_frame);
        self.end_frame = Some(end);
        self.end_s = Some(end as f64 * FRAME_S);
    }

    pub fn is_open(&self) -> bool {
        self.end_frame.is_none()
    }

    /// Laenge in Sekunden; aus den Rahmenindizes, sonst aus den Anzeigewerten.
    pub fn duration_s(&self) -> Option<f64> {
        if let Some(end) = self.end_frame {
            return Some(end.saturating_sub(self.start_frame) as f64 * FRAME_S);
        }
        self.end_s.map(|e| e - self.start_s)
    }

    /// Laenge plausibel (Entscheidung 7: zu kurz = Jingle, zu lang = Sendung)?
    pub fn length_ok(&self, cfg: &SplitConfig) -> bool {
        match self.duration_s() {
            Some(d) => d >= cfg.min_len_s && d <= cfg.max_len_s,
            None => false,
        }
    }

    /// Anzeigename fuer Liste und Dateiname.
    pub fn label(&self) -> String {
        match (self.artist.as_deref(), self.title.as_deref()) {
            (Some(a), Some(t)) => format!("{a} - {t}"),
            (None, Some(t)) => t.to_string(),
            (Some(a), None) => a.to_string(),
            (None, None) => String::new(),
        }
    }

    /// Vorlauf, der fuer diesen Kandidaten gilt: individuell verschoben
    /// (Stepper) oder `cfg.pre_roll_s`.
    pub fn effective_pre_roll(&self, cfg: &SplitConfig) -> f64 {
        self.pre_roll_s.unwrap_or(cfg.pre_roll_s)
    }

    /// Nachlauf, der fuer diesen Kandidaten gilt (siehe `effective_pre_roll`).
    pub fn effective_post_roll(&self, cfg: &SplitConfig) -> f64 {
        self.post_roll_s.unwrap_or(cfg.post_roll_s)
    }

    /// Vor-/Nachlauf individuell setzen (Stepper in der Oberflaeche), geklemmt
    /// auf `[MIN_ROLL_S, MAX_ROLL_S]`.
    pub fn set_roll(&mut self, pre_roll_s: f64, post_roll_s: f64) {
        self.pre_roll_s = Some(pre_roll_s.clamp(MIN_ROLL_S, MAX_ROLL_S));
        self.post_roll_s = Some(post_roll_s.clamp(MIN_ROLL_S, MAX_ROLL_S));
    }

    /// Bereich fuer `Command::ExportTimeshiftRange`, bezogen auf den jetzigen
    /// Schreibzeiger `current_frame` (beide Werte = Sekunden hinter live,
    /// `from_s > to_s`). `None`, wenn der Kandidat noch offen ist oder der
    /// Bereich nach Vor-/Nachlauf keine Laenge mehr hat.
    pub fn to_export_range(&self, current_frame: u64, cfg: &SplitConfig) -> Option<(f64, f64)> {
        let end = self.end_frame?;
        let pre_roll_s = self.effective_pre_roll(cfg);
        let post_roll_s = self.effective_post_roll(cfg);
        let from_s = current_frame.saturating_sub(self.start_frame) as f64 * FRAME_S + pre_roll_s;
        let to_s = (current_frame.saturating_sub(end) as f64 * FRAME_S - post_roll_s).max(0.0);
        if !from_s.is_finite() || !to_s.is_finite() || from_s <= to_s {
            return None;
        }
        Some((from_s, to_s))
    }
}

/// Einstellungen der Trennung (Entscheidung 7).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct SplitConfig {
    pub pre_roll_s: f64,
    pub post_roll_s: f64,
    pub min_len_s: f64,
    pub max_len_s: f64,
}

impl Default for SplitConfig {
    fn default() -> Self {
        Self { pre_roll_s: 8.0, post_roll_s: 3.0, min_len_s: 90.0, max_len_s: 15.0 * 60.0 }
    }
}

/// DLS-Text nach v1-Konvention trennen: "Interpret - Titel". Ohne Trenner
/// gilt alles als Titel (Interpret unbekannt).
pub fn split_dls(text: &str) -> (Option<String>, Option<String>) {
    let t = text.trim();
    if t.is_empty() {
        return (None, None);
    }
    if let Some((left, right)) = t.split_once(" - ") {
        let (left, right) = (left.trim(), right.trim());
        if !left.is_empty() && !right.is_empty() {
            return (Some(left.to_string()), Some(right.to_string()));
        }
    }
    (None, Some(t.to_string()))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cand(start: u64, end: Option<u64>) -> TrackCandidate {
        let mut c = TrackCandidate::opened(start, Some("Titel".into()), Some("Artist".into()), Some("Dlf".into()));
        if let Some(e) = end {
            c.close(e);
        }
        c
    }

    #[test]
    fn export_range_from_frames() {
        let cfg = SplitConfig::default(); // pre 8 s, post 3 s
        // Titel von Rahmen 1000 bis 10000, jetzt bei 12500.
        // (12500-1000)*0.024 = 276 s + 8 = 284 ; (12500-10000)*0.024 = 60 - 3 = 57
        let c = cand(1000, Some(10_000));
        let (from_s, to_s) = c.to_export_range(12_500, &cfg).unwrap();
        assert!((from_s - 284.0).abs() < 1e-9, "{from_s}");
        assert!((to_s - 57.0).abs() < 1e-9, "{to_s}");
        assert!(from_s > to_s, "from_s zaehlt weiter zurueck als to_s");
        // Laenge des Ausschnitts = Titellaenge + Vorlauf + Nachlauf
        assert!((from_s - to_s - (c.duration_s().unwrap() + 8.0 + 3.0)).abs() < 1e-9);
    }

    #[test]
    fn export_range_moves_with_the_write_pointer() {
        let cfg = SplitConfig::default();
        let c = cand(1000, Some(10_000));
        let (f1, t1) = c.to_export_range(11_000, &cfg).unwrap();
        let (f2, t2) = c.to_export_range(11_500, &cfg).unwrap();
        // 500 Rahmen = 12 s weiter hinter live, Ausschnittlaenge bleibt gleich
        assert!((f2 - f1 - 12.0).abs() < 1e-9);
        assert!((t2 - t1 - 12.0).abs() < 1e-9);
        assert!((f1 - t1 - (f2 - t2)).abs() < 1e-9);
    }

    #[test]
    fn export_range_none_when_open_or_empty() {
        let cfg = SplitConfig::default();
        // noch offen
        assert_eq!(cand(1000, None).to_export_range(5000, &cfg), None);
        // Ende praktisch am Schreibzeiger: Nachlauf klemmt auf 0, Bereich bleibt gueltig
        let (f, t) = cand(1000, Some(9_900)).to_export_range(10_000, &cfg).unwrap();
        assert!(f > t && t == 0.0, "to_s wird auf 0 geklemmt: {f}/{t}");
        // Ein Ende vor dem Start kann nicht entstehen (wird auf start_frame geklemmt)
        let mut c = cand(10_000, None);
        c.close(9_000);
        assert_eq!(c.end_frame, Some(10_000));
        assert_eq!(c.duration_s(), Some(0.0));
        // Unsinniger Vorlauf (negativ in settings.json): Bereich ohne Laenge -> None
        let cfg2 = SplitConfig { pre_roll_s: -20.0, post_roll_s: 5.0, ..SplitConfig::default() };
        assert_eq!(c.to_export_range(10_000, &cfg2), None);
    }

    #[test]
    fn set_roll_overrides_split_config() {
        let cfg = SplitConfig::default(); // pre 8 s, post 3 s
        let mut c = cand(1000, Some(10_000));
        assert_eq!(c.effective_pre_roll(&cfg), 8.0);
        assert_eq!(c.effective_post_roll(&cfg), 3.0);
        c.set_roll(2.0, 6.0);
        assert_eq!(c.effective_pre_roll(&cfg), 2.0);
        assert_eq!(c.effective_post_roll(&cfg), 6.0);
        let (from_s, to_s) = c.to_export_range(12_500, &cfg).unwrap();
        // wie export_range_from_frames, aber pre 2 s statt 8 s, post 6 s statt 3 s
        assert!((from_s - 278.0).abs() < 1e-9, "{from_s}");
        assert!((to_s - 54.0).abs() < 1e-9, "{to_s}");
    }

    #[test]
    fn set_roll_clamps_to_bounds() {
        let mut c = cand(1000, Some(10_000));
        c.set_roll(-5.0, 999.0);
        assert_eq!(c.pre_roll_s, Some(MIN_ROLL_S));
        assert_eq!(c.post_roll_s, Some(MAX_ROLL_S));
    }

    #[test]
    fn duration_and_length_check() {
        let cfg = SplitConfig::default(); // min 90 s, max 900 s
        let c = cand(0, Some(8_000)); // 192 s
        assert!((c.duration_s().unwrap() - 192.0).abs() < 1e-9);
        assert!(c.length_ok(&cfg));
        assert!(!cand(0, Some(1_000)).length_ok(&cfg)); // 24 s: Jingle
        assert!(!cand(0, Some(60_000)).length_ok(&cfg)); // 1440 s: Sendung
        assert!(!cand(0, None).length_ok(&cfg));
    }

    #[test]
    fn dls_split_v1_convention() {
        assert_eq!(split_dls("Depeche Mode - Enjoy the Silence"), (Some("Depeche Mode".into()), Some("Enjoy the Silence".into())));
        assert_eq!(split_dls("Nachrichten"), (None, Some("Nachrichten".into())));
        assert_eq!(split_dls("  "), (None, None));
        // Kein Trenner mit Leerzeichen: bleibt ein Titel
        assert_eq!(split_dls("AC-DC"), (None, Some("AC-DC".into())));
    }

    #[test]
    fn label_and_defaults() {
        assert_eq!(cand(0, None).label(), "Artist - Titel");
        let c = TrackCandidate::opened(5, Some("Nur Titel".into()), None, None);
        assert_eq!(c.label(), "Nur Titel");
        assert_eq!(c.start_s, 5.0 * FRAME_S);
        assert!(c.is_open() && !c.taken && !c.from_dls);
        // Leerer DL+-Text zaehlt nicht als Angabe
        assert_eq!(TrackCandidate::opened(0, Some("  ".into()), None, None).title, None);
    }
}
