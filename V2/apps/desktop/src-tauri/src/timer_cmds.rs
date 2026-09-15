//! Tauri-Kommandos fuer Timer, Aufnahme, Sleep-Timer und das Alarmfenster
//! (Entscheidungen 5, 9/10, 18). Logik liegt in `dab-app` (`timer.rs`,
//! `recording.rs`, `sleep.rs`); hier nur die Bruecke und das zweite Fenster.

use super::{act, lock_app, Shared, R};
use dab_api::{Event, EwsPhase};
use dab_app::{AddOutcome, EpgTimerRequest, RecordingInfo, SleepAction, SleepState, Timer, Timers};
use tauri::{AppHandle, Manager, State};

/// Label des Alarmfensters (Capability `alarm.json`, Route `/alarm`).
pub const ALARM_WINDOW: &str = "alarm";

fn now() -> i64 {
    dab_app::state::unix_now()
}

// ---------------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------------

#[tauri::command]
pub fn timers_list(shared: State<'_, Shared>) -> R<Timers> {
    Ok(lock_app(&shared)?.sched.timers.clone())
}

#[tauri::command]
pub fn timer_add(handle: AppHandle, shared: State<'_, Shared>, timer: Timer, force: Option<bool>) -> R<AddOutcome> {
    act(&handle, &shared, |a| a.timer_add(timer, force.unwrap_or(false), now()))
}

#[tauri::command]
pub fn timer_update(handle: AppHandle, shared: State<'_, Shared>, timer: Timer, force: Option<bool>) -> R<AddOutcome> {
    act(&handle, &shared, |a| a.timer_update(timer, force.unwrap_or(false), now()))
}

#[tauri::command]
pub fn timer_delete(handle: AppHandle, shared: State<'_, Shared>, id: u32) -> R<()> {
    act(&handle, &shared, |a| a.timer_delete(id).map(|fx| ((), fx)))
}

#[tauri::command]
pub fn timer_toggle_active(handle: AppHandle, shared: State<'_, Shared>, id: u32) -> R<()> {
    act(&handle, &shared, |a| a.timer_toggle_active(id).map(|fx| ((), fx)))
}

/// Vertrag mit dem EPG-Panel: `req = {channel, eid, sid, service, title,
/// start_unix, duration_s, kind: "record"|"switch"}` -> Timer-Id oder
/// i18n-Schluessel `timer.conflict.*`.
#[tauri::command]
pub fn timer_add_from_epg(handle: AppHandle, shared: State<'_, Shared>, req: EpgTimerRequest) -> R<u32> {
    act(&handle, &shared, |a| a.timer_add_from_epg(req, now()))
}

// ---------------------------------------------------------------------------
// Aufnahme
// ---------------------------------------------------------------------------

#[tauri::command]
pub fn recording_start(handle: AppHandle, shared: State<'_, Shared>) -> R<()> {
    act(&handle, &shared, |a| a.recording_start(None, None).map(|fx| ((), fx)))
}

#[tauri::command]
pub fn recording_stop(handle: AppHandle, shared: State<'_, Shared>) -> R<()> {
    act(&handle, &shared, |a| a.recording_stop().map(|fx| ((), fx)))
}

#[tauri::command]
pub fn recording_toggle(handle: AppHandle, shared: State<'_, Shared>) -> R<()> {
    act(&handle, &shared, |a| a.recording_toggle().map(|fx| ((), fx)))
}

#[tauri::command]
pub fn recording_status(shared: State<'_, Shared>) -> R<RecordingInfo> {
    Ok(lock_app(&shared)?.recording_info().clone())
}

// ---------------------------------------------------------------------------
// Sleep-Timer
// ---------------------------------------------------------------------------

#[tauri::command]
pub fn sleep_set(handle: AppHandle, shared: State<'_, Shared>, minutes: u32, action: SleepAction) -> R<()> {
    act(&handle, &shared, |a| a.sleep_set(minutes, action, now()).map(|fx| ((), fx)))
}

