//! DAB Classic – `dab-app`: Anwendungslogik ohne GUI.
//!
//! Alles, was nicht Empfang (Kern) und nicht Darstellung (Svelte) ist, lebt
//! hier: Datenordner (portabel oder Benutzerprofil), Einstellungen, die zehn
//! Stationsspeicher, spaeter Timer, EPG-Cache, Timeshift-Steuerung,
//! Aufnahme-Regeln und die Schnittliste der Musik-Trennung. Alles hier ist
//! ohne Fenster testbar und wird auch vom Headless-Treiber `dab-cli` genutzt.

pub mod app;
pub mod epg;
pub mod ews_location;
pub mod favorites;
pub mod logos;
pub mod music;
pub mod paths;
pub mod presets;
pub mod radiodns;
pub mod recording;
pub mod settings;
pub mod sleep;
pub mod state;
pub mod stations;
pub mod storage;
pub mod tii;
pub mod timer;
pub mod timeshift;
pub mod tpeg;
pub mod traffic;

pub use app::{App, AppError, AppEvent, Effects, NoticeLevel, PresetStatus, StoreResult, DEFAULT_HACKRF_GAIN, PRESET_TIMEOUT};
pub use epg::{EpgCache, NowNext, Programme, ProgrammeBrief};
pub use logos::{LogoCache, LogoSize};
pub use music::{MusicDetector, MUSIC_MAX};
pub use paths::DataDirs;
pub use presets::{Preset, Presets, PRESET_SLOTS};
pub use radiodns::{Fetcher as RadioDnsFetcher, Job as RadioDnsJob, JobResult as RadioDnsResult, NetFetcher, RadioDnsStatus};
pub use recording::RecordingInfo;
pub use settings::{Panels, Settings};
pub use sleep::{SleepAction, SleepState};
pub use state::AppState;
pub use stations::{StationEntry, Stations};
pub use tii::{DebugState, TiiDatabase, TiiSeen, Transmitter};
pub use timeshift::{TimeshiftInfo, TimeshiftNotice, CAPACITY_MAX_S, CAPACITY_MIN_S, SKIP_STEP_S};
pub use traffic::{TrafficEntry, TRAFFIC_HISTORY_MAX};
pub use timer::{AddOutcome, Conflict, EpgTimerRequest, Timer, TimerFireStatus, TimerKind, Timers};
