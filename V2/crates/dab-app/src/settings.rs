//! Einstellungen der App (`settings.json`). Kern-relevante Werte werden beim
//! Start als Kommandos an den Kern geschickt.

use dab_api::Gain;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::Path;

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct Settings {
    pub version: u32,
    /// "de" oder "en" (Entscheidung 27), None = Systemsprache.
    pub language: Option<String>,
    /// "hackrf", "rtlsdr" oder "file".
    pub device: String,
    pub last_channel: Option<String>,
    pub last_service: Option<(u32, u8)>,
    pub volume_percent: u8,
    /// SNR-gesteuerte Nachfuehrung (Entscheidung 26), Standard an.
    pub agc: bool,
    /// Manuelle Gain-Werte je Geraet und Kanal, wenn AGC aus.
    pub gain_by_channel: BTreeMap<String, BTreeMap<String, Gain>>,
    pub ppm: i32,
    pub timeshift_capacity_s: u32,
    pub ews_enabled: bool,
    pub ews_autoswitch: bool,
    pub record_pre_s: u32,
    pub record_post_s: u32,
    pub music_auto_save: bool,
    pub music_keep_aac: bool,
    pub music_mp3_kbps: u16,
    pub audio_device: Option<u32>,
    pub debug_panel_open: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            version: 1,
            language: None,
            device: "hackrf".into(),
            last_channel: None,
            last_service: None,
            volume_percent: 70,
            agc: true,
            gain_by_channel: BTreeMap::new(),
            ppm: 0,
            timeshift_capacity_s: 60 * 60,
            ews_enabled: true,
            ews_autoswitch: true,
            record_pre_s: 2 * 60,
            record_post_s: 5 * 60,
            music_auto_save: false,
            music_keep_aac: false,
            music_mp3_kbps: 256,
            audio_device: None,
            debug_panel_open: false,
        }
    }
}

impl Settings {
    pub fn load(path: &Path) -> Self {
        match std::fs::read_to_string(path) {
            Ok(s) => serde_json::from_str(&s).unwrap_or_else(|e| {
                log::warn!("settings.json unlesbar ({e}), Standard verwendet");
                Self::default()
            }),
            Err(_) => Self::default(),
        }
    }

    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        std::fs::write(path, serde_json::to_string_pretty(self).expect("serialisierbar"))
    }

    pub fn gain_for(&self, device: &str, channel: &str) -> Option<Gain> {
        self.gain_by_channel.get(device)?.get(channel).copied()
    }

    pub fn set_gain_for(&mut self, device: &str, channel: &str, gain: Gain) {
        self.gain_by_channel
            .entry(device.to_string())
            .or_default()
            .insert(channel.to_string(), gain);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_match_decisions() {
        let s = Settings::default();
        assert_eq!(s.timeshift_capacity_s, 3600);
        assert!(s.agc);
        assert_eq!(s.record_pre_s, 120);
        assert_eq!(s.record_post_s, 300);
        assert!(!s.music_auto_save);
    }

    #[test]
    fn partial_json_fills_defaults() {
        let s: Settings = serde_json::from_str(r#"{"volume_percent": 42}"#).unwrap();
        assert_eq!(s.volume_percent, 42);
        assert_eq!(s.music_mp3_kbps, 256);
    }
}
