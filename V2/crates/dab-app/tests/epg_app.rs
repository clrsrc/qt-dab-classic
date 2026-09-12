//! Integrationstest: Kern-Ereignisse `mot_object`/`epg_object` laufen durch
//! die App-Schicht und landen im Cache (Dateien + Index), Presets bekommen das
//! Logo, das Display bekommt Logo + Now/Next.
//!
//! `replay_events_jsonl` (ignoriert) spielt ein Ereignisprotokoll von
//! `dab-cli replay ... --events out.jsonl` durch die App und zaehlt, was im
//! Cache ankommt: `DAB_EVENTS_JSONL=out.jsonl cargo test -p dab-app -- --ignored`.

use base64::Engine as _;
use dab_api::{Codec, Event, ServiceInfo, ServiceSlot};
use dab_app::{App, AppEvent, DataDirs, LogoSize, Presets, Settings};
use std::path::PathBuf;
use std::time::Instant;

fn data_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests").join("data").join("10BC")
}

fn tmp(tag: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("dabclassic-epgapp-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn svc(sid: u32, name: &str) -> ServiceInfo {
    ServiceInfo { sid, scids: 0, name: name.into(), is_audio: true, is_primary: true, sub_ch: 1, bitrate_kbps: 96, pty: 0 }
}

#[test]
fn objects_flow_into_cache_presets_and_display() {
    let root = tmp("flow");
    let dirs = DataDirs::with_root(&root, true);
    dirs.ensure().unwrap();
    let mut a = App::with(dirs, Settings::default(), Presets::default());
    let now = Instant::now();
    a.handle_event(&Event::EnsembleFound { eid: 0x10BC, name: "DR Deutschland".into(), channel: "5C".into() }, now);
    a.handle_event(&Event::ServiceAdded { service: svc(0xD210, "Dlf") }, now);
    a.handle_event(&Event::ServiceAdded { service: svc(0xD230, "Dlf Nova") }, now);
    a.state.channel = Some("5C".into());
    a.handle_event(
        &Event::ServiceStarted { slot: ServiceSlot::Primary, sid: 0xD210, scids: 0, codec: Codec::HeAac { sbr: true, ps: false, sample_rate: 48000 }, stereo: true },
        now,
    );
    // Preset vor dem Logo belegen: noch ohne Logo
    let (r, _) = a.preset_store(0, false).unwrap();
    assert!(r.stored);
    assert!(a.presets.get(0).unwrap().logo_data_url.is_none());

    // Logos aus dem Karussell
    let b64 = |name: &str| base64::engine::general_purpose::STANDARD.encode(std::fs::read(data_dir().join(name)).unwrap());
    let mut logo_events = 0;
    for name in ["d210_Dlf_32x32.png", "d210_Dlf_128x128.png", "d210_Dlf_320x240.png"] {
        let fx = a.handle_event(&Event::MotObject { eid: 0x10BC, sid: 0xD210, content_type: 0x0203, name: name.into(), data_b64: b64(name) }, now);
        logo_events += fx.events.iter().filter(|e| matches!(e, AppEvent::LogoUpdated { sid: 0xD210, .. })).count();
        assert!(root.join("logos").join("10BC").join(name).is_file(), "{name} im Cache");
    }
    assert_eq!(logo_events, 3);
    // Preset hat jetzt das kleine Logo, Display das 128er
    let p = a.presets.get(0).unwrap();
    assert!(p.logo_data_url.as_deref().unwrap().starts_with("data:image/png;base64,"));
    assert!(p.logo_path.as_ref().unwrap().ends_with("d210_Dlf_32x32.png"));
    assert_eq!(a.state.logo_data_url, a.logos.data_url(0x10BC, 0xD210, LogoSize::Medium));
    assert!(a.logos.logo_path(0x10BC, 0xD210, LogoSize::Medium).unwrap().ends_with("d210_Dlf_128x128.png"));
    // Wiederholung: nichts Neues
    let fx = a.handle_event(&Event::MotObject { eid: 0x10BC, sid: 0xD210, content_type: 0x0203, name: "d210_Dlf_32x32.png".into(), data_b64: b64("d210_Dlf_32x32.png") }, now);
    assert!(fx.events.is_empty());

    // Logo mit unbekannter SId, dann Service-Information -> zugeordnet
    let fx = a.handle_event(&Event::MotObject { eid: 0x10BC, sid: 0, content_type: 0x0203, name: "nova_logo_32x32.png".into(), data_b64: b64("d220_Dlf_Kult_32x32.png") }, now);
    assert!(fx.events.is_empty());
    let si = std::fs::read_to_string(data_dir().join("list.xml")).unwrap().replace("d230_Dlf_Nova_32x32.png", "nova_logo_32x32.png");
    let fx = a.handle_event(&Event::EpgObject { eid: 0x10BC, sid: 0, date_yyyymmdd: 0, name: "list.xml".into(), xml: si }, now);
    assert!(fx.events.iter().any(|e| matches!(e, AppEvent::LogoUpdated { sid: 0xD230, .. })));
    assert!(a.logos.logo_path(0x10BC, 0xD230, LogoSize::Small).is_some());

    // EPG fuer heute (Zeiten der Testdatei auf heute umgeschrieben) -> Now/Next im Zustand
    let today = chrono::Local::now().date_naive();
    let day = today.format("%Y%m%d").to_string().parse::<u32>().unwrap();
    let xml = std::fs::read_to_string(data_dir().join("20260414_D210_SI.xml")).unwrap();
    let xml = xml.replace("2026-4-14", &today.format("%Y-%-m-%-d").to_string()).replace("2026-4-13", &(today - chrono::Duration::days(1)).format("%Y-%-m-%-d").to_string());
    let xml = xml.replace("<epg system=\"DAB\">", "<epg system=\"DAB\" tz=\"local\">");
    let fx = a.handle_event(&Event::EpgObject { eid: 0x10BC, sid: 0xD210, date_yyyymmdd: day, name: "w.EHB".into(), xml: xml.clone() }, now);
    assert!(fx.events.iter().any(|e| matches!(e, AppEvent::EpgUpdated { sid: 0xD210, .. })));
    assert!(root.join("epg").join("10BC").join(format!("{day}_D210_SI.xml")).is_file());
    assert_eq!(a.epg.programmes(0x10BC, 0xD210, day).len(), 49);
    assert!(!a.epg.programmes(0x10BC, 0xD210, day)[0].legacy_time);
    let nn = a.state.now_next.as_ref().expect("Now/Next fuer heute");
    assert!(nn.now.is_some() || nn.next.is_some());
    assert!(fx.events.iter().any(|e| matches!(e, AppEvent::CurrentMedia { now_next: Some(_), .. })));
    // gleicher Inhalt erneut: kein Ereignis
    let fx = a.handle_event(&Event::EpgObject { eid: 0x10BC, sid: 0xD210, date_yyyymmdd: day, name: "w.EHB".into(), xml }, now);
    assert!(!fx.events.iter().any(|e| matches!(e, AppEvent::EpgUpdated { .. })));
    assert_eq!(a.epg_services_named(0x10BC, day), vec![(0xD210, "Dlf".to_string())]);

    // Dienst weg -> Logo/Now-Next weg (der Spiegel im Frontend leert bei service_stopped ebenso)
    a.handle_event(&Event::ServiceStopped { slot: ServiceSlot::Primary, sid: 0xD210 }, now);
    assert!(a.state.logo_data_url.is_none() && a.state.now_next.is_none());

    // Neustart liest den Cache vom Dateisystem
    let again = App::with(DataDirs::with_root(&root, true), Settings::default(), Presets::default());
    assert_eq!(again.logos.sizes(0x10BC, 0xD210), vec![(32, 32), (128, 128), (320, 240)]);
    assert_eq!(again.epg.days(0x10BC), vec![day]);
    let _ = std::fs::remove_dir_all(&root);
}

/// Ereignisprotokoll eines Replays durch die App spielen (Abnahme, manuell).
#[test]
#[ignore]
fn replay_events_jsonl() {
    let Some(path) = std::env::var_os("DAB_EVENTS_JSONL") else {
        eprintln!("DAB_EVENTS_JSONL nicht gesetzt");
        return;
    };
    let root = std::env::var_os("DAB_EVENTS_OUT").map(PathBuf::from).unwrap_or_else(|| tmp("replay"));
    let dirs = DataDirs::with_root(&root, true);
    dirs.ensure().unwrap();
    let mut a = App::with(dirs, Settings::default(), Presets::default());
    let text = std::fs::read_to_string(&path).unwrap();
    let now = Instant::now();
    let (mut mot, mut epg, mut logo_ev, mut epg_ev) = (0, 0, 0, 0);
    for line in text.lines() {
        // dab-cli schreibt `{"t": .., "event": {..}}`, dabcored die nackten Ereignisse.
        let Ok(mut v) = serde_json::from_str::<serde_json::Value>(line) else { continue };
        if let Some(inner) = v.get_mut("event") {
            v = inner.take();
        }
        let Ok(ev) = serde_json::from_value::<Event>(v) else { continue };
        match &ev {
            Event::MotObject { .. } => mot += 1,
            Event::EpgObject { .. } => epg += 1,
            _ => {}
        }
        let fx = a.handle_event(&ev, now);
        for e in fx.events {
            match e {
                AppEvent::LogoUpdated { .. } => logo_ev += 1,
                AppEvent::EpgUpdated { .. } => epg_ev += 1,
                _ => {}
            }
        }
    }
    let eid = a.state.ensemble.as_ref().map(|e| e.eid).unwrap_or(0x10BC);
    eprintln!(
        "Ereignisse: {mot} mot_object, {epg} epg_object -> {logo_ev} logo_updated, {epg_ev} epg_updated; Cache: {} Logos ({} Dienste), {} EPG-Dateien, Tage {:?}; Datenordner {}",
        a.logos.len(),
        a.logos.services(eid).len(),
        a.epg.len(),
        a.epg.days(eid),
        root.display()
    );
    assert!(mot + epg == 0 || a.logos.len() + a.epg.len() > 0);
}
