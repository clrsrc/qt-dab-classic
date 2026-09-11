//! DAB Classic – `dab-cli`: Headless-Treiber.
//!
//! ```text
//! dab-cli replay <datei.uff|.iq> [--service NAME|0xSID] [--wav out.wav] [--events out.jsonl] [--duration S] [--loop] [--fast]
//! dab-cli live   --channel 5C [--service Dlf] [--device hackrf|rtlsdr] [--events out.jsonl]
//! dab-cli spike  [--seconds 10]        # IPC-Durchsatz mit dem Kernstub messen
//! ```
//! Der Kern wird ueber `--core PFAD`, `DABCORE=PFAD` oder die Standardorte gefunden.

use anyhow::{anyhow, Context, Result};
use clap::{Parser, Subcommand};
use crossbeam_channel::RecvTimeoutError;
use dab_api::{Command, Event, ScanMode, ServiceSlot, SourceKind};
use dab_core::{locate_core, CoreBackend, IpcBackend, IpcConfig};
use std::collections::HashMap;
use std::io::Write;
use std::path::PathBuf;
use std::time::{Duration, Instant};

#[derive(Parser, Debug)]
#[command(name = "dab-cli", version, about = "DAB Classic Headless-Treiber")]
struct Cli {
    /// Pfad zu dabcored.exe (sonst DABCORE oder Standardorte)
    #[arg(long, global = true)]
    core: Option<PathBuf>,
    /// Audio-Ausgabe im Kern abschalten
    #[arg(long, global = true)]
    no_audio: bool,
    #[command(subcommand)]
    cmd: Sub,
}

#[derive(Subcommand, Debug)]
enum Sub {
    /// Datei abspielen (.uff oder rohe int8-IQ)
    Replay {
        file: PathBuf,
        /// Dienstname (Teilstring) oder 0xSID
        #[arg(long)]
        service: Option<String>,
        /// Audio als WAV schreiben (Audio-Dump im Kern)
        #[arg(long)]
        wav: Option<PathBuf>,
        /// Alle Ereignisse als JSON-Zeilen mitschreiben
        #[arg(long)]
        events: Option<PathBuf>,
        /// Nach S Sekunden beenden (Standard: bis Dateiende)
        #[arg(long)]
        duration: Option<f64>,
        /// Datei in Schleife abspielen
        #[arg(long)]
        r#loop: bool,
        /// Ohne Echtzeit-Pacing (so schnell wie moeglich)
        #[arg(long)]
        fast: bool,
    },
    /// Live-Empfang
    Live {
        #[arg(long, default_value = "5C")]
        channel: String,
        #[arg(long)]
        service: Option<String>,
        #[arg(long, default_value = "hackrf")]
        device: String,
        #[arg(long)]
        events: Option<PathBuf>,
        #[arg(long)]
        duration: Option<f64>,
    },
    /// Band-III-Scan
    Scan {
        #[arg(long, default_value = "hackrf")]
        device: String,
    },
    /// IPC-Durchsatz messen (Kernstub sendet Testereignisse)
    Spike {
        #[arg(long, default_value_t = 10.0)]
        seconds: f64,
    },
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let cli = Cli::parse();

    let exe = locate_core(cli.core.as_deref())
        .ok_or_else(|| anyhow!("dabcored.exe nicht gefunden (--core, DABCORE oder core-cpp/build/)"))?;
    let mut cfg = IpcConfig::new(&exe);
    if cli.no_audio {
        cfg.args.push("--no-audio".into());
    }
    log::info!("Kern: {}", exe.display());
    let mut backend = IpcBackend::spawn(cfg).context("Kern starten")?;
    log::info!("Kern gestartet, PID {}", backend.pid());

