//! DAB Classic – `dab-music`: Musikaufnahme mit Titel-Trennung (M4).
//!
//! Vorgesehen (Entscheidungen 6, 7):
//! - Titelerkennung aus `Event::DlPlus` (ETSI TS 102 980, Item-Toggle/Running).
//! - Schnittliste mit Vor-/Nachlauf, Schnitt aus dem Timeshift-Puffer des Kerns.
//! - Vorschlagsliste (Wellenform, verschiebbare Marken) oder automatisches Speichern.
//! - MP3 (LAME) mit ID3v2.4 inkl. Cover; optional Original-AAC (.m4a).
//!
//! Hier steht vorerst nur das Datenmodell der Schnittliste; Encoding und
//! Tagging folgen in M4.

use serde::{Deserialize, Serialize};

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

/// Ein erkannter Titel im Timeshift-Puffer (Zeiten relativ zum Pufferanfang).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct TrackCandidate {
    pub start_s: f64,
    pub end_s: Option<f64>,
    pub title: Option<String>,
    pub artist: Option<String>,
    pub album: Option<String>,
    pub genre: Option<String>,
    pub station: Option<String>,
    /// `false`, wenn IR=0 (Moderation/Nachrichten) – Kandidat wird ausgeschlossen.
    pub item_running: bool,
}

impl TrackCandidate {
    pub fn duration_s(&self) -> Option<f64> {
        self.end_s.map(|e| e - self.start_s)
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
