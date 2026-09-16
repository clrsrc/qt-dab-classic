//! Zehn Stationsspeicher (Entscheidung 8).

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

pub const PRESET_SLOTS: usize = 10;

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Preset {
    /// Kanal, z. B. "5C".
    pub channel: String,
    pub eid: u16,
    pub sid: u32,
    #[serde(default)]
    pub scids: u8,
    /// Anzeige-Cache; der echte Name kommt beim Empfang aus dem FIC.
    pub name: String,
    #[serde(default)]
    pub logo_path: Option<PathBuf>,
    /// Kleines Logo (32x32) als `data:`-URL fuer die Speicherleiste (Entscheidung 8).
    #[serde(default)]
    pub logo_data_url: Option<String>,
    /// Unix-Zeit (UTC) der Belegung.
    #[serde(default)]
    pub stored_at: i64,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Presets {
    pub version: u32,
    /// Index 0..9 = Slot 1..10 (Taste 1..9, 0).
    pub slots: Vec<Option<Preset>>,
}

impl Default for Presets {
    fn default() -> Self {
        Self { version: 1, slots: vec![None; PRESET_SLOTS] }
    }
}

impl Presets {
    pub fn load(path: &Path) -> Self {
        match std::fs::read_to_string(path) {
            Ok(s) => match serde_json::from_str::<Presets>(&s) {
                Ok(mut p) => {
                    p.slots.resize(PRESET_SLOTS, None);
                    p
                }
                Err(e) => {
                    log::warn!("presets.json unlesbar ({e}), Standard verwendet");
                    Self::default()
                }
            },
            Err(_) => Self::default(),
        }
    }

    /// Atomar (tmp + rename, siehe `paths::write_atomic`).
    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        crate::paths::write_atomic(path, serde_json::to_string_pretty(self).expect("serialisierbar"))
    }

    pub fn get(&self, slot: usize) -> Option<&Preset> {
        self.slots.get(slot).and_then(|s| s.as_ref())
    }

    /// Belegt einen Slot; gibt den vorherigen Inhalt zurueck (fuer die Nachfrage
    /// "ueberschreiben?" fragt die GUI vorher mit [`is_occupied`](Self::is_occupied)).
    pub fn set(&mut self, slot: usize, preset: Preset) -> Option<Preset> {
        assert!(slot < PRESET_SLOTS, "Slot ausserhalb 0..{PRESET_SLOTS}");
        self.slots[slot].replace(preset)
    }

    pub fn clear(&mut self, slot: usize) -> Option<Preset> {
        self.slots.get_mut(slot).and_then(|s| s.take())
    }

    pub fn is_occupied(&self, slot: usize) -> bool {
        self.get(slot).is_some()
    }

    /// Erster freier Slot (fuer den Favoriten-Import).
    pub fn first_free(&self) -> Option<usize> {
        self.slots.iter().position(|s| s.is_none())
    }

    /// Taste '1'..'9','0' -> Slot 0..9.
    pub fn slot_for_key(key: char) -> Option<usize> {
        match key {
            '1'..='9' => Some(key as usize - '1' as usize),
            '0' => Some(9),
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn p(sid: u32) -> Preset {
        Preset { channel: "5C".into(), eid: 0x10BC, sid, scids: 0, name: "Dlf".into(), logo_path: None, logo_data_url: None, stored_at: 0 }
    }

    #[test]
    fn slots_and_keys() {
        let mut ps = Presets::default();
        assert_eq!(ps.slots.len(), PRESET_SLOTS);
        assert_eq!(Presets::slot_for_key('1'), Some(0));
        assert_eq!(Presets::slot_for_key('0'), Some(9));
        assert_eq!(Presets::slot_for_key('x'), None);
        assert!(ps.set(0, p(0xD210)).is_none());
        assert!(ps.is_occupied(0));
        assert_eq!(ps.first_free(), Some(1));
        assert_eq!(ps.set(0, p(0xD220)).unwrap().sid, 0xD210);
    }

    #[test]
    fn roundtrip_file() {
        let tmp = std::env::temp_dir().join(format!("dabclassic-presets-{}.json", std::process::id()));
        let mut ps = Presets::default();
        ps.set(3, p(0xD210));
        ps.save(&tmp).unwrap();
        let back = Presets::load(&tmp);
        assert_eq!(back, ps);
        std::fs::remove_file(&tmp).unwrap();
    }
}