    let result = match cli.cmd {
        Sub::Replay { file, service, wav, events, duration, r#loop, fast } => {
            let file = file.canonicalize().unwrap_or(file);
            run_session(
                &backend,
                SourceKind::File { path: file, r#loop, fast },
                None,
                service,
                wav,
                events,
                duration,
            )
        }
        Sub::Live { channel, service, device, events, duration } => run_session(
            &backend,
            device_source(&device)?,
            Some(channel),
            service,
            None,
            events,
            duration,
        ),
        Sub::Scan { device } => run_scan(&backend, device_source(&device)?),
        Sub::Spike { seconds } => run_spike(&backend, seconds),
    };

    backend.shutdown();
    result
}

fn device_source(name: &str) -> Result<SourceKind> {
    match name.to_ascii_lowercase().as_str() {
        "hackrf" => Ok(SourceKind::HackRf { serial: None }),
        "rtlsdr" | "rtl-sdr" | "rtl" => Ok(SourceKind::RtlSdr { index: 0 }),
        other => Err(anyhow!("unbekanntes Geraet: {other}")),
    }
}

/// Wartet auf `Ready`, oeffnet die Quelle, waehlt ggf. Kanal und Dienst und
/// protokolliert Ereignisse bis Dateiende, Zeitlimit oder Kern-Ende.
fn run_session(
    backend: &IpcBackend,
    source: SourceKind,
    channel: Option<String>,
    service: Option<String>,
    wav: Option<PathBuf>,
    events_path: Option<PathBuf>,
    duration: Option<f64>,
) -> Result<()> {
    let tx = backend.commands();
    let rx = backend.events();
    let mut sink = EventSink::open(events_path)?;

    wait_ready(&rx, &mut sink)?;
    tx.send(Command::OpenDevice { source })?;
    if let Some(ch) = channel {
        tx.send(Command::SetChannel { channel: ch })?;
    }

    let start = Instant::now();
    let mut services: HashMap<u32, dab_api::ServiceInfo> = HashMap::new();
    let mut selected = false;
    let mut wav = wav;
    let wanted = service.map(|s| s.trim().to_string());

    loop {
        if let Some(d) = duration {
            if start.elapsed().as_secs_f64() >= d {
                log::info!("Zeitlimit erreicht");
                break;
            }
        }
        let ev = match rx.recv_timeout(Duration::from_millis(200)) {
            Ok(ev) => ev,
            Err(RecvTimeoutError::Timeout) => {
                if !backend.is_alive() {
                    return Err(anyhow!("Kern unerwartet beendet"));
                }
                continue;
            }
            Err(RecvTimeoutError::Disconnected) => break,
        };
        sink.write(start.elapsed(), &ev)?;
        print_event(start.elapsed(), &ev);

        match &ev {
            Event::ServiceAdded { service } => {
                services.insert(service.sid, service.clone());
                if !selected {
                    if let Some(w) = &wanted {
                        if service_matches(service, w) {
                            tx.send(Command::SelectService { sid: service.sid, scids: service.scids, slot: ServiceSlot::Primary })?;
                            selected = true;
                        }
                    }
                }
            }
            Event::ServiceStarted { .. } => {
                if let Some(p) = wav.take() {
                    tx.send(Command::StartRecording { path: p, format: dab_api::RecFormat::Wav, slot: ServiceSlot::Primary })?;
                }
            }
            Event::FileEnded | Event::Exiting { .. } => break,
            Event::DeviceError { message } => return Err(anyhow!("Geraetefehler: {message}")),
            _ => {}
        }
    }

    if wanted.is_some() && !selected {
        log::warn!("Dienst nicht gefunden; bekannte Dienste: {:?}", services.values().map(|s| &s.name).collect::<Vec<_>>());
    }
    Ok(())
}

fn service_matches(s: &dab_api::ServiceInfo, wanted: &str) -> bool {
    if let Some(hex) = wanted.strip_prefix("0x").or_else(|| wanted.strip_prefix("0X")) {
        return u32::from_str_radix(hex, 16).map(|sid| sid == s.sid).unwrap_or(false);
    }
    s.name.to_lowercase().contains(&wanted.to_lowercase())
}

fn run_scan(backend: &IpcBackend, source: SourceKind) -> Result<()> {
    let tx = backend.commands();
    let rx = backend.events();
    let mut sink = EventSink::open(None)?;
    wait_ready(&rx, &mut sink)?;
    tx.send(Command::OpenDevice { source })?;
    let channels: Vec<String> = dab_api::BAND_III.iter().map(|(c, _)| c.to_string()).collect();
    tx.send(Command::StartScan { channels, mode: ScanMode::Single })?;
    let start = Instant::now();
    for ev in rx.iter() {
        print_event(start.elapsed(), &ev);
        match ev {
            Event::ScanFinished | Event::Exiting { .. } => break,
            _ => {}
        }
    }
    Ok(())
}

fn run_spike(backend: &IpcBackend, seconds: f64) -> Result<()> {
    let tx = backend.commands();
    let rx = backend.events();
    let mut sink = EventSink::open(None)?;
    wait_ready(&rx, &mut sink)?;
    // Der Kernstub (Spike 1) sendet nach OpenDevice{File} synthetische Ereignisse.
    tx.send(Command::OpenDevice { source: SourceKind::File { path: PathBuf::from("spike"), r#loop: true, fast: false } })?;
    tx.send(Command::SetScopes { spectrum: true, iq: false, rate_hz: 10 })?;
    let start = Instant::now();
    let mut counts: HashMap<&'static str, u64> = HashMap::new();
    let mut bytes = 0u64;
    while start.elapsed().as_secs_f64() < seconds {
        match rx.recv_timeout(Duration::from_millis(100)) {
            Ok(ev) => {
                bytes += serde_json::to_string(&ev)?.len() as u64;
                *counts.entry(event_name(&ev)).or_default() += 1;
                if matches!(ev, Event::Exiting { .. }) {
                    break;
                }
            }
            Err(RecvTimeoutError::Timeout) => {
                if !backend.is_alive() {
                    return Err(anyhow!("Kern unerwartet beendet"));
                }
            }
            Err(RecvTimeoutError::Disconnected) => break,
        }
    }
    let el = start.elapsed().as_secs_f64();
    let total: u64 = counts.values().sum();
    println!("\nIPC-Spike: {total} Ereignisse in {el:.1} s = {:.0}/s, {:.1} kB/s", total as f64 / el, bytes as f64 / el / 1024.0);
    let mut v: Vec<_> = counts.into_iter().collect();
    v.sort_by(|a, b| b.1.cmp(&a.1));
    for (k, n) in v {
        println!("  {k:<20} {n:>8}");
    }
    Ok(())
}

fn wait_ready(rx: &crossbeam_channel::Receiver<Event>, sink: &mut EventSink) -> Result<()> {
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let ev = rx
            .recv_deadline(deadline)
            .map_err(|_| anyhow!("Kern meldet kein Ready innerhalb von 10 s"))?;
        sink.write(Duration::ZERO, &ev)?;
        match ev {
            Event::Ready { core_version, protocol_version, decoders } => {
                log::info!("Kern bereit: Version {core_version}, Protokoll {protocol_version}, Decoder {decoders:?}");
                if protocol_version != dab_api::PROTOCOL_VERSION {
                    log::warn!("Protokollversion {} erwartet", dab_api::PROTOCOL_VERSION);
                }
                return Ok(());
            }
            Event::Exiting { reason } => return Err(anyhow!("Kern beendet: {reason}")),
            _ => {}
        }
    }
}

struct EventSink(Option<std::io::BufWriter<std::fs::File>>);

impl EventSink {
    fn open(path: Option<PathBuf>) -> Result<Self> {
        Ok(Self(match path {
            Some(p) => Some(std::io::BufWriter::new(std::fs::File::create(&p).with_context(|| format!("{}", p.display()))?)),
            None => None,
        }))
    }
    fn write(&mut self, t: Duration, ev: &Event) -> Result<()> {
        if let Some(w) = &mut self.0 {
            writeln!(w, "{{\"t\":{:.3},\"event\":{}}}", t.as_secs_f64(), serde_json::to_string(ev)?)?;
        }
        Ok(())
    }
}

impl Drop for EventSink {
    fn drop(&mut self) {
        if let Some(w) = &mut self.0 {
            let _ = w.flush();
        }
    }
}

fn event_name(ev: &Event) -> &'static str {
    match ev {
        Event::Ready { .. } => "ready",
        Event::DeviceOpened { .. } => "device_opened",
        Event::DeviceClosed => "device_closed",
        Event::DeviceError { .. } => "device_error",
        Event::FileProgress { .. } => "file_progress",
        Event::FileEnded => "file_ended",
        Event::Synced { .. } => "synced",
        Event::NoSignal { .. } => "no_signal",
        Event::Snr { .. } => "snr",
        Event::FicQuality { .. } => "fic_quality",
        Event::FrequencyOffset { .. } => "frequency_offset",
        Event::EnsembleFound { .. } => "ensemble_found",
        Event::ServiceAdded { .. } => "service_added",
        Event::EnsembleReconfigured => "ensemble_reconfigured",
        Event::ClockTime { .. } => "clock_time",
        Event::ServiceStarted { .. } => "service_started",
        Event::ServiceStopped { .. } => "service_stopped",
        Event::ServiceStats { .. } => "service_stats",
        Event::Dls { .. } => "dls",
        Event::DlPlus { .. } => "dl_plus",
        Event::MotSlide { .. } => "mot_slide",
        Event::MotObject { .. } => "mot_object",
        Event::EpgObject { .. } => "epg_object",
        Event::Announcement { .. } => "announcement",
        Event::AudioFormat { .. } => "audio_format",
        Event::AudioLevel { .. } => "audio_level",
        Event::AudioUnderrun { .. } => "audio_underrun",
        Event::AudioDevices { .. } => "audio_devices",
        Event::EwsPresent => "ews_present",
        Event::EwsAlert { .. } => "ews_alert",
        Event::EwsAlive { .. } => "ews_alive",
        Event::EwfAlarm { .. } => "ewf_alarm",
        Event::EwsSwitched { .. } => "ews_switched",
        Event::RecordingState { .. } => "recording_state",
        Event::TimeshiftState { .. } => "timeshift_state",
        Event::ScanProgress { .. } => "scan_progress",
        Event::ScanResult { .. } => "scan_result",
        Event::ScanFinished => "scan_finished",
        Event::Tii { .. } => "tii",
        Event::Spectrum { .. } => "spectrum",
        Event::IqSamples { .. } => "iq_samples",
        Event::Log { .. } => "log",
        Event::StateSnapshot { .. } => "state_snapshot",
        Event::Exiting { .. } => "exiting",
    }
}

/// Kompakte Konsolenausgabe; latest-wins-Ereignisse werden nicht gedruckt.
fn print_event(t: Duration, ev: &Event) {
    if ev.is_latest_wins() {
        return;
    }
    let ts = format!("{:8.3}", t.as_secs_f64());
    match ev {
        Event::Ready { .. } => {}
        Event::Synced { synced } => println!("{ts}  SYNC {}", if *synced { "ja" } else { "nein" }),
        Event::EnsembleFound { eid, name, channel } => println!("{ts}  ENSEMBLE {name} ({eid:04X}) auf {channel}"),
        Event::ServiceAdded { service } => println!("{ts}  DIENST {:<20} SId {:04X} SubCh {:2} {} kbit/s", service.name, service.sid, service.sub_ch, service.bitrate_kbps),
        Event::ServiceStarted { sid, codec, stereo, .. } => println!("{ts}  START {sid:04X} {codec:?} stereo={stereo}"),
        Event::Dls { text, .. } => println!("{ts}  DLS  {text}"),
        Event::DlPlus { item_toggle, item_running, tags, .. } => println!("{ts}  DL+  IT={} IR={} {tags:?}", *item_toggle as u8, *item_running as u8),
        Event::MotSlide { mime, name, data_b64, .. } => println!("{ts}  SLIDE {name} {mime} {} B", data_b64.len() * 3 / 4),
        Event::EwsAlert { phase, sub_ch, stage, iid, locations, is_test } => println!("{ts}  EWS  {phase:?} subCh={sub_ch} stage={stage} iid={iid} test={is_test} {} Orte", locations.len()),
        Event::EwsAlive { sub_ch: Some(sub_ch) } => println!("{ts}  EWS  alive subCh={sub_ch}"),
        Event::EwsAlive { sub_ch: None } => println!("{ts}  EWS  heartbeat"),
        Event::EwsPresent => println!("{ts}  EWS  vorhanden"),
        Event::EwfAlarm { active, sub_ch } => println!("{ts}  EWF  alarm={active} subCh={sub_ch}"),
        Event::EwsSwitched { to_sid, from_sid } => println!("{ts}  EWS  umgeschaltet {from_sid:?} -> {to_sid:04X}"),
        Event::ClockTime { unix_utc, lto_minutes } => println!("{ts}  ZEIT utc={unix_utc} lto={lto_minutes}"),
        Event::Log { level, text } => println!("{ts}  LOG  {level:?}: {text}"),
        Event::DeviceOpened { name, serial, bit_depth } => println!("{ts}  GERAET {name} {serial} {bit_depth} Bit"),
        Event::DeviceError { message } => println!("{ts}  FEHLER {message}"),
        Event::FileEnded => println!("{ts}  DATEIENDE"),
        Event::ScanResult { channel, ensemble, services, snr, .. } => println!("{ts}  SCAN {channel}: {} ({} Dienste, SNR {snr:.1})", ensemble.as_deref().unwrap_or("-"), services.len()),
        Event::RecordingState { active, path, seconds, .. } => println!("{ts}  REC  {} {:?} {seconds:.1} s", if *active { "laeuft" } else { "aus" }, path),
        Event::Exiting { reason } => println!("{ts}  ENDE {reason}"),
        other => println!("{ts}  {}", event_name(other)),
    }
}
