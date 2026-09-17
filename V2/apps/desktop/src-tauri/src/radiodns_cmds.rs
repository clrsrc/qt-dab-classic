//! Hybrid Radio / RadioDNS (dab_app::radiodns): Worker-Thread und Kommandos.
//! Die App-Schicht plant Auftraege (`radiodns_take_job`) und uebernimmt
//! Ergebnisse (`radiodns_apply`); hier laeuft nur der Netzzugriff ausserhalb
//! des App-Locks. Ein Auftrag zur Zeit; der Thread fragt alle zwei Sekunden
//! nach und endet mit `shutting_down`.

use crate::{lock_app, run_effects, Shared};
use dab_app::{radiodns, NetFetcher, RadioDnsStatus};
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};
use tauri::{AppHandle, Manager, State};

type R<T> = Result<T, String>;

const POLL: Duration = Duration::from_secs(2);

pub fn spawn_worker(handle: &AppHandle) {
    let app = handle.clone();
    let r = std::thread::Builder::new().name("radiodns".into()).spawn(move || {
        let shared = app.state::<Shared>();
        let mut fetcher: Option<NetFetcher> = None;
        loop {
            if shared.shutting_down.load(Ordering::SeqCst) {
                return;
            }
            std::thread::sleep(POLL);
            let job = match lock_app(&shared) {
                Ok(mut a) => a.radiodns_take_job(Instant::now()),
                Err(_) => None,
            };
            let Some(job) = job else { continue };
            if fetcher.is_none() {
                match NetFetcher::new() {
                    Ok(f) => fetcher = Some(f),
                    Err(e) => {
                        log::warn!("radiodns: {e}");
                        let res = radiodns::JobResult { eid: job.eid, ecc: job.ecc, errors: vec![e], ..Default::default() };
                        let fx = lock_app(&shared).map(|mut a| a.radiodns_apply(res, Instant::now())).unwrap_or_default();
                        run_effects(&app, &shared, fx);
                        continue;
                    }
                }
            }
            log::info!("radiodns: Auftrag {:04X} mit {} Diensten", job.eid, job.services.len());
            let res = radiodns::run_job(fetcher.as_ref().expect("gesetzt"), &job, dab_app::state::unix_now());
            if shared.shutting_down.load(Ordering::SeqCst) {
                return;
            }
            let fx = lock_app(&shared).map(|mut a| a.radiodns_apply(res, Instant::now())).unwrap_or_default();
            run_effects(&app, &shared, fx);
        }
    });
    if let Err(e) = r {
        log::error!("RadioDNS-Thread: {e}");
    }
}

/// Einstellungen "Jetzt abrufen": Merker verwerfen, naechster Lauf sofort.
#[tauri::command]
pub fn radiodns_refresh(handle: AppHandle, shared: State<'_, Shared>) -> R<()> {
    let fx = lock_app(&shared)?.radiodns_refresh(Instant::now());
    run_effects(&handle, &shared, fx);
    Ok(())
}

#[tauri::command]
pub fn radiodns_status(shared: State<'_, Shared>) -> R<RadioDnsStatus> {
    Ok(lock_app(&shared)?.state.radiodns.clone())
}
