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

/// ID3v2.4-Tags fuer eine MP3-Aufnahme (Plan M4b 1.1). Der Kern schreibt nur
/// die gesetzten Felder; das Cover liefert die App fertig als PNG (Slide oder
/// Logo), der Kern kennt selbst keine Bilder.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
pub struct Id3Tags {
    pub title: Option<String>,
    pub artist: Option<String>,
    /// z. B. Sendername ("Dlf")
    pub album: Option<String>,
    /// ISO yyyy-mm-dd
    pub date: Option<String>,
    pub cover_png_b64: Option<String>,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(tag = "format", rename_all = "snake_case")]
pub enum RecFormat {
    Wav,
    /// `id3` kam additiv in M4b dazu; Aufrufe ohne das Feld bleiben gueltig.
    Mp3 {
        kbps: u16,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        id3: Option<Id3Tags>,
    },
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
    /// Paketdienst (MOT/EPG), kein Audio.
    Data,
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

fn is_zero_f64(v: &f64) -> bool { *v == 0.0 }

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

    // Dienste. Im Background-Slot koennen mehrere Dienste gleichzeitig laufen
    // (Entscheidung 24); `sid` waehlt einen davon, `None` = alle im Slot.
    SelectService { sid: u32, scids: u8, slot: ServiceSlot },
    StopService { slot: ServiceSlot, #[serde(default, skip_serializing_if = "Option::is_none")] sid: Option<u32> },
    StartScan { channels: Vec<String>, mode: ScanMode },
    StopScan,

    // Audio
    SetVolume { percent: u8 },
    SetMute { muted: bool },
    SetAudioDevice { index: Option<u32> },

    // Aufnahme / Dumps
    StartRecording {
        path: PathBuf,
        format: RecFormat,
        slot: ServiceSlot,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        sid: Option<u32>,
        /// Vorlauf aus dem Timeshift-Ring in dieselbe Aufnahme uebernehmen
        /// (Sekunden, Entscheidung 18); 0 = kein Vorlauf. Additiv, der Kern
        /// schreibt den Vorlauf als eigene Datei `<name>_vorlauf.wav`
        /// (M4-Timeshift-Plan 1.6) und meldet das nur per `log info`.
        #[serde(default, skip_serializing_if = "is_zero_f64")]
        pre_s: f64,
    },
    StopRecording { slot: ServiceSlot, #[serde(default, skip_serializing_if = "Option::is_none")] sid: Option<u32> },
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
    /// Heimatkoordinaten fuer das Geofencing (ETSI TS 104 089 Annex F):
    /// der Kern vergleicht die Location-Codes eines Alarms damit und setzt
    /// `Event::EwsAlert.relevant` entsprechend; `None`/fehlend = kein
    /// Standort bekannt, jeder Alarm gilt als relevant (Ausgangsverhalten).
    SetHomeLocation { lat: Option<f64>, lon: Option<f64> },

    // SPI/EPG: den Paketdienst des Ensembles (FIG 0/13 Appl-Type 7) automatisch
    // als Background-Slot laufen lassen (Logos, EPG). Standard an; `false`
    // beendet den vom Kern gestarteten Dienst.
    SetEpg { enabled: bool },

    // Diagnose. Scopes: `spectrum` und `iq` getrennt schaltbar, gemeinsame
    // Rate 1..10 Hz (Standard 5; der Kern klemmt); beide aus = kein Aufwand.
    SetScopes { spectrum: bool, iq: bool, rate_hz: u8 },
    // TII: Standard an, Schwelle 6; `dx_mode` wird gemerkt, hat im Kern
    // heute keine Wirkung. `Tii` kommt hoechstens 1x/s und nur bei Aenderung.
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
    /// Aktueller Gain-Satz und AGC-Zustand: nach `SetGain`/`SetAgc`, beim
    /// Oeffnen eines Geraets, bei jeder AGC-Nachfuehrung (VGA +-2 bzw.
    /// Tuner-Gain-Stufe) und beim AMP-Retry im Scan. Die App speichert ihn
    /// je Geraet und Kanal (Entscheidung 26).
    GainChanged { lna: u16, vga: u16, amp: bool, agc: bool },
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
    // Dienst-Ereignisse tragen neben dem Slot immer den SId, weil im
    // Background-Slot mehrere Dienste laufen koennen.
    ServiceStarted { slot: ServiceSlot, sid: u32, scids: u8, codec: Codec, stereo: bool },
    ServiceStopped { slot: ServiceSlot, sid: u32 },
    /// Fehlerzaehler der letzten Sekunde (Superframes ohne Firecode/RS-Erfolg,
    /// nicht korrigierbare RS-Zeilen, AAC-CRC-/Decoderfehler, korrigierte RS-Symbole).
    ServiceStats { slot: ServiceSlot, sid: u32, frame_errors: u16, rs_errors: u16, aac_errors: u16, rs_corrections: u16 },
    Dls { slot: ServiceSlot, sid: u32, text: String },
    /// Dynamic Label Plus (ETSI TS 102 980), je DL+-Kommando: `tags`: (content_type, text),
    /// alle Content-Types 0..63; Text = Ausschnitt des zuletzt vollstaendigen DLS-Labels.
    DlPlus { slot: ServiceSlot, sid: u32, item_toggle: bool, item_running: bool, tags: Vec<(u8, String)> },
    /// Slideshow-Bild (X-PAD); `data_b64` = Rohbytes (JPEG/PNG) Base64.
    MotSlide { slot: ServiceSlot, sid: u32, mime: String, name: String, data_b64: String },
    /// MOT-Objekt aus dem SPI-Paketdienst (Logos als PNG/JPEG, Text-Objekte).
    /// `sid` = Dienst, dem das Objekt gilt (aus dem Namen "d210_Dlf_32x32.png"
    /// gegen die Dienstliste des Ensembles), sonst 0; `eid` = Ensemble;
    /// `name` = MOT-Dateiname (Cache: `data/logos/<eid>/<name>`).
    MotObject { eid: u16, sid: u32, content_type: u16, name: String, data_b64: String },
    /// EPG (SPI) als XML-Text, wie vom epg-compiler erzeugt (Format wie v1
    /// `<EId>/<yyyyMMdd>_<SId>_SI.xml`). Sendeplan: `sid`/`date_yyyymmdd`
    /// aus dem MOT-Namen ("w20260914dd230c0.EHB"), Wurzel `<epg>`.
    /// Service-Information (Logo-Zuordnung, v1 `list.xml`): `sid` = 0,
    /// `date_yyyymmdd` = 0, Wurzel `<serviceInformation>`.
    EpgObject { eid: u16, sid: u32, date_yyyymmdd: u32, name: String, xml: String },
    /// Ankuendigung (FIG 0/18/0/19). `sid`/`cluster` ergaenzt der Kern seit
    /// 2026-09-16 (Dienst, fuer den die Ankuendigung gilt, und Cluster-Id
    /// der FIG 0/19); aeltere Kerne liefern sie nicht -> Standard 0.
    Announcement {
        kind: u16,
        sub_ch: u8,
        active: bool,
        #[serde(default)]
        sid: u32,
        #[serde(default)]
        cluster: u8,
    },

    // Audio
    AudioFormat { rate: u32, channels: u8 },
    AudioLevel { left: f32, right: f32 },
    AudioUnderrun { missed: u32 },
    AudioDevices { names: Vec<String>, current: Option<u32> },

    // EWS / EWF
    EwsPresent,
    /// `stage_raw`: rohes Status-Byte der FIG 0/15 (Bit 7 Last, Bits 6..4
    /// Stage, Bits 3..0 IId) der ersten Trigger-Instanz, Warntag 2026: 0x01;
    /// additiv, Standard 0.
    EwsAlert {
        phase: EwsPhase,
        sub_ch: u8,
        stage: u8,
        #[serde(default)]
        stage_raw: u8,
        iid: u16,
        locations: Vec<String>,
        is_test: bool,
        /// Geofencing-Ergebnis des Kerns (Annex F Location-Codes gegen die
        /// per `SetHomeLocation` gesetzten Heimatkoordinaten): `None` = kein
        /// Standort bekannt (immer relevant), `Some(true)` = mindestens ein
        /// Code deckt den Standort ab, `Some(false)` = keiner (z. B. der
        /// "Eiffelturm"-Funktionstest, gesehen von einem deutschen Standort).
        /// Additiv, aeltere Kerne senden das Feld nicht -> `None`.
        #[serde(default)]
        relevant: Option<bool>,
    },
    /// Lebenszeichen der EWS-Signalisierung: `sub_ch` = Unterkanal des
    /// aktiven Alarms (hoechstens 1/s), `None` = Heartbeat ohne Alarm (1/s).
    EwsAlive { sub_ch: Option<u8> },
    EwfAlarm { active: bool, sub_ch: u8 },
    EwsSwitched { to_sid: u32, from_sid: Option<u32> },

    // Aufnahme / Timeshift
    RecordingState { slot: ServiceSlot, sid: u32, active: bool, path: Option<PathBuf>, bytes: u64, seconds: f64 },
    /// `frame_index` (Schreibzeiger) und `live_unix` (Ensemble-Uhrzeit am
    /// Schreibzeiger, 0 = unbekannt) kamen additiv in M4 dazu; aeltere Kerne
    /// senden sie nicht (Plan M4 1.4).
    TimeshiftState {
        mode: TimeshiftMode,
        buffered_s: f64,
        offset_s: f64,
        capacity_s: f64,
        #[serde(default)]
        frame_index: u64,
        #[serde(default)]
        live_unix: i64,
    },

    // Scan
    ScanProgress { channel: String, index: u16, total: u16 },
    ScanResult { channel: String, eid: Option<u16>, ensemble: Option<String>, services: Vec<ServiceInfo>, snr: f32 },
    ScanFinished,

    // TII / Diagnose
    Tii { transmitters: Vec<TiiEntry> },
    /// Spektrum der Eingangssamples: 2048 Bins als u8, Base64. fftshift
    /// (Bin 0 = -1,024 MHz, Bin 1024 = Traegermitte), 0,5 dB je Stufe:
    /// dBFS = Wert / 2 - 120 (0 = -120 dBFS, 240 = 0 dBFS). Nur mit
    /// `SetScopes { spectrum: true }`, hoechstens `rate_hz`-mal je Sekunde.
    Spectrum { bins_b64: String },
    /// Konstellation eines OFDM-Symbols (Symbol 2, wie das v1-IQ-Scope):
    /// 1536 Traeger nach der Differenzdemodulation, in Frequenzreihenfolge
    /// (k = -768..-1, 1..768), auf den Einheitskreis normiert, als 3072
    /// int8-Werte I0,Q0,I1,Q1,... (127 = 1,0), Base64 (4096 Zeichen). Nur
    /// mit `SetScopes { iq: true }`, hoechstens `rate_hz`-mal je Sekunde.
    IqSamples { iq_b64: String },
    Log { level: LogLevel, text: String },
    /// Antwort auf `GetState` oder nach Neustart des Kerns.
    StateSnapshot { state: CoreState },
    /// Der Kern beendet sich (nach `Shutdown` oder bei Fehler).
    Exiting { reason: String },
}

/// Scope-Einstellungen (`SetScopes`), wie der Kern sie gerade haelt.
#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq)]
pub struct ScopeSettings {
    pub spectrum: bool,
    pub iq: bool,
    /// 1..10, Standard 5
    pub rate_hz: u8,
}

impl Default for ScopeSettings {
    fn default() -> Self { ScopeSettings { spectrum: false, iq: false, rate_hz: 5 } }
}

/// Zuletzt empfangene Ensemble-Uhrzeit (FIG 0/10 + 0/9), wie `ClockTime`.
#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct ClockTimeState {
    pub unix_utc: i64,
    pub lto_minutes: i16,
}

/// Aufnahmestand eines laufenden Dienstes (Felder wie `RecordingState`).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
pub struct RecordingInfo {
    pub active: bool,
    pub path: Option<PathBuf>,
    pub bytes: u64,
    pub seconds: f64,
}

/// Letztes DL+-Kommando eines Dienstes (Felder wie `DlPlus`).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
pub struct DlPlusState {
    pub item_toggle: bool,
    pub item_running: bool,
    pub tags: Vec<(u8, String)>,
}

/// Letztes Slideshow-Bild eines Dienstes (Felder wie `MotSlide`).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
pub struct SlideState {
    pub mime: String,
    pub name: String,
    pub data_b64: String,
}

/// Ein laufender Dienst (alle Slots) mit dem letzten Anzeigezustand, damit
/// eine neu verbundene App DLS, DL+, Slide, Codec und Aufnahme sofort hat.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct RunningServiceState {
    pub slot: ServiceSlot,
    pub sid: u32,
    pub scids: u8,
    pub name: String,
    pub is_audio: bool,
    /// `None`, solange bei Audio noch kein Block dekodiert wurde.
    pub codec: Option<Codec>,
    pub stereo: bool,
    pub dls: Option<String>,
    pub dl_plus: Option<DlPlusState>,
    pub slide: Option<SlideState>,
    pub recording: RecordingInfo,
}

