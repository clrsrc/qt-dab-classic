//! `dab-cli zap`: Favoriten-Zapping mit der echten App-Zustandsmaschine.
//!
//! Treibt `dab_app::App` (dieselbe Logik wie die Tauri-Bruecke in
//! apps/desktop: `handle_event`, `tick`, `preset_recall`, Effekte als
//! Kommandos an den Kern) gegen den echten Kern und das echte Geraet, ruft
//! zufaellig Favoriten aus `<data>/presets.json` auf und bewertet jede
//! Umschaltung: Kam `service_started` des Ziels? Zeigt die App-Wahrheit
//! (`state.current`) und das Frontend-Modell (state.svelte.ts nachgebildet)
//! am Ende des Intervalls den Zieldienst? Ergebnis als Tabelle und optional
//! als JSON-Zeilen.
//!
//! ```text
//! dab-cli zap --data <ordner mit settings.json/presets.json> [--count 30] [--min-s 2] [--max-s 10]
//!             [--seed 1] [--events out.jsonl] [--settle-s 12]
//! ```
//! Der Datenordner wird von der App beschrieben (settings.json, presets.json,
//! stations.json) - eine Kopie verwenden.

use anyhow::{anyhow, Context, Result};
use crossbeam_channel::RecvTimeoutError;
use dab_api::{Event, ServiceSlot};
use dab_app::app::{App, AppEvent, Effects, PresetStatus};
use dab_app::paths::DataDirs;
use dab_app::presets::Preset;
use dab_core::{CoreBackend, IpcBackend};
use std::io::Write;
use std::path::PathBuf;
use std::time::{Duration, Instant};

pub struct ZapOptions {
    pub data: PathBuf,
    pub count: u32,
    pub min_s: f64,
    pub max_s: f64,
    pub seed: u64,
    pub settle_s: f64,
    pub events: Option<PathBuf>,
    /// Nur Favoriten desselben Kanals wie der aktuelle waehlen (wenn moeglich).
    pub same_only: bool,
    /// Nur Favoriten eines anderen Kanals waehlen.
    pub cross_only: bool,
    /// Intervall erst ab dem Audiostart zaehlen (bis dahin max. `lost_s`):
    /// misst die echte Umschaltzeit und erkennt verlorene Umschaltungen.
    pub wait_audio: bool,
    pub lost_s: f64,
}

/// xorshift64*: reproduzierbare Zufallsfolge ohne zusaetzliche Abhaengigkeit.
struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.0 = x;
        x.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }
    fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
    fn range_f64(&mut self, lo: f64, hi: f64) -> f64 {
        lo + (self.next() >> 11) as f64 / (1u64 << 53) as f64 * (hi - lo)
    }
}

/// Nachbildung von apps/desktop/src/lib/state.svelte.ts (nur `s.current`
/// und `s.pending`): rohe Kern-Ereignisse (ausser verschluckten) plus
/// App-Ereignisse, in derselben Reihenfolge wie die Bruecke sie sendet.
#[derive(Default)]
struct Frontend {
    current: Option<(u32, u8, bool)>,   // sid, scids, codec bekannt
    ensemble_eid: Option<u16>,
    pending_slot: Option<Option<usize>>,
}

impl Frontend {
    fn core_event(&mut self, ev: &Event) {
        match ev {
            Event::ServiceStarted { slot: ServiceSlot::Primary, sid, scids, .. } => self.current = Some((*sid, *scids, true)),
            Event::ServiceStopped { slot: ServiceSlot::Primary, sid } => {
                if self.current.map(|c| c.0 == *sid).unwrap_or(false) {
                    self.current = None;
                }
            }
            Event::EnsembleFound { eid, .. } => {
                // state.svelte.ts: nur ein echter Ensemble-Wechsel leert die
                // Liste, den laufenden Dienst nie.
                self.ensemble_eid = Some(*eid);
            }
            Event::DeviceClosed | Event::Exiting { .. } => {
                self.current = None;
                self.ensemble_eid = None;
                self.pending_slot = None;
            }
            _ => {}
        }
    }
    fn app_event(&mut self, ev: &AppEvent) {
        match ev {
            AppEvent::PresetStatus { slot, status, .. } => {
                self.pending_slot = if *status == PresetStatus::Tuning { Some(*slot) } else { None };
            }
            AppEvent::ChannelChanged { .. } => self.ensemble_eid = None,
            AppEvent::CurrentChanged { current } => {
                self.current = current.as_ref().map(|c| {
                    let same = self.current.map(|f| f.0 == c.sid && f.1 == c.scids).unwrap_or(false);
                    (c.sid, c.scids, c.codec.is_some() || (same && self.current.map(|f| f.2).unwrap_or(false)))
                });
            }
            _ => {}
        }
    }
}

