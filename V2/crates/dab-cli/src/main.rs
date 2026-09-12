//! DAB Classic – `dab-cli`: Headless-Treiber.
//!
//! ```text
//! dab-cli replay <datei.uff|.iq> [--service NAME|0xSID] [--wav out.wav] [--events out.jsonl] [--duration S] [--loop] [--fast]
//!                [--epg-dir PFAD]
//! dab-cli live   --channel 5C [--service Dlf] [--device hackrf|rtlsdr] [--events out.jsonl] [--duration S]
//!                [--gain LNA,VGA,AMP] [--no-agc] [--iq-dump out.uff] [--epg-dir PFAD]
//! dab-cli scan   [--device hackrf|rtlsdr] [--gain LNA,VGA,AMP] [--events out.jsonl]   # Band III, Tabelle
//! dab-cli spike  [--seconds 10]        # IPC-Durchsatz mit dem Kernstub messen
//! ```
//! Der Kern wird ueber `--core PFAD`, `DABCORE=PFAD` oder die Standardorte gefunden.

use anyhow::{anyhow, Context, Result};
use clap::{Parser, Subcommand};
use crossbeam_channel::RecvTimeoutError;
use dab_api::{Command, Event, Gain, ScanMode, ServiceSlot, SourceKind};
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
        /// Nach S Sekunden *Dateizeit* beenden (Standard: bis Dateiende); mit --fast entsprechend frueher
        #[arg(long)]
        duration: Option<f64>,
        /// Datei in Schleife abspielen
        #[arg(long)]
        r#loop: bool,
        /// Ohne Echtzeit-Pacing (so schnell wie moeglich)
        #[arg(long)]
        fast: bool,
        /// EPG-XML (<eid>/<yyyymmdd>_<SID>_SI.xml, list.xml) und MOT-Objekte
        /// (<eid>/<name>) zur Abnahme in diesen Ordner schreiben
        #[arg(long)]
        epg_dir: Option<PathBuf>,
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
        /// Gain-Satz LNA,VGA,AMP (HackRF 0-40, 0-62, 0|1; RTL-SDR: LNA = Tuner-Gain in 0,1 dB)
        #[arg(long)]
        gain: Option<String>,
        /// SNR-Nachfuehrung (AGC) abschalten
        #[arg(long)]
        no_agc: bool,
        /// IQ-Dump der Quelle als .uff mitschreiben
        #[arg(long)]
        iq_dump: Option<PathBuf>,
        /// EPG-XML und MOT-Objekte zur Abnahme in diesen Ordner schreiben
        #[arg(long)]
        epg_dir: Option<PathBuf>,
    },
    /// Band-III-Scan
    Scan {
        #[arg(long, default_value = "hackrf")]
        device: String,
        #[arg(long)]
        gain: Option<String>,
        #[arg(long)]
        events: Option<PathBuf>,
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
    // Replay: --duration ist Dateizeit und wird vom Kern umgesetzt (file_ended).
    if let Sub::Replay { duration: Some(d), .. } = &cli.cmd {
        cfg.args.push("--duration".into());
        cfg.args.push(format!("{d}"));
    }
    log::info!("Kern: {}", exe.display());
    let mut backend = IpcBackend::spawn(cfg).context("Kern starten")?;
    log::info!("Kern gestartet, PID {}", backend.pid());

    let result = match cli.cmd {
        Sub::Replay { file, service, wav, events, duration: _, r#loop, fast, epg_dir } => {
            let file = file.canonicalize().unwrap_or(file);
            run_session(
                &backend,
                SourceKind::File { path: file, r#loop, fast },
                None,
                service,
                wav,
                events,
                None,   // Dateizeit-Limit setzt der Kern um (--duration oben)
                LiveOptions { epg_dir, ..LiveOptions::default() },
            )
        }
        Sub::Live { channel, service, device, events, duration, gain, no_agc, iq_dump, epg_dir } => run_session(
            &backend,
            device_source(&device)?,
            Some(channel),
            service,
            None,
            events,
            duration,
            LiveOptions { gain: gain.as_deref().map(parse_gain).transpose()?, agc: !no_agc, iq_dump, epg_dir },
        ),
        Sub::Scan { device, gain, events } => run_scan(&backend, device_source(&device)?, gain.as_deref().map(parse_gain).transpose()?, events),
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

/// "LNA,VGA,AMP" -> Gain
fn parse_gain(s: &str) -> Result<Gain> {
    let parts: Vec<&str> = s.split(',').map(|p| p.trim()).collect();
    let num = |i: usize| -> Result<u16> {
        parts.get(i).map(|p| p.parse::<u16>().map_err(|e| anyhow!("--gain {s}: {e}"))).unwrap_or(Ok(0))
    };
    Ok(Gain {
        lna: num(0)?,
        vga: num(1)?,
        amp: matches!(parts.get(2).copied(), Some("1") | Some("true") | Some("on")),
    })
}

#[derive(Default)]
struct LiveOptions {
    gain: Option<Gain>,
    agc: bool,
    iq_dump: Option<PathBuf>,
    epg_dir: Option<PathBuf>,
}

/// Base64 (Standard-Alphabet, mit/ohne Padding) -> Bytes; ungueltige Zeichen werden uebersprungen.
/// Eigene Minimalfassung, damit dab-cli keine weitere Abhaengigkeit braucht.
fn base64_decode(s: &str) -> Vec<u8> {
    let mut out = Vec::with_capacity(s.len() * 3 / 4);
    let mut acc: u32 = 0;
    let mut bits = 0;
    for c in s.bytes() {
        let v = match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'+' => 62,
            b'/' => 63,
            _ => continue,
        };
        acc = (acc << 6) | v as u32;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
            acc &= (1 << bits) - 1;
        }
    }
    out
}

/// Abnahme-Hilfe (nicht die spaetere App-Logik): schreibt `epg_object` als
/// `<eid>/<yyyymmdd>_<SID>_SI.xml` (Service-Information als `<eid>/list.xml`)
/// und `mot_object` als `<eid>/<name>`.
fn write_epg_file(dir: &std::path::Path, ev: &Event) -> Result<()> {
    let (sub, name, bytes): (u16, String, Vec<u8>) = match ev {
        Event::EpgObject { eid, sid, date_yyyymmdd, xml, .. } => {
            let name = if *sid == 0 { "list.xml".to_string() } else { format!("{date_yyyymmdd}_{sid:04X}_SI.xml") };
            (*eid, name, xml.as_bytes().to_vec())
        }
        Event::MotObject { eid, name, data_b64, .. } => {
            let clean: String = name.chars().map(|c| if matches!(c, '/' | '\\' | ':' | '*' | '?' | '"' | '<' | '>' | '|') { '_' } else { c }).collect();
            (*eid, clean, base64_decode(data_b64))
        }
        _ => return Ok(()),
    };
    let d = dir.join(format!("{sub:04X}"));
    std::fs::create_dir_all(&d).with_context(|| format!("{}", d.display()))?;
    let p = d.join(name);
    std::fs::write(&p, bytes).with_context(|| format!("{}", p.display()))?;
    Ok(())
}

/// Wartet auf `Ready`, oeffnet die Quelle, waehlt ggf. Kanal und Dienst und
/// protokolliert Ereignisse bis Dateiende, Zeitlimit oder Kern-Ende.
/// Der Kern verarbeitet Kommandos der Reihe nach: `SetChannel` direkt nach
/// `OpenDevice` trifft auf die schon geoeffnete Quelle (oder wird gemerkt).
fn run_session(
    backend: &IpcBackend,
    source: SourceKind,
    channel: Option<String>,
    service: Option<String>,
    wav: Option<PathBuf>,
    events_path: Option<PathBuf>,
    duration: Option<f64>,
    live: LiveOptions,
) -> Result<()> {
    let tx = backend.commands();
    let rx = backend.events();
    let mut sink = EventSink::open(events_path)?;

    wait_ready(&rx, &mut sink)?;
    let is_device = !matches!(source, SourceKind::File { .. });
    if let Some(g) = live.gain {
        tx.send(Command::SetGain { gain: g })?;
    }
    if is_device {
        tx.send(Command::SetAgc { enabled: live.agc })?;
    }
    tx.send(Command::OpenDevice { source })?;
    if let Some(ch) = channel {
        tx.send(Command::SetChannel { channel: ch })?;
    }
    if let Some(p) = live.iq_dump {
        tx.send(Command::StartIqDump { path: p })?;
    }
    let mut snr: Vec<f32> = Vec::new();
    let mut gain_changes = 0u32;
    let mut stats = (0u64, 0u64, 0u64, 0u64, 0u32);   // frame, rs, aac, rs_corr, n

    let start = Instant::now();
    let mut services: HashMap<u32, dab_api::ServiceInfo> = HashMap::new();
    let mut selected = false;
    let mut candidate: Option<(u32, u8)> = None;
    let mut candidate_since: Option<Instant> = None;
    // Ein Teilstring-Treffer wartet so lange auf einen exakten Treffer
    // (die FIC meldet die Dienste in beliebiger Reihenfolge, "Dlf Kultur"
    // kommt oft vor "Dlf").
    const CANDIDATE_GRACE: Duration = Duration::from_millis(1500);
    let mut wav = wav;
    let wanted = service.map(|s| s.trim().to_string());

    loop {
        if let Some(d) = duration {
            if start.elapsed().as_secs_f64() >= d {
                log::info!("Zeitlimit erreicht");
                break;
            }
        }
        // Teilstring-Kandidat uebernehmen, wenn die Schonfrist ohne exakten
        // Treffer abgelaufen ist.
        if !selected {
            if let (Some((sid, scids)), Some(since)) = (candidate, candidate_since) {
                if since.elapsed() >= CANDIDATE_GRACE {
                    tx.send(Command::SelectService { sid, scids, slot: ServiceSlot::Primary })?;
                    selected = true;
                }
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
        if let Some(d) = &live.epg_dir {
            write_epg_file(d, &ev)?;
        }

        match &ev {
            Event::ServiceAdded { service } => {
                // Exakter Name/SId sofort; ein Teilstring-Treffer ("Dlf" passt
                // auch auf "Dlf Kultur") erst nach der Schonfrist.
                services.insert(service.sid, service.clone());
                if !selected {
                    if let Some(w) = &wanted {
                        match service_match(service, w) {
                            2 => {
                                tx.send(Command::SelectService { sid: service.sid, scids: service.scids, slot: ServiceSlot::Primary })?;
                                selected = true;
                            }
                            1 if candidate.is_none() => {
                                candidate = Some((service.sid, service.scids));
                                candidate_since = Some(Instant::now());
                            }
                            _ => {}
                        }
                    }
                }
            }
            Event::ServiceStarted { slot: ServiceSlot::Primary, .. } => {
                if let Some(p) = wav.take() {
                    tx.send(Command::StartRecording { path: p, format: dab_api::RecFormat::Wav, slot: ServiceSlot::Primary, sid: None, pre_s: 0.0 })?;
                }
            }
            Event::Snr { db } => snr.push(*db),
            Event::GainChanged { .. } => gain_changes += 1,
            Event::ServiceStats { slot: ServiceSlot::Primary, frame_errors, rs_errors, aac_errors, rs_corrections, .. } => {
                stats.0 += *frame_errors as u64;
                stats.1 += *rs_errors as u64;
                stats.2 += *aac_errors as u64;
                stats.3 += *rs_corrections as u64;
                stats.4 += 1;
            }
            Event::FileEnded | Event::Exiting { .. } => break,
            Event::DeviceError { message } => return Err(anyhow!("Geraetefehler: {message}")),
            _ => {}
        }
    }

    if wanted.is_some() && !selected {
        log::warn!("Dienst nicht gefunden; bekannte Dienste: {:?}", services.values().map(|s| &s.name).collect::<Vec<_>>());
    }
    if !snr.is_empty() {
        let min = snr.iter().cloned().fold(f32::INFINITY, f32::min);
        let max = snr.iter().cloned().fold(f32::NEG_INFINITY, f32::max);
        let mean = snr.iter().sum::<f32>() / snr.len() as f32;
        println!("SNR: min {min:.1} / mittel {mean:.1} / max {max:.1} dB ({} Werte), gain_changed: {gain_changes}", snr.len());
    }
    if stats.4 > 0 {
        println!("Primary-Statistik ueber {} s: Superframe-Fehler {}, RS-Fehler {}, AAC-Fehler {}, RS-Korrekturen {}",
                 stats.4, stats.0, stats.1, stats.2, stats.3);
    }
    Ok(())
}

/// 2 = exakter Name oder 0xSID, 1 = Name-Teilstring, 0 = kein Treffer.
fn service_match(s: &dab_api::ServiceInfo, wanted: &str) -> u8 {
    if let Some(hex) = wanted.strip_prefix("0x").or_else(|| wanted.strip_prefix("0X")) {
        return if u32::from_str_radix(hex, 16).map(|sid| sid == s.sid).unwrap_or(false) { 2 } else { 0 };
    }
    let a = s.name.trim().to_lowercase();
    let b = wanted.trim().to_lowercase();
    if a == b {
        2
    } else if a.contains(&b) {
        1
    } else {
        0
    }
}

fn run_scan(backend: &IpcBackend, source: SourceKind, gain: Option<Gain>, events_path: Option<PathBuf>) -> Result<()> {
    let tx = backend.commands();
    let rx = backend.events();
    let mut sink = EventSink::open(events_path)?;
    wait_ready(&rx, &mut sink)?;
    if let Some(g) = gain {
        tx.send(Command::SetGain { gain: g })?;
    }
    tx.send(Command::OpenDevice { source })?;
    let channels: Vec<String> = dab_api::BAND_III.iter().map(|(c, _)| c.to_string()).collect();
    tx.send(Command::StartScan { channels, mode: ScanMode::Single })?;
    let start = Instant::now();
    let mut results: Vec<(String, Option<u16>, Option<String>, usize, f32)> = Vec::new();
    let mut amp_retries = 0u32;
    for ev in rx.iter() {
        sink.write(start.elapsed(), &ev)?;
        print_event(start.elapsed(), &ev);
        match ev {
            Event::ScanResult { channel, eid, ensemble, services, snr } => results.push((channel, eid, ensemble, services.len(), snr)),
            Event::Log { text, .. } if text.contains("AMP") => amp_retries += 1,
            Event::DeviceError { message } => return Err(anyhow!("Geraetefehler: {message}")),
            Event::ScanFinished | Event::Exiting { .. } => break,
            _ => {}
        }
    }
    let elapsed = start.elapsed().as_secs_f64();
    println!();
    println!("{:<5} {:<6} {:<24} {:>7} {:>8}", "Kanal", "EId", "Ensemble", "Dienste", "SNR dB");
    let mut found = 0;
    for (ch, eid, name, n, snr) in &results {
        if eid.is_some() {
            found += 1;
        }
        println!(
            "{:<5} {:<6} {:<24} {:>7} {:>8.1}",
            ch,
            eid.map(|e| format!("{e:04X}")).unwrap_or_else(|| "-".into()),
            name.as_deref().unwrap_or("-"),
            n,
            snr
        );
    }
    println!("{} Kanaele, {found} Ensembles, {amp_retries} AMP-Retries, Dauer {elapsed:.1} s", results.len());
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
        Event::GainChanged { .. } => "gain_changed",
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
        Event::ServiceStarted { slot, sid, codec, stereo, .. } => println!("{ts}  START {slot:?} {sid:04X} {codec:?} stereo={stereo}"),
        Event::ServiceStopped { slot, sid } => println!("{ts}  STOP  {slot:?} {sid:04X}"),
        Event::Dls { sid, text, .. } => println!("{ts}  DLS  [{sid:04X}] {text}"),
        Event::DlPlus { sid, item_toggle, item_running, tags, .. } => println!("{ts}  DL+  [{sid:04X}] IT={} IR={} {tags:?}", *item_toggle as u8, *item_running as u8),
        Event::MotSlide { sid, mime, name, data_b64, .. } => println!("{ts}  SLIDE [{sid:04X}] {name} {mime} {} B", data_b64.len() * 3 / 4),
        Event::MotObject { sid, content_type, name, data_b64, .. } => println!("{ts}  MOT   [{sid:04X}] {name} ct=0x{content_type:04X} {} B", data_b64.len() * 3 / 4),
        Event::EpgObject { sid, date_yyyymmdd, name, xml, .. } => println!("{ts}  EPG   [{sid:04X}] {date_yyyymmdd} {name} {} B XML", xml.len()),
        Event::AudioFormat { rate, channels } => println!("{ts}  AUDIO {rate} Hz, {channels} Kanaele"),
        Event::EwsAlert { phase, sub_ch, stage, stage_raw, iid, locations, is_test } => println!("{ts}  EWS  {phase:?} subCh={sub_ch} stage={stage} (roh 0x{stage_raw:02X}) iid={iid} test={is_test} {} Orte", locations.len()),
        Event::EwsAlive { sub_ch: Some(sub_ch) } => println!("{ts}  EWS  alive subCh={sub_ch}"),
        Event::EwsAlive { sub_ch: None } => println!("{ts}  EWS  heartbeat"),
        Event::EwsPresent => println!("{ts}  EWS  vorhanden"),
        Event::EwfAlarm { active, sub_ch } => println!("{ts}  EWF  alarm={active} subCh={sub_ch}"),
        Event::EwsSwitched { to_sid, from_sid } => println!("{ts}  EWS  umgeschaltet {from_sid:?} -> {to_sid:04X}"),
        Event::ClockTime { unix_utc, lto_minutes } => println!("{ts}  ZEIT utc={unix_utc} lto={lto_minutes}"),
        Event::Log { level, text } => println!("{ts}  LOG  {level:?}: {text}"),
        Event::DeviceOpened { name, serial, bit_depth } => println!("{ts}  GERAET {name} {serial} {bit_depth} Bit"),
        Event::DeviceError { message } => println!("{ts}  FEHLER {message}"),
        Event::GainChanged { lna, vga, amp, agc } => println!("{ts}  GAIN  LNA {lna} VGA {vga} AMP {} AGC {}", *amp as u8, *agc as u8),
        Event::NoSignal { channel } => println!("{ts}  KEIN SIGNAL auf {channel}"),
        Event::ScanProgress { channel, index, total } => println!("{ts}  SCAN {channel} ({}/{total})", index + 1),
        Event::FileEnded => println!("{ts}  DATEIENDE"),
        Event::ScanResult { channel, ensemble, services, snr, .. } => println!("{ts}  SCAN {channel}: {} ({} Dienste, SNR {snr:.1})", ensemble.as_deref().unwrap_or("-"), services.len()),
        Event::RecordingState { active, path, seconds, .. } => println!("{ts}  REC  {} {:?} {seconds:.1} s", if *active { "laeuft" } else { "aus" }, path),
        Event::Exiting { reason } => println!("{ts}  ENDE {reason}"),
        other => println!("{ts}  {}", event_name(other)),
    }
}