#[tauri::command]
pub fn sleep_cancel(handle: AppHandle, shared: State<'_, Shared>) -> R<()> {
    act(&handle, &shared, |a| Ok(((), a.sleep_cancel())))
}

#[tauri::command]
pub fn sleep_status(shared: State<'_, Shared>) -> R<Option<SleepState>> {
    Ok(lock_app(&shared)?.sleep_state().cloned())
}

// ---------------------------------------------------------------------------
// Alarmfenster (Entscheidung 5 + 10)
// ---------------------------------------------------------------------------

/// Vom Alarmfenster nach "Quittieren" (der Kern bekommt `ews_dismiss` ueber `core_send`).
#[tauri::command]
pub fn alarm_close(handle: AppHandle) -> R<()> {
    close_alarm(&handle);
    Ok(())
}

pub fn close_alarm(handle: &AppHandle) {
    if let Some(w) = handle.get_webview_window(ALARM_WINDOW) {
        if let Err(e) = w.close() {
            log::warn!("Alarmfenster schliessen: {e}");
        }
    }
}

fn open_alarm(handle: &AppHandle) {
    if let Some(w) = handle.get_webview_window(ALARM_WINDOW) {
        let _ = w.show();
        let _ = w.set_focus();
        return;
    }
    let h = handle.clone();
    let r = handle.run_on_main_thread(move || {
        let builder = tauri::WebviewWindowBuilder::new(&h, ALARM_WINDOW, tauri::WebviewUrl::App("alarm".into()))
            .title("DAB Classic – Alarm")
            .inner_size(420.0, 260.0)
            .min_inner_size(360.0, 220.0)
            .resizable(false)
            .decorations(false)
            .always_on_top(true)
            .center()
            .focused(true)
            .skip_taskbar(false);
        // Gleiche WebView2-Argumente wie das Hauptfenster (eine Umgebung je Prozess),
        // damit der Warnton ohne Nutzergeste spielt.
        #[cfg(windows)]
        let builder = builder.additional_browser_args(
            "--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection --autoplay-policy=no-user-gesture-required",
        );
        match builder.build() {
            Ok(w) => {
                let _ = w.set_focus();
            }
            Err(e) => log::error!("Alarmfenster: {e}"),
        }
    });
    if let Err(e) = r {
        log::error!("Alarmfenster (main thread): {e}");
    }
}

/// Hook aus dem Brueckenthread: Alarmfenster bei `trigger`/`sustain` oeffnen
/// (sofern nicht quittiert), bei `end` schliessen. `pre_trigger` bleibt ein
/// Statusleisten-Hinweis im Hauptfenster.
pub fn on_core_event(handle: &AppHandle, shared: &Shared, ev: &Event) {
    match ev {
        Event::EwsAlert { phase: EwsPhase::Trigger | EwsPhase::Sustain, relevant, .. } => {
            // Geofencing (ETSI TS 104 089 Klausel 7.5/7.6): `relevant == Some(false)`
            // heisst, dass kein Ortscode des Alarms den eigenen Standort abdeckt -
            // z. B. der Eiffelturm-Testalarm des Bundesmux, von Deutschland aus
            // gesehen. Der Alarm ist echt und wird von dab_app normal in die
            // EWF-Historie geschrieben, er soll nur nicht aufpoppen und keinen
            // Warnton machen. `None` = der Kern kennt keine Heimatkoordinaten,
            // dann bleibt es beim ungefilterten Verhalten.
            let dismissed = lock_app(shared).map(|a| a.state.alert.as_ref().map(|x| x.dismissed).unwrap_or(false)).unwrap_or(false);
            if !dismissed && *relevant != Some(false) {
                open_alarm(handle);
            }
        }
        Event::EwsAlert { phase: EwsPhase::End, .. } | Event::Exiting { .. } => close_alarm(handle),
        _ => {}
    }
}
