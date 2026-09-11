//! DAB Classic – `dab-core`: besitzt den Empfangskern und spricht mit ihm
//! ueber Kanaele. Kommandos rein (`Sender<Command>`), Ereignisse raus
//! (`Receiver<Event>`).
//!
//! Heute gibt es genau einen Backend-Typ, [`IpcBackend`]: er startet
//! `dabcored.exe` als Kindprozess und uebersetzt zwischen den Kanaelen und
//! stdin/stdout des Prozesses. Die Schnittstelle [`CoreBackend`] haelt
//! spaetere Varianten (In-Process-DLL, Rust-Kern) offen, ohne dass sich fuer
//! App oder GUI etwas aendert.

use crossbeam_channel::{bounded, Receiver, Sender, TrySendError};
use dab_api::protocol;
use dab_api::{Command, Event};
use std::io::{BufReader, BufWriter};
use std::path::{Path, PathBuf};
use std::process::{Child, Command as Process, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;

/// Fehler beim Starten oder Betreiben des Kerns.
#[derive(Debug, thiserror::Error)]
pub enum CoreError {
    #[error("Kernprozess nicht gefunden: {0}")]
    NotFound(PathBuf),
    #[error("Kernprozess konnte nicht gestartet werden: {0}")]
    Spawn(#[source] std::io::Error),
    #[error("Kernprozess ist beendet")]
    Exited,
}

/// Gemeinsame Schnittstelle aller Kern-Anbindungen.
pub trait CoreBackend: Send {
    /// Kanal, ueber den die App Kommandos an den Kern schickt.
    fn commands(&self) -> Sender<Command>;
    /// Kanal, aus dem die App Ereignisse des Kerns liest.
    fn events(&self) -> Receiver<Event>;
    /// `true`, solange der Kern laeuft.
    fn is_alive(&self) -> bool;
    /// Faehrt den Kern geordnet herunter (blockiert kurz).
    fn shutdown(&mut self);
}

/// Konfiguration fuer [`IpcBackend`].
#[derive(Clone, Debug)]
pub struct IpcConfig {
    /// Pfad zu `dabcored.exe`.
    pub executable: PathBuf,
    /// Zusaetzliche Kommandozeilenargumente (z. B. `--no-audio`).
    pub args: Vec<String>,
    /// Kapazitaet des Ereigniskanals; lueckenlose Ereignisse blockieren den
    /// Lesethread kurz, latest-wins-Ereignisse werden bei vollem Kanal verworfen.
    pub event_capacity: usize,
    /// Kapazitaet des Kommandokanals.
    pub command_capacity: usize,
    /// stderr des Kerns an den eigenen stderr durchreichen (Diagnose).
    pub inherit_stderr: bool,
}

impl IpcConfig {
    pub fn new(executable: impl Into<PathBuf>) -> Self {
        Self {
            executable: executable.into(),
            args: Vec::new(),
            event_capacity: 4096,
            command_capacity: 256,
            inherit_stderr: true,
        }
    }
}

/// Sucht `dabcored.exe` an den ueblichen Orten: explizit uebergebener Pfad,
/// Umgebungsvariable `DABCORE`, neben der eigenen EXE (`core/dabcored.exe`),
/// und im Entwicklungsbaum (`core-cpp/build/dabcored.exe`).
pub fn locate_core(explicit: Option<&Path>) -> Option<PathBuf> {
    let mut candidates: Vec<PathBuf> = Vec::new();
    if let Some(p) = explicit {
        candidates.push(p.to_path_buf());
    }
    if let Ok(env) = std::env::var("DABCORE") {
        candidates.push(PathBuf::from(env));
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            candidates.push(dir.join("core").join("dabcored.exe"));
            candidates.push(dir.join("dabcored.exe"));
            // target/{debug,release}/ -> Workspace-Wurzel. Bevorzugt den
            // Ressourcenordner, dort liegen die Laufzeit-DLLs neben der EXE.
            for up in [2usize, 3] {
                let mut root = dir.to_path_buf();
                for _ in 0..up {
                    root.pop();
                }
                candidates.push(
                    root.join("apps").join("desktop").join("src-tauri").join("resources").join("core").join("dabcored.exe"),
                );
                candidates.push(root.join("core-cpp").join("build").join("dabcored.exe"));
            }
        }
    }
    candidates.into_iter().find(|p| p.is_file())
}

/// Kern als Kindprozess, angebunden ueber stdin/stdout.
pub struct IpcBackend {
    cmd_tx: Sender<Command>,
    ev_rx: Receiver<Event>,
    child: Arc<Mutex<Child>>,
    alive: Arc<AtomicBool>,
    reader: Option<JoinHandle<()>>,
    writer: Option<JoinHandle<()>>,
}

impl IpcBackend {
    pub fn spawn(config: IpcConfig) -> Result<Self, CoreError> {
        if !config.executable.is_file() {
            return Err(CoreError::NotFound(config.executable));
        }
        let mut proc = Process::new(&config.executable);
        proc.args(&config.args)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(if config.inherit_stderr { Stdio::inherit() } else { Stdio::null() });
        if let Some(dir) = config.executable.parent() {
            // DLLs des Kerns liegen neben der EXE.
            proc.current_dir(dir);
        }
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            const CREATE_NO_WINDOW: u32 = windows_sys::Win32::System::Threading::CREATE_NO_WINDOW;
            proc.creation_flags(CREATE_NO_WINDOW);
        }
        let mut child = proc.spawn().map_err(CoreError::Spawn)?;
        let stdin = child.stdin.take().expect("stdin piped");
        let stdout = child.stdout.take().expect("stdout piped");

        let (cmd_tx, cmd_rx) = bounded::<Command>(config.command_capacity);
        let (ev_tx, ev_rx) = bounded::<Event>(config.event_capacity);
        let alive = Arc::new(AtomicBool::new(true));

        // Lesethread: stdout -> Ereigniskanal
        let reader = {
            let alive = alive.clone();
            std::thread::Builder::new()
                .name("dabcore-events".into())
                .spawn(move || {
                    let mut r = BufReader::with_capacity(1 << 16, stdout);
                    loop {
                        match protocol::read_event(&mut r) {
                            Ok(Some(Ok(ev))) => {
                                if ev.is_latest_wins() {
                                    match ev_tx.try_send(ev) {
                                        Ok(()) | Err(TrySendError::Full(_)) => {}
                                        Err(TrySendError::Disconnected(_)) => break,
                                    }
                                } else if ev_tx.send(ev).is_err() {
                                    break;
                                }
                            }
                            Ok(Some(Err(msg))) => log::warn!("Kern: unlesbares Ereignis: {msg}"),
                            Ok(None) => break,
                            Err(e) => {
                                log::warn!("Kern: Lesefehler: {e}");
                                break;
                            }
                        }
                    }
                    alive.store(false, Ordering::SeqCst);
                    let _ = ev_tx.send(Event::Exiting { reason: "stdout geschlossen".into() });
                })
                .expect("Thread")
        };

        // Schreibthread: Kommandokanal -> stdin
        let writer = std::thread::Builder::new()
            .name("dabcore-commands".into())
            .spawn(move || {
                let mut w = BufWriter::new(stdin);
                while let Ok(cmd) = cmd_rx.recv() {
                    let stop = matches!(cmd, Command::Shutdown);
                    if protocol::write_command(&mut w, &cmd).is_err() {
                        break;
                    }
                    if stop {
                        break;
                    }
                }
                // stdin schliessen: der Kern beendet sich bei EOF.
            })
            .expect("Thread");

        Ok(Self {
            cmd_tx,
            ev_rx,
            child: Arc::new(Mutex::new(child)),
            alive,
            reader: Some(reader),
            writer: Some(writer),
        })
    }

    /// Prozess-ID des Kerns (Diagnose).
    pub fn pid(&self) -> u32 {
        self.child.lock().map(|c| c.id()).unwrap_or(0)
    }
}

