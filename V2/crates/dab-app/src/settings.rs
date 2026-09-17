//! Einstellungen der App (`settings.json`). Kern-relevante Werte werden beim
//! Start als Kommandos an den Kern geschickt.

use dab_api::Gain;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

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
    /// Verkehrsfunk "TA" (crate::traffic): bei einer Durchsage fuer den
    /// laufenden Dienst auf den Durchsage-Dienst umschalten, danach zurueck.
    /// Standard aus - die Liste im Panel "Verkehr" fuellt sich unabhaengig davon.
    pub traffic_autoswitch: bool,
    /// Durchsagen und Notfallwarnungen im Background-Slot als MP3 mitschneiden
    /// (crate::traffic, Unterordner "durchsagen" des Aufnahmeordners), Standard an.
    pub announcement_record: bool,
    /// Obergrenze fuer den Unterordner "durchsagen" (crate::storage, Punkt N2,
    /// 17.09.2026): hoechstens so viele Dateien bzw. so viele MB, aelteste
    /// zuerst weg; 0 = unbegrenzt. Standard 50 Dateien / 200 MB.
    pub announcement_keep_files: u32,
    pub announcement_keep_mb: u32,
    pub record_pre_s: u32,
    pub record_post_s: u32,
    /// Musik-Trennung (Titelerkennung aus DL+/DLS, Schnitt aus dem Timeshift-Ring)
    /// insgesamt an/aus, Standard an. Aus loescht offene Kandidaten und die Liste.
    pub music_enabled: bool,
    pub music_auto_save: bool,
    pub music_keep_aac: bool,
    pub music_mp3_kbps: u16,
    /// Audio-Ausgabegeraet: `id` aus `audio_devices` (Windows: WASAPI-
    /// Endpoint-ID, stabil ueber Sitzungen und Umstecken), None = Standard-
    /// geraet des Systems. Bis 17.09.2026 stand hier ein PortAudio-Index.
    #[serde(deserialize_with = "de_audio_device")]
    pub audio_device: Option<String>,
    /// Anzeigename des gewaehlten Geraets, damit die UI es auch benennen
    /// kann, wenn es gerade nicht angeschlossen ist.
    pub audio_device_name: Option<String>,
    /// EPG/SPI-Paketdienst im Kern automatisch mitlaufen lassen (`set_epg`), Standard an.
    pub epg_enabled: bool,
    /// TPEG-Paketdienst im Kern automatisch mitlaufen lassen (`set_tpeg`) und
    /// TEC-Verkehrsmeldungen dekodieren (crate::tpeg), Standard an (Punkt 3,
    /// 17.09.2026). Nur Broadcast, kein Internet.
    pub tpeg_enabled: bool,
    /// Hybrid Radio (crate::radiodns): Logos und Sendeplaene fuer Dienste ohne
    /// Broadcast-EPG per RadioDNS/SPI ueber IP nachladen. Standard AUS - die
    /// App spricht dann nie mit dem Internet (17.09.2026).
    pub radiodns_enabled: bool,
    /// Speichertasten zeigen das Kurzlabel des Senders (FIG 1 Zeichen-Flags,
    /// max. 8 Zeichen) statt des vollen Namens, Standard an (17.09.2026).
    pub preset_short_labels: bool,
    /// Beim Start das zuletzt benutzte Geraet oeffnen und den letzten Dienst wiederherstellen.
    pub autostart: bool,
    /// Letzte Datei fuer die Datei-Wiedergabe (Entscheidung 13).
    pub last_file: Option<PathBuf>,
    pub file_loop: bool,
    pub rtlsdr_index: u32,
    /// Sichtbarkeit der ein-/ausklappbaren Panels (Entscheidung 9).
    pub panels: Panels,
    /// Zielordner fuer Aufnahmen; None = `data/recordings` (crate::recording).
    pub recording_dir: Option<PathBuf>,
    /// Warnton im Alarmfenster (crate::timer / Alarm, Entscheidung 5).
    pub alarm_beep: bool,
    /// TII / Debug-Panel (crate::tii, Entscheidung 25): Heimatkoordinaten fuer
    /// Entfernung/Azimut, TII-Detektor, DX-Protokoll `tii-files.csv`, Scope-Rate 1..10.
    pub home_lat: Option<f64>,
    pub home_lon: Option<f64>,
    pub tii_enabled: bool,
    pub tii_threshold: i16,
    pub tii_dx_mode: bool,
    pub scope_rate_hz: u8,
    /// Fenstergroesse beim letzten Beenden (physische Pixel); None = Standard
    /// aus tauri.conf.json (Bugfixes.txt #4).
    pub window_width: Option<u32>,
    pub window_height: Option<u32>,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct Panels {
    pub presets: bool,
    /// Tab "Ensemble": Dienste des abgestimmten Ensembles.
    pub services: bool,
    /// Tab "Senderliste": alle Dienste aller bekannten Ensembles (crate::stations).
    pub stations: bool,
    pub settings: bool,
    pub scan: bool,
    pub epg: bool,
    pub timer: bool,
    /// Debug-Panel (crate::tii): offen = Kern liefert Spektrum/IQ.
    pub debug: bool,
    /// Panel "Musik" (crate::music): Vorschlagsliste der Titel-Trennung.
    pub music: bool,
    /// EWF-Historie (Bugfixes.txt #10), erreichbar ueber den Button "EWF".
    pub ews_history: bool,
    /// Panel "Verkehr" (crate::traffic), Button "TA" neben EPG.
    pub traffic: bool,
}

