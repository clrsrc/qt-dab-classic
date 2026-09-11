//! DAB Classic – Protokoll zwischen Kernprozess (`dabcored`) und App.
//!
//! Der Kern spricht ausschliesslich ueber dieses Protokoll: Kommandos gehen
//! als JSON-Zeilen auf stdin hinein, Ereignisse kommen als JSON-Zeilen auf
//! stdout heraus. Beide Seiten (Rust hier, C++ in `core-cpp/libdabcore`)
//! implementieren dieselben Typen; `docs/protocol.md` ist die Referenz.
//!
//! Serialisierung: `serde_json`, extern getaggt mit `"type"`, Felder in
//! snake_case. Binaerdaten (MOT-Slides, Spektrum) als Base64-String.

pub mod protocol;

use serde::{Deserialize, Serialize};
use std::path::PathBuf;

/// Protokollversion; der Kern meldet seine Version im `Ready`-Ereignis.
pub const PROTOCOL_VERSION: u32 = 1;

// ---------------------------------------------------------------------------
// Hilfstypen
// ---------------------------------------------------------------------------

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum SourceKind {
    /// HackRF One (libhackrf zur Laufzeit geladen).
    HackRf { serial: Option<String> },
    /// RTL-SDR (librtlsdr zur Laufzeit geladen).
    RtlSdr { index: u32 },
    /// Datei-Wiedergabe: `.uff` (Qt-DAB XML-Format) oder rohe int8-IQ (`.iq`/`.raw`).
    /// `fast`: ohne Echtzeit-Pacing (Tests); Standard ist das `--fast` von dabcored.
    File {
        path: PathBuf,
        r#loop: bool,
        #[serde(default)]
        fast: bool,
    },
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ScanMode {
    /// Einmal ueber alle Kanaele, Ergebnis je Kanal, danach `ScanFinished`.
    Single,
    /// Bis zum ersten Ensemble mit Diensten, dann dort bleiben.
    ToData,
    /// Endlos, bis `StopScan`.
    Continuous,
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ServiceSlot {
    /// Der gehoerte Dienst (Audio-Ausgabe, Timeshift, PAD-Events).
    Primary,
    /// Hintergrunddienst (z. B. Aufnahme eines anderen Dienstes im selben Ensemble).
    Background,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(tag = "format", rename_all = "snake_case")]
pub enum RecFormat {
    Wav,
    Mp3 { kbps: u16 },
    /// Original-AAC-Zugriffseinheiten ohne Neucodierung (960-Sample-Frames).
    AacPassthrough,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(tag = "backing", rename_all = "snake_case")]
pub enum TimeshiftBacking {
    Ram,
    Disk { dir: PathBuf },
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum TimeshiftMode {
    Live,
    Paused,
    Playing,
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum EwsPhase {
    PreTrigger,
    Trigger,
    Sustain,
    End,
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum LogLevel {
    Error,
    Warn,
    Info,
    Debug,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(tag = "codec", rename_all = "snake_case")]
pub enum Codec {
    HeAac { sbr: bool, ps: bool, sample_rate: u32 },
    Mp2 { sample_rate: u32 },
}

/// Gain-Einstellung; Bedeutung je Geraet (HackRF: lna/vga/amp, RTL-SDR: nur `lna` als Tuner-Gain in 0,1 dB).
#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Gain {
    pub lna: u16,
    pub vga: u16,
    pub amp: bool,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct ServiceInfo {
    pub sid: u32,
    pub scids: u8,
    pub name: String,
    pub is_audio: bool,
    pub is_primary: bool,
    pub sub_ch: u8,
    pub bitrate_kbps: u16,
    pub pty: u8,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct TiiEntry {
    pub main_id: u8,
    pub sub_id: u8,
    pub strength: f32,
}

// ---------------------------------------------------------------------------
// Kommandos (App -> Kern)
// ---------------------------------------------------------------------------

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Command {
    // Quelle
    OpenDevice { source: SourceKind },
    CloseDevice,
    SetChannel { channel: String },
    SetGain { gain: Gain },
    SetAgc { enabled: bool },
    SetPpm { ppm: i32 },

    // Dienste
    SelectService { sid: u32, scids: u8, slot: ServiceSlot },
    StopService { slot: ServiceSlot },
    StartScan { channels: Vec<String>, mode: ScanMode },
    StopScan,

    // Audio
    SetVolume { percent: u8 },
    SetMute { muted: bool },
    SetAudioDevice { index: Option<u32> },

    // Aufnahme / Dumps
    StartRecording { path: PathBuf, format: RecFormat, slot: ServiceSlot },
    StopRecording { slot: ServiceSlot },
    ExportTimeshiftRange { from_s: f64, to_s: f64, path: PathBuf, format: RecFormat },
    StartIqDump { path: PathBuf },
    StopIqDump,
    StartFrameDump { path: PathBuf },
    StopFrameDump,

    // Timeshift
    TimeshiftConfigure { capacity_s: u32, backing: TimeshiftBacking },
    TimeshiftPause,
    TimeshiftPlay,
    TimeshiftSeek { offset_s: f64 },
    TimeshiftSkip { delta_s: f64 },
    TimeshiftLive,

    // EWS
    SetEws { enabled: bool, autoswitch: bool },
    EwsDismiss,

    // Diagnose
    SetScopes { spectrum: bool, iq: bool, rate_hz: u8 },
    SetTii { enabled: bool, threshold: i16, dx_mode: bool },
    GetState,
    Shutdown,
}

// ---------------------------------------------------------------------------
// Ereignisse (Kern -> App)
// ---------------------------------------------------------------------------

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Event {
    // Lebenszyklus / Quelle
    Ready { core_version: String, protocol_version: u32, decoders: Vec<String> },
    DeviceOpened { name: String, serial: String, bit_depth: u8 },
    DeviceClosed,
    DeviceError { message: String },
    FileProgress { position_s: f64, length_s: f64 },
    FileEnded,

    // Sync / FIC
    Synced { synced: bool },
    NoSignal { channel: String },
    Snr { db: f32 },
    FicQuality { ok: u16, total: u16 },
    FrequencyOffset { hz: i32 },
    EnsembleFound { eid: u16, name: String, channel: String },
    ServiceAdded { service: ServiceInfo },
    EnsembleReconfigured,
    ClockTime { unix_utc: i64, lto_minutes: i16 },

    // Dienst
    ServiceStarted { slot: ServiceSlot, sid: u32, scids: u8, codec: Codec, stereo: bool },
    ServiceStopped { slot: ServiceSlot },
    ServiceStats { slot: ServiceSlot, frame_errors: u16, rs_errors: u16, aac_errors: u16, rs_corrections: u16 },
    Dls { slot: ServiceSlot, text: String },
    /// Dynamic Label Plus (ETSI TS 102 980). `tags`: (content_type, text).
    DlPlus { slot: ServiceSlot, item_toggle: bool, item_running: bool, tags: Vec<(u8, String)> },
    /// Slideshow-Bild; `data_b64` = Rohbytes (JPEG/PNG) Base64.
    MotSlide { slot: ServiceSlot, mime: String, name: String, data_b64: String },
    /// Sonstiges MOT-Objekt (SPI-Logos, EPG-Rohdaten).
    MotObject { sid: u32, content_type: u16, name: String, data_b64: String },
    /// EPG (SPI) als XML-Text, wie vom epg-compiler erzeugt.
    EpgObject { sid: u32, date_yyyymmdd: u32, xml: String },
    Announcement { kind: u16, sub_ch: u8, active: bool },

    // Audio
    AudioFormat { rate: u32, channels: u8 },
    AudioLevel { left: f32, right: f32 },
    AudioUnderrun { missed: u32 },
    AudioDevices { names: Vec<String>, current: Option<u32> },

    // EWS / EWF
    EwsPresent,
    EwsAlert { phase: EwsPhase, sub_ch: u8, stage: u8, iid: u16, locations: Vec<String>, is_test: bool },
    /// Lebenszeichen der EWS-Signalisierung: `sub_ch` = Unterkanal des
    /// aktiven Alarms (hoechstens 1/s), `None` = Heartbeat ohne Alarm (1/s).
    EwsAlive { sub_ch: Option<u8> },
    EwfAlarm { active: bool, sub_ch: u8 },
    EwsSwitched { to_sid: u32, from_sid: Option<u32> },

    // Aufnahme / Timeshift
    RecordingState { slot: ServiceSlot, active: bool, path: Option<PathBuf>, bytes: u64, seconds: f64 },
    TimeshiftState { mode: TimeshiftMode, buffered_s: f64, offset_s: f64, capacity_s: f64 },

    // Scan
    ScanProgress { channel: String, index: u16, total: u16 },
    ScanResult { channel: String, eid: Option<u16>, ensemble: Option<String>, services: Vec<ServiceInfo>, snr: f32 },
    ScanFinished,

    // TII / Diagnose
    Tii { transmitters: Vec<TiiEntry> },
    /// dB-Werte 0..255 je Bin, Base64.
    Spectrum { bins_b64: String },
    IqSamples { iq_b64: String },
    Log { level: LogLevel, text: String },
    /// Antwort auf `GetState` oder nach Neustart des Kerns.
    StateSnapshot { state: CoreState },
    /// Der Kern beendet sich (nach `Shutdown` oder bei Fehler).
    Exiting { reason: String },
}

/// Vollstaendiger Zustand des Kerns, wie er ihn selbst kennt.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
pub struct CoreState {
    pub source: Option<SourceKind>,
    pub channel: Option<String>,
    pub gain: Gain,
    pub agc: bool,
    pub synced: bool,
    pub ensemble: Option<(u16, String)>,
    pub services: Vec<ServiceInfo>,
    pub primary: Option<(u32, u8)>,
    pub background: Option<(u32, u8)>,
    pub volume_percent: u8,
    pub muted: bool,
    pub timeshift: Option<(TimeshiftMode, f64, f64, f64)>,
    pub recording: bool,
    pub ews_enabled: bool,
    pub ews_autoswitch: bool,
}

impl Event {
    /// Ereignisse, bei denen nur der jeweils letzte Wert zaehlt. Die
    /// Rust-Seite darf sie bei Ueberlast verwerfen; alle anderen sind lueckenlos.
    pub fn is_latest_wins(&self) -> bool {
        matches!(
            self,
            Event::Snr { .. }
                | Event::FicQuality { .. }
                | Event::FrequencyOffset { .. }
                | Event::AudioLevel { .. }
                | Event::Spectrum { .. }
                | Event::IqSamples { .. }
                | Event::TimeshiftState { .. }
                | Event::FileProgress { .. }
                | Event::ServiceStats { .. }
        )
    }
}

/// DAB-Band-III-Kanaele mit Mittenfrequenz in kHz (EN 300 401 / TR 101 496).
pub const BAND_III: &[(&str, u32)] = &[
    ("5A", 174_928), ("5B", 176_640), ("5C", 178_352), ("5D", 180_064),
    ("6A", 181_936), ("6B", 183_648), ("6C", 185_360), ("6D", 187_072),
    ("7A", 188_928), ("7B", 190_640), ("7C", 192_352), ("7D", 194_064),
    ("8A", 195_936), ("8B", 197_648), ("8C", 199_360), ("8D", 201_072),
    ("9A", 202_928), ("9B", 204_640), ("9C", 206_352), ("9D", 208_064),
    ("10A", 209_936), ("10B", 211_648), ("10C", 213_360), ("10D", 215_072),
    ("11A", 216_928), ("11B", 218_640), ("11C", 220_352), ("11D", 222_064),
    ("12A", 223_936), ("12B", 225_648), ("12C", 227_360), ("12D", 229_072),
    ("13A", 230_784), ("13B", 232_496), ("13C", 234_208), ("13D", 235_776),
    ("13E", 237_488), ("13F", 239_200),
];

/// Mittenfrequenz eines Band-III-Kanals in kHz.
pub fn channel_frequency_khz(channel: &str) -> Option<u32> {
    BAND_III.iter().find(|(c, _)| c.eq_ignore_ascii_case(channel)).map(|(_, f)| *f)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn command_roundtrip() {
        let cmd = Command::OpenDevice {
            source: SourceKind::File { path: PathBuf::from("x.uff"), r#loop: true, fast: false },
        };
        let s = serde_json::to_string(&cmd).unwrap();
        assert!(s.contains("\"type\":\"open_device\""));
        assert!(s.contains("\"kind\":\"file\""));
        let back: Command = serde_json::from_str(&s).unwrap();
        assert_eq!(back, cmd);
    }

    #[test]
    fn event_roundtrip() {
        let ev = Event::EwsAlert {
            phase: EwsPhase::Trigger,
            sub_ch: 1,
            stage: 1,
            iid: 1,
            locations: vec!["Z1:5C+F300".into()],
            is_test: false,
        };
        let s = serde_json::to_string(&ev).unwrap();
        assert!(s.contains("\"type\":\"ews_alert\""));
        let back: Event = serde_json::from_str(&s).unwrap();
        assert_eq!(back, ev);
    }

    #[test]
    fn channel_table() {
        assert_eq!(channel_frequency_khz("5C"), Some(178_352));
        assert_eq!(channel_frequency_khz("11d"), Some(222_064));
        assert_eq!(channel_frequency_khz("99Z"), None);
    }
}