struct Sink(Option<std::io::BufWriter<std::fs::File>>);
impl Sink {
    fn line(&mut self, t: f64, kind: &str, body: serde_json::Value) {
        if let Some(w) = &mut self.0 {
            let _ = writeln!(w, "{{\"t\":{t:.3},\"kind\":\"{kind}\",\"body\":{body}}}");
        }
    }
}

#[derive(Default, Clone, serde::Serialize)]
struct Switch {
    n: u32,
    slot: usize,
    name: String,
    channel: String,
    sid: u32,
    cross: bool,
    interval_s: f64,
    /// Sekunden bis PresetStatus (tuning / selected / not_found).
    status_selected_s: Option<f64>,
    status_not_found_s: Option<f64>,
    /// Sekunden bis service_started des Ziels.
    started_s: Option<f64>,
    /// Anzahl "Dienst nicht in der FIC"/"vorgemerkt"-Logs, no_signal, ensemble_found.
    no_signal: u32,
    ensemble_found_s: Option<f64>,
    service_added_s: Option<f64>,
    notices: Vec<String>,
    warn_logs: Vec<String>,
    /// Der Kern hat wegen einer Notfallwarnung (EWS) selbst umgeschaltet.
    ews_switched: bool,
    /// Zustand am Ende des Intervalls.
    app_current_ok: bool,
    app_codec_known: bool,
    fe_current_ok: bool,
    fe_pending_left: bool,
    verdict: String,
}