impl Default for Panels {
    fn default() -> Self {
        Self { presets: true, services: true, stations: true, settings: false, scan: false, epg: false, timer: false, debug: false, music: false, ews_history: false, traffic: false }
    }
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
            traffic_autoswitch: false,
            announcement_record: true,
            announcement_keep_files: 50,
            announcement_keep_mb: 200,
            record_pre_s: 2 * 60,
            record_post_s: 5 * 60,
            music_enabled: false,
            music_auto_save: false,
            music_keep_aac: false,
            music_mp3_kbps: 256,
            audio_device: None,
            audio_device_name: None,
            epg_enabled: true,
            tpeg_enabled: true,
            radiodns_enabled: false,
            preset_short_labels: true,
            autostart: true,
            last_file: None,
            file_loop: true,
            rtlsdr_index: 0,
            panels: Panels::default(),
            recording_dir: None,
            alarm_beep: true,
            home_lat: None,
            home_lon: None,
            tii_enabled: true,
            tii_threshold: crate::tii::TII_THRESHOLD_DEFAULT,
            tii_dx_mode: false,
            scope_rate_hz: crate::tii::SCOPE_RATE_DEFAULT,
            window_width: None,
            window_height: None,
        }
    }
}

/// `audio_device` war bis 17.09.2026 ein PortAudio-Index (Zahl). Eine alte
/// Zahl wird zu None (Standardgeraet) statt die ganze settings.json zu
/// verwerfen (`Settings::load` faellt bei einem Lesefehler komplett auf
/// Standard zurueck).
fn de_audio_device<'de, D: serde::Deserializer<'de>>(d: D) -> Result<Option<String>, D::Error> {
    #[derive(Deserialize)]
    #[serde(untagged)]
    enum Raw {
        Id(String),
        Other(serde::de::IgnoredAny),
    }
    Ok(match Option::<Raw>::deserialize(d)? {
        Some(Raw::Id(id)) if !id.is_empty() => Some(id),
        _ => None,
    })
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

    /// Atomar (tmp + rename, siehe `paths::write_atomic`): settings.json wird
    /// bei jedem Kanalwechsel geschrieben, ein Absturz mittendrin darf sie
    /// nicht halb hinterlassen.
    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        crate::paths::write_atomic(path, serde_json::to_string_pretty(self).expect("serialisierbar"))
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
        assert!(!s.music_enabled);
        assert!(!s.music_auto_save);
        assert!(!s.radiodns_enabled, "kein Internet ohne Zustimmung");
    }

    #[test]
    fn partial_json_fills_defaults() {
        let s: Settings = serde_json::from_str(r#"{"volume_percent": 42}"#).unwrap();
        assert_eq!(s.volume_percent, 42);
        assert_eq!(s.music_mp3_kbps, 256);
        assert!(s.epg_enabled);
        assert!(s.tpeg_enabled);
    }

    #[test]
    fn audio_device_old_index_becomes_default() {
        // settings.json von vor N1 (PortAudio-Index): Standardgeraet, Rest bleibt
        let s: Settings = serde_json::from_str(r#"{"audio_device": 3, "volume_percent": 7}"#).unwrap();
        assert_eq!(s.audio_device, None);
        assert_eq!(s.volume_percent, 7);
        let s: Settings = serde_json::from_str(r#"{"audio_device": null}"#).unwrap();
        assert_eq!(s.audio_device, None);
        let s: Settings = serde_json::from_str(r#"{"audio_device": "{0.0.0.00000000}.{abc}", "audio_device_name": "USB DAC"}"#).unwrap();
        assert_eq!(s.audio_device.as_deref(), Some("{0.0.0.00000000}.{abc}"));
        assert_eq!(s.audio_device_name.as_deref(), Some("USB DAC"));
        // Hin und zurueck
        let back: Settings = serde_json::from_str(&serde_json::to_string(&s).unwrap()).unwrap();
        assert_eq!(back, s);
    }

    #[test]
    fn unknown_fields_are_ignored() {
        // aeltere settings.json (z. B. mit `debug_panel_open`) bleibt lesbar
        let s: Settings = serde_json::from_str(r#"{"debug_panel_open": true, "epg_enabled": false}"#).unwrap();
        assert!(!s.epg_enabled);
    }
}
