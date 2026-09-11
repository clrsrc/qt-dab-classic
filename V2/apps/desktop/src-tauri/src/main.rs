// Verhindert ein zusaetzliches Konsolenfenster unter Windows im Release.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    // Portabel (Entscheidung 12): liegt neben der EXE ein Ordner "webview2\"
    // mit der Fixed-Version-Runtime, wird er statt der installierten
    // Evergreen-Runtime benutzt. wry/WebView2Loader werten die Variable aus.
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            let wv = dir.join("webview2");
            if wv.join("msedgewebview2.exe").is_file() {
                std::env::set_var("WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", &wv);
            }
        }
    }
    dab_classic_lib::run()
}