impl CoreBackend for IpcBackend {
    fn commands(&self) -> Sender<Command> {
        self.cmd_tx.clone()
    }

    fn events(&self) -> Receiver<Event> {
        self.ev_rx.clone()
    }

    fn is_alive(&self) -> bool {
        if !self.alive.load(Ordering::SeqCst) {
            return false;
        }
        match self.child.lock() {
            Ok(mut c) => matches!(c.try_wait(), Ok(None)),
            Err(_) => false,
        }
    }

    fn shutdown(&mut self) {
        let _ = self.cmd_tx.try_send(Command::Shutdown);
        if let Some(w) = self.writer.take() {
            let _ = w.join();
        }
        // Dem Kern kurz Zeit geben, sich selbst zu beenden.
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(3);
        loop {
            if let Ok(mut c) = self.child.lock() {
                if let Ok(Some(_)) = c.try_wait() {
                    break;
                }
                if std::time::Instant::now() > deadline {
                    let _ = c.kill();
                    let _ = c.wait();
                    break;
                }
            }
            std::thread::sleep(std::time::Duration::from_millis(20));
        }
        self.alive.store(false, Ordering::SeqCst);
        if let Some(r) = self.reader.take() {
            let _ = r.join();
        }
    }
}

impl Drop for IpcBackend {
    fn drop(&mut self) {
        if self.alive.load(Ordering::SeqCst) {
            self.shutdown();
        }
    }
}