fn default_true() -> bool { true }
fn default_tii_threshold() -> i16 { 6 }

/// Vollstaendiger Zustand des Kerns, wie er ihn selbst kennt. Die mit
/// `#[serde(default)]` markierten Felder kamen additiv hinzu (Protokoll 1,
/// M3); aeltere Kerne senden sie nicht.
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
    // --- additiv (M3) ---
    /// SPI/EPG-Hintergrunddienst automatisch starten (`SetEpg`).
    #[serde(default = "default_true")]
    pub epg_enabled: bool,
    #[serde(default = "default_true")]
    pub tii_enabled: bool,
    #[serde(default = "default_tii_threshold")]
    pub tii_threshold: i16,
    /// Gemerkt, im Kern heute ohne Wirkung.
    #[serde(default)]
    pub tii_dx_mode: bool,
    #[serde(default)]
    pub scopes: ScopeSettings,
    /// Letzter SNR-Wert in dB (0, solange keiner vorliegt).
    #[serde(default)]
    pub snr: f32,
    #[serde(default)]
    pub clock_time: Option<ClockTimeState>,
    #[serde(default)]
    pub ppm: i32,
    #[serde(default)]
    pub scanning: bool,
    /// Alle laufenden Dienste (Primary und alle Background-Dienste).
    #[serde(default)]
    pub running: Vec<RunningServiceState>,
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
            stage_raw: 0x81,
            iid: 1,
            locations: vec!["Z1:5C+F300".into()],
            is_test: false,
            relevant: Some(false),
        };
        let s = serde_json::to_string(&ev).unwrap();
        assert!(s.contains("\"type\":\"ews_alert\""));
        assert!(s.contains("\"stage_raw\":129"));
        assert!(s.contains("\"relevant\":false"));
        let back: Event = serde_json::from_str(&s).unwrap();
        assert_eq!(back, ev);
        // Aeltere Kerne ohne stage_raw/relevant: Felder fehlen -> Default
        let old = r#"{"type":"ews_alert","phase":"trigger","sub_ch":1,"stage":1,"iid":1,"locations":[],"is_test":false}"#;
        match serde_json::from_str::<Event>(old).unwrap() {
            Event::EwsAlert { stage_raw, relevant, .. } => {
                assert_eq!(stage_raw, 0);
                assert_eq!(relevant, None);
            }
            _ => panic!(),
        }
    }

    #[test]
    fn set_home_location_roundtrip() {
        let cmd = Command::SetHomeLocation { lat: Some(51.218), lon: Some(6.7617) };
        let s = serde_json::to_string(&cmd).unwrap();
        assert!(s.contains("\"type\":\"set_home_location\""));
        let back: Command = serde_json::from_str(&s).unwrap();
        assert_eq!(back, cmd);

        let cleared = Command::SetHomeLocation { lat: None, lon: None };
        let s2 = serde_json::to_string(&cleared).unwrap();
        let back2: Command = serde_json::from_str(&s2).unwrap();
        assert_eq!(back2, cleared);
    }

    #[test]
    fn service_events_carry_sid() {
        let ev = Event::DlPlus { slot: ServiceSlot::Background, sid: 0xD210, item_toggle: true, item_running: false, tags: vec![(1, "Titel".into()), (4, "Artist".into())] };
        let s = serde_json::to_string(&ev).unwrap();
        assert!(s.contains("\"sid\":53776"));
        assert!(s.contains("\"tags\":[[1,\"Titel\"],[4,\"Artist\"]]"));
        let back: Event = serde_json::from_str(&s).unwrap();
        assert_eq!(back, ev);
        // C++-Seite: service_started eines Paketdienstes
        let js = r#"{"type":"service_started","slot":"background","sid":4292,"scids":0,"stereo":false,"codec":{"codec":"data"}}"#;
        let ev: Event = serde_json::from_str(js).unwrap();
        assert_eq!(ev, Event::ServiceStarted { slot: ServiceSlot::Background, sid: 4292, scids: 0, codec: Codec::Data, stereo: false });
        // stop_service ohne sid bleibt kompakt
        let c = Command::StopService { slot: ServiceSlot::Background, sid: None };
        assert_eq!(serde_json::to_string(&c).unwrap(), r#"{"type":"stop_service","slot":"background"}"#);
    }

    #[test]
    fn epg_events() {
        // C++-Seite: events::epgObject / events::motObject
        let js = r#"{"type":"epg_object","eid":4284,"sid":53776,"date_yyyymmdd":20260914,"name":"w20260914dd210c0.EHB","xml":"<epg system=\"DAB\">\n</epg>\n"}"#;
        let ev: Event = serde_json::from_str(js).unwrap();
        assert_eq!(ev, Event::EpgObject { eid: 0x10BC, sid: 0xD210, date_yyyymmdd: 20260914, name: "w20260914dd210c0.EHB".into(), xml: "<epg system=\"DAB\">\n</epg>\n".into() });
        let js = r#"{"type":"mot_object","eid":4284,"sid":53776,"content_type":515,"name":"d210_Dlf_32x32.png","data_b64":"iVBORw0KGgo="}"#;
        let ev: Event = serde_json::from_str(js).unwrap();
        assert_eq!(ev, Event::MotObject { eid: 0x10BC, sid: 0xD210, content_type: 0x0203, name: "d210_Dlf_32x32.png".into(), data_b64: "iVBORw0KGgo=".into() });
        assert_eq!(serde_json::to_string(&Command::SetEpg { enabled: false }).unwrap(), r#"{"type":"set_epg","enabled":false}"#);
    }

    #[test]
    fn gain_changed_is_flat() {
        // C++-Seite: events::gainChanged (flache Felder wie protocol.md)
        let js = r#"{"type":"gain_changed","lna":40,"vga":26,"amp":false,"agc":true}"#;
        let ev: Event = serde_json::from_str(js).unwrap();
        assert_eq!(ev, Event::GainChanged { lna: 40, vga: 26, amp: false, agc: true });
        assert!(!ev.is_latest_wins());
        let s = serde_json::to_string(&Command::SetGain { gain: Gain { lna: 40, vga: 24, amp: false } }).unwrap();
        assert_eq!(s, r#"{"type":"set_gain","gain":{"lna":40,"vga":24,"amp":false}}"#);
    }

    #[test]
    fn core_state_roundtrip_and_defaults() {
        // Alter Snapshot (ohne die additiven Felder) bleibt lesbar
        let old = r#"{"type":"state_snapshot","state":{"source":null,"channel":"5C","gain":{"lna":40,"vga":24,"amp":false},"agc":true,"synced":true,"ensemble":[4284,"DR Deutschland"],"services":[],"primary":[53776,0],"background":null,"volume_percent":70,"muted":false,"timeshift":null,"recording":false,"ews_enabled":true,"ews_autoswitch":true}}"#;
        let ev: Event = serde_json::from_str(old).unwrap();
        let Event::StateSnapshot { state } = ev else { panic!("kein state_snapshot") };
        assert_eq!(state.ensemble, Some((0x10BC, "DR Deutschland".into())));
        assert!(state.epg_enabled && state.tii_enabled);
        assert_eq!(state.tii_threshold, 6);
        assert_eq!(state.scopes, ScopeSettings::default());
        assert!(state.running.is_empty());
        // Neuer Snapshot (C++-Seite emitState) mit laufendem Dienst
        let new = r#"{"state":{"agc":true,"background":[3771797692,0],"channel":"5C","clock_time":{"lto_minutes":120,"unix_utc":1789194000},"ensemble":[4284,"DR Deutschland"],"epg_enabled":true,"ews_autoswitch":true,"ews_enabled":true,"gain":{"amp":false,"lna":40,"vga":24},"muted":false,"ppm":0,"primary":[53776,0],"recording":false,"running":[{"codec":{"codec":"he_aac","ps":false,"sample_rate":48000,"sbr":true},"dl_plus":{"item_running":true,"item_toggle":false,"tags":[[1,"Titel"],[4,"Autor"]]},"dls":"Nachrichten","is_audio":true,"name":"Dlf","recording":{"active":true,"bytes":192000,"path":"C:/rec/dlf.wav","seconds":1.0},"scids":0,"sid":53776,"slide":{"data_b64":"/9j/","mime":"image/jpeg","name":"a.jpg"},"slot":"primary","stereo":true},{"codec":{"codec":"data"},"dl_plus":null,"dls":null,"is_audio":false,"name":"EPG Deutschland","recording":{"active":false,"bytes":0,"path":null,"seconds":0.0},"scids":0,"sid":3771797692,"slide":null,"slot":"background","stereo":false}],"scanning":false,"scopes":{"iq":true,"rate_hz":5,"spectrum":false},"services":[],"snr":14.5,"source":{"kind":"hack_rf","serial":null},"synced":true,"tii_dx_mode":false,"tii_enabled":true,"tii_threshold":6,"timeshift":null,"volume_percent":70},"type":"state_snapshot"}"#;
        let ev: Event = serde_json::from_str(new).unwrap();
        let Event::StateSnapshot { state } = ev else { panic!("kein state_snapshot") };
        assert_eq!(state.running.len(), 2);
        let dlf = &state.running[0];
        assert_eq!(dlf.slot, ServiceSlot::Primary);
        assert_eq!(dlf.codec, Some(Codec::HeAac { sbr: true, ps: false, sample_rate: 48000 }));
        assert_eq!(dlf.dls.as_deref(), Some("Nachrichten"));
        assert_eq!(dlf.dl_plus.as_ref().unwrap().tags[1], (4, "Autor".to_string()));
        assert_eq!(dlf.slide.as_ref().unwrap().mime, "image/jpeg");
        assert!(dlf.recording.active);
        assert_eq!(state.running[1].codec, Some(Codec::Data));
        assert_eq!(state.clock_time, Some(ClockTimeState { unix_utc: 1789194000, lto_minutes: 120 }));
        assert_eq!(state.scopes, ScopeSettings { spectrum: false, iq: true, rate_hz: 5 });
        // Roundtrip
        let s = serde_json::to_string(&Event::StateSnapshot { state: state.clone() }).unwrap();
        let back: Event = serde_json::from_str(&s).unwrap();
        assert_eq!(back, Event::StateSnapshot { state });
        // Scopes-Kommando und iq_samples-Ereignis
        assert_eq!(serde_json::to_string(&Command::SetScopes { spectrum: true, iq: true, rate_hz: 5 }).unwrap(),
                   r#"{"type":"set_scopes","spectrum":true,"iq":true,"rate_hz":5}"#);
        let ev: Event = serde_json::from_str(r#"{"iq_b64":"f4EAAA==","type":"iq_samples"}"#).unwrap();
        assert!(ev.is_latest_wins());
    }

    #[test]
    fn channel_table() {
        assert_eq!(channel_frequency_khz("5C"), Some(178_352));
        assert_eq!(channel_frequency_khz("11d"), Some(222_064));
        assert_eq!(channel_frequency_khz("99Z"), None);
    }
}