pub fn run(backend: &IpcBackend, opt: ZapOptions) -> Result<()> {
    let tx = backend.commands();
    let rx = backend.events();
    let mut sink = Sink(match &opt.events {
        Some(p) => Some(std::io::BufWriter::new(std::fs::File::create(p).with_context(|| p.display().to_string())?)),
        None => None,
    });
    let dirs = DataDirs::with_root(&opt.data, true);
    let mut app = App::new(dirs);
    if !app.settings.autostart {
        return Err(anyhow!("settings.json: autostart=false - der Test braucht Geraet + letzten Kanal aus den Settings"));
    }
    let slots: Vec<(usize, Preset)> =
        app.presets.slots.iter().enumerate().filter_map(|(i, p)| p.clone().map(|p| (i, p))).collect();
    if slots.len() < 2 {
        return Err(anyhow!("presets.json: mindestens zwei belegte Favoriten noetig"));
    }
    println!("Favoriten:");
    for (i, p) in &slots {
        println!("  {:2}  {:4}  SId {:04X}  {}", i + 1, p.channel, p.sid, p.name);
    }
    let mut rng = Rng(opt.seed.max(1) ^ 0x9E37_79B9_7F4A_7C15);
    let mut fe = Frontend::default();
    let t0 = Instant::now();
    let el = |t0: Instant| t0.elapsed().as_secs_f64();

    // Effekte wie apps/desktop lib.rs run_effects: Kommandos an den Kern,
    // App-Ereignisse ans (modellierte) Frontend.
    let mut run_effects = |fx: Effects, fe: &mut Frontend, sink: &mut Sink, sw: Option<&mut Switch>, now_s: f64| -> Result<()> {
        let mut sw = sw;
        for c in fx.commands {
            sink.line(now_s, "cmd", serde_json::to_value(&c)?);
            tx.send(c).map_err(|e| anyhow!("Kommando: {e}"))?;
        }
        for ev in fx.events {
            match &ev {
                AppEvent::PresetStatus { .. } | AppEvent::CurrentChanged { .. } | AppEvent::Notice { .. } => {
                    sink.line(now_s, "app", serde_json::to_value(&ev)?);
                }
                _ => {}
            }
            if let Some(sw) = sw.as_deref_mut() {
                if let AppEvent::PresetStatus { slot: Some(s), status, .. } = &ev {
                    if *s == sw.slot {
                        match status {
                            PresetStatus::Selected => { sw.status_selected_s.get_or_insert(now_s); }
                            PresetStatus::NotFound => { sw.status_not_found_s.get_or_insert(now_s); }
                            PresetStatus::Tuning => {}
                        }
                    }
                }
                if let AppEvent::Notice { text, .. } = &ev {
                    sw.notices.push(text.clone());
                }
            }
            fe.app_event(&ev);
        }
        Ok(())
    };

    // Ein Kern-Ereignis wie die Bruecke verarbeiten.
    let on_event = |app: &mut App, ev: &Event, fe: &mut Frontend, sink: &mut Sink, sw: Option<&mut Switch>, run_effects: &mut dyn FnMut(Effects, &mut Frontend, &mut Sink, Option<&mut Switch>, f64) -> Result<()>| -> Result<()> {
        let now = Instant::now();
        let now_s = el(t0);
        if !matches!(ev, Event::Spectrum { .. } | Event::IqSamples { .. } | Event::AudioLevel { .. } | Event::Tii { .. } | Event::ServiceStats { .. }) {
            sink.line(now_s, "core", serde_json::to_value(ev)?);
        }
        let swallowed = app.is_expected_stop(ev);
        let mut fx = app.handle_event(ev, now);
        if matches!(ev, Event::Ready { .. }) {
            fx.append(app.startup(now));
        }
        if !swallowed {
            fe.core_event(ev);
        }
        run_effects(fx, fe, sink, sw, now_s)
    };

    // Startphase: Ready abwarten, Geraet/Kanal/letzter Dienst laufen ueber
    // App::startup wie in der echten App.
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let ev = rx.recv_deadline(deadline).map_err(|_| anyhow!("Kern meldet kein Ready innerhalb von 10 s"))?;
        let ready = matches!(ev, Event::Ready { .. });
        on_event(&mut app, &ev, &mut fe, &mut sink, None, &mut run_effects)?;
        if ready {
            break;
        }
    }
    println!("Startphase {:.0} s (Geraet, letzter Kanal {:?}) ...", opt.settle_s, app.settings.last_channel);
    let settle_until = Instant::now() + Duration::from_secs_f64(opt.settle_s);
    while Instant::now() < settle_until {
        match rx.recv_timeout(Duration::from_millis(200)) {
            Ok(ev) => {
                if let Event::DeviceError { message } = &ev {
                    return Err(anyhow!("Geraetefehler: {message}"));
                }
                on_event(&mut app, &ev, &mut fe, &mut sink, None, &mut run_effects)?;
            }
            Err(RecvTimeoutError::Timeout) => {
                if !backend.is_alive() {
                    return Err(anyhow!("Kern unerwartet beendet"));
                }
                let fx = app.tick(Instant::now());
                run_effects(fx, &mut fe, &mut sink, None, el(t0))?;
            }
            Err(RecvTimeoutError::Disconnected) => return Err(anyhow!("Kern weg")),
        }
    }
    println!("Start: Kanal {:?}, Dienst {:?}", app.state.channel, app.state.current.as_ref().map(|c| format!("{:04X}", c.sid)));

    let mut results: Vec<Switch> = Vec::new();
    let mut last_slot: Option<usize> = None;
    for n in 1..=opt.count {
        // Ziel waehlen: nie derselbe Slot zweimal; same/cross nach Option.
        let cur_channel = app.state.channel.clone().unwrap_or_default().to_uppercase();
        let candidates: Vec<&(usize, Preset)> = slots
            .iter()
            .filter(|(i, p)| {
                Some(*i) != last_slot
                    && !app.state.current.as_ref().map(|c| c.sid == p.sid && cur_channel.eq_ignore_ascii_case(&p.channel)).unwrap_or(false)
                    && (!opt.same_only || p.channel.eq_ignore_ascii_case(&cur_channel))
                    && (!opt.cross_only || !p.channel.eq_ignore_ascii_case(&cur_channel))
            })
            .collect();
        let candidates = if candidates.is_empty() { slots.iter().filter(|(i, _)| Some(*i) != last_slot).collect() } else { candidates };
        let (slot, preset) = candidates[rng.below(candidates.len() as u64) as usize].clone();
        let interval = rng.range_f64(opt.min_s, opt.max_s);
        let cross = !preset.channel.eq_ignore_ascii_case(&cur_channel);
        let mut sw = Switch {
            n,
            slot,
            name: preset.name.clone(),
            channel: preset.channel.clone(),
            sid: preset.sid,
            cross,
            interval_s: interval,
            ..Default::default()
        };
        let click = Instant::now();
        let click_s = el(t0);
        sink.line(click_s, "click", serde_json::json!({"n": n, "slot": slot, "name": preset.name, "channel": preset.channel, "sid": preset.sid, "interval_s": interval}));
        print!("#{n:2} {:>4} -> {:4} {:<18} SId {:04X} ({:.1} s): ", cur_channel, preset.channel, preset.name, preset.sid, interval);
        std::io::stdout().flush().ok();
        match app.preset_recall(slot, click) {
            Ok(fx) => run_effects(fx, &mut fe, &mut sink, Some(&mut sw), click_s)?,
            Err(e) => {
                sw.notices.push(format!("preset_recall: {e}"));
            }
        }
        // Intervall abwarten und dabei alles verarbeiten. Mit --wait-audio
        // laeuft das Intervall erst ab dem Audiostart (bis dahin lost_s).
        let mut until = click + Duration::from_secs_f64(if opt.wait_audio { opt.lost_s } else { interval });
        let mut audio_at: Option<Instant> = None;
        while Instant::now() < until {
            if opt.wait_audio && audio_at.is_none() {
                if let Some(s) = sw.started_s {
                    let at = click + Duration::from_secs_f64(s);
                    audio_at = Some(at);
                    until = at + Duration::from_secs_f64(interval);
                }
            }
            let rest = until.saturating_duration_since(Instant::now()).min(Duration::from_millis(200));
            match rx.recv_timeout(rest) {
                Ok(ev) => {
                    let rel = click.elapsed().as_secs_f64();
                    match &ev {
                        Event::ServiceStarted { slot: ServiceSlot::Primary, sid, .. } if *sid == preset.sid => {
                            sw.started_s.get_or_insert(rel);
                        }
                        Event::ServiceAdded { service } if service.sid == preset.sid => {
                            sw.service_added_s.get_or_insert(rel);
                        }
                        Event::EnsembleFound { .. } => {
                            sw.ensemble_found_s.get_or_insert(rel);
                        }
                        Event::NoSignal { .. } => sw.no_signal += 1,
                        Event::EwsSwitched { .. } => sw.ews_switched = true,
                        Event::Log { level: dab_api::LogLevel::Warn | dab_api::LogLevel::Error, text } => sw.warn_logs.push(text.clone()),
                        Event::DeviceError { message } => return Err(anyhow!("Geraetefehler: {message}")),
                        _ => {}
                    }
                    on_event(&mut app, &ev, &mut fe, &mut sink, Some(&mut sw), &mut run_effects)?;
                }
                Err(RecvTimeoutError::Timeout) => {
                    if !backend.is_alive() {
                        return Err(anyhow!("Kern unerwartet beendet"));
                    }
                    let fx = app.tick(Instant::now());
                    run_effects(fx, &mut fe, &mut sink, Some(&mut sw), el(t0))?;
                }
                Err(RecvTimeoutError::Disconnected) => return Err(anyhow!("Kern weg")),
            }
        }
        // Bewertung am Ende des Intervalls (Statuszeiten relativ zum Klick).
        sw.status_selected_s = sw.status_selected_s.map(|s| s - click_s);
        sw.status_not_found_s = sw.status_not_found_s.map(|s| s - click_s);
        let cur = app.state.current.clone();
        sw.app_current_ok = cur.as_ref().map(|c| c.sid == preset.sid).unwrap_or(false);
        sw.app_codec_known = cur.as_ref().map(|c| c.codec.is_some()).unwrap_or(false);
        sw.fe_current_ok = fe.current.map(|c| c.0 == preset.sid).unwrap_or(false);
        sw.fe_pending_left = fe.pending_slot.is_some();
        sw.verdict = if sw.started_s.is_some() && sw.app_current_ok && sw.fe_current_ok && sw.status_selected_s.is_some() {
            "ok".into()
        } else if sw.ews_switched {
            "EWS (Kern hat umgeschaltet)".into()
        } else if opt.wait_audio && sw.started_s.is_none() {
            "VERLOREN".into()
        } else if sw.started_s.is_none() && sw.status_not_found_s.is_some() {
            "NICHT GEFUNDEN".into()
        } else if sw.started_s.is_none() {
            "KEIN AUDIO (noch)".into()
        } else if !sw.fe_current_ok {
            "ANZEIGE FALSCH (Frontend)".into()
        } else if !sw.app_current_ok {
            "ANZEIGE FALSCH (App)".into()
        } else {
            "STATUS FEHLT".into()
        };
        println!(
            "{:<26} start {} sel {} nf {} ens {} lbl {} nosig {} | app {} fe {}{}{}",
            sw.verdict,
            fmt_s(sw.started_s),
            fmt_s(sw.status_selected_s),
            fmt_s(sw.status_not_found_s),
            fmt_s(sw.ensemble_found_s),
            fmt_s(sw.service_added_s),
            sw.no_signal,
            cur.as_ref().map(|c| format!("{:04X}{}", c.sid, if c.codec.is_some() { "" } else { "?" })).unwrap_or("-".into()),
            fe.current.map(|c| format!("{:04X}{}", c.0, if c.2 { "" } else { "?" })).unwrap_or("-".into()),
            if sw.fe_pending_left { " PENDING" } else { "" },
            if sw.notices.is_empty() && sw.warn_logs.is_empty() { String::new() } else { format!(" | {:?} {:?}", sw.notices, sw.warn_logs) },
        );
        sink.line(el(t0), "switch", serde_json::to_value(&sw)?);
        results.push(sw);
        last_slot = Some(slot);
    }

    // Zusammenfassung
    let ok = results.iter().filter(|s| s.verdict == "ok").count();
    let ews = results.iter().filter(|s| s.ews_switched).count();
    let cross: Vec<&Switch> = results.iter().filter(|s| s.cross).collect();
    let same: Vec<&Switch> = results.iter().filter(|s| !s.cross).collect();
    let mean = |v: &[&Switch]| {
        let t: Vec<f64> = v.iter().filter_map(|s| s.started_s).collect();
        if t.is_empty() { f64::NAN } else { t.iter().sum::<f64>() / t.len() as f64 }
    };
    let max = |v: &[&Switch]| v.iter().filter_map(|s| s.started_s).fold(0.0f64, f64::max);
    println!();
    println!("Ergebnis: {ok}/{} ok{}", results.len(), if ews > 0 { format!(" ({ews} durch EWS-Umschaltung des Kerns nicht bewertbar)") } else { String::new() });
    println!("  Kanalwechsel: {} (Audio nach mittel {:.2} s, max {:.2} s, {} ohne Audio im Intervall)",
             cross.len(), mean(&cross), max(&cross), cross.iter().filter(|s| s.started_s.is_none()).count());
    println!("  gleiches Ensemble: {} (Audio nach mittel {:.2} s, max {:.2} s, {} ohne Audio im Intervall)",
             same.len(), mean(&same), max(&same), same.iter().filter(|s| s.started_s.is_none()).count());
    for s in results.iter().filter(|s| s.verdict != "ok") {
        println!("  #{:2} {:4} {:<18} {:<24} Intervall {:.1} s, start {}, sel {}, nf {}, ens {}, lbl {}, no_signal {}, notices {:?}, warn {:?}",
                 s.n, s.channel, s.name, s.verdict, s.interval_s, fmt_s(s.started_s), fmt_s(s.status_selected_s),
                 fmt_s(s.status_not_found_s), fmt_s(s.ensemble_found_s), fmt_s(s.service_added_s), s.no_signal, s.notices, s.warn_logs);
    }
    if let Some(w) = &mut sink.0 {
        w.flush()?;
    }
    let _ = app.save_all();
    Ok(())
}

fn fmt_s(v: Option<f64>) -> String {
    v.map(|s| format!("{s:5.2}")).unwrap_or_else(|| "  -  ".into())
}
