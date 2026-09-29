//! Infobereich (System-Tray), Vorbild Crossmixer `Source/MainWindow.h`
//! (SystemTrayIconComponent, minimizeToTray/restoreFromTray): der Knopf in
//! der Titelleiste versteckt das Hauptfenster (Frontend `win.hide()`), ein
//! Linksklick auf das Tray-Symbol stellt es wieder her, das Menue bietet
//! Anzeigen und Beenden. Beenden schliesst das Hauptfenster auf dem normalen
//! Weg (`on_window_event` in lib.rs: Kern stoppen, Zustand sichern).
//! Waehrend das Fenster versteckt ist, bestellt das Frontend die Anzeige-
//! Ereignisse ab (document.visibilitychange -> set_audio_spectrum false).

use crate::{lock_app, Shared};
use tauri::menu::{Menu, MenuItem};
use tauri::tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent};
use tauri::{AppHandle, Manager};

pub fn install(handle: &AppHandle) -> tauri::Result<()> {
    let de = lang_is_de(handle);
    let show = MenuItem::with_id(handle, "show", if de { "Anzeigen" } else { "Show" }, true, None::<&str>)?;
    let quit = MenuItem::with_id(handle, "quit", if de { "Beenden" } else { "Quit" }, true, None::<&str>)?;
    let menu = Menu::with_items(handle, &[&show, &quit])?;
    let mut builder = TrayIconBuilder::with_id("main")
        .menu(&menu)
        .show_menu_on_left_click(false)
        .tooltip("DAB Classic")
        .on_menu_event(|app, ev| match ev.id().0.as_str() {
            "show" => restore(app),
            "quit" => quit_app(app),
            _ => {}
        })
        .on_tray_icon_event(|tray, ev| {
            if let TrayIconEvent::Click { button: MouseButton::Left, button_state: MouseButtonState::Up, .. } = ev {
                restore(tray.app_handle());
            }
        });
    if let Some(icon) = handle.default_window_icon() {
        builder = builder.icon(icon.clone());
    }
    builder.build(handle)?;
    Ok(())
}

/// Fenster wieder anzeigen und nach vorn holen (Crossmixer restoreFromTray).
pub fn restore(app: &AppHandle) {
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.show();
        let _ = w.unminimize();
        let _ = w.set_focus();
    }
}

fn quit_app(app: &AppHandle) {
    match app.get_webview_window("main") {
        Some(w) => {
            let _ = w.close();
        }
        None => app.exit(0),
    }
}

/// Menuesprache aus den Einstellungen (Entscheidung 27: "de"/"en", None =
/// System); ohne Angabe Deutsch.
fn lang_is_de(app: &AppHandle) -> bool {
    lock_app(&app.state::<Shared>())
        .ok()
        .and_then(|a| a.settings.language.clone())
        .map(|l| !l.to_ascii_lowercase().starts_with("en"))
        .unwrap_or(true)
}
