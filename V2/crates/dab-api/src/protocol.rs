//! Zeilenbasiertes Framing: eine JSON-Zeile je Kommando bzw. Ereignis,
//! abgeschlossen mit `\n`. Keine Laengenpraefixe, damit das Protokoll mit
//! jedem Werkzeug (Editor, `type`, Python) lesbar bleibt.

use crate::{Command, Event};
use std::io::{self, BufRead, Write};

/// Serialisiert ein Kommando als eine Zeile (mit `\n`).
pub fn encode_command(cmd: &Command) -> String {
    let mut s = serde_json::to_string(cmd).expect("Command ist immer serialisierbar");
    s.push('\n');
    s
}

/// Serialisiert ein Ereignis als eine Zeile (mit `\n`).
pub fn encode_event(ev: &Event) -> String {
    let mut s = serde_json::to_string(ev).expect("Event ist immer serialisierbar");
    s.push('\n');
    s
}

pub fn decode_command(line: &str) -> serde_json::Result<Command> {
    serde_json::from_str(line.trim())
}

pub fn decode_event(line: &str) -> serde_json::Result<Event> {
    serde_json::from_str(line.trim())
}

/// Schreibt ein Kommando in einen Stream und flusht (Pipes puffern sonst).
pub fn write_command<W: Write>(w: &mut W, cmd: &Command) -> io::Result<()> {
    w.write_all(encode_command(cmd).as_bytes())?;
    w.flush()
}

pub fn write_event<W: Write>(w: &mut W, ev: &Event) -> io::Result<()> {
    w.write_all(encode_event(ev).as_bytes())?;
    w.flush()
}

/// Liest die naechste nicht-leere Zeile und dekodiert sie als Ereignis.
/// `Ok(None)` bei EOF. Unbekannte Zeilen werden als Fehler gemeldet, der
/// Aufrufer entscheidet, ob er sie ueberspringt.
pub fn read_event<R: BufRead>(r: &mut R) -> io::Result<Option<Result<Event, String>>> {
    let mut line = String::new();
    loop {
        line.clear();
        let n = r.read_line(&mut line)?;
        if n == 0 {
            return Ok(None);
        }
        let t = line.trim();
        if t.is_empty() {
            continue;
        }
        return Ok(Some(decode_event(t).map_err(|e| format!("{e}: {t}"))));
    }
}

pub fn read_command<R: BufRead>(r: &mut R) -> io::Result<Option<Result<Command, String>>> {
    let mut line = String::new();
    loop {
        line.clear();
        let n = r.read_line(&mut line)?;
        if n == 0 {
            return Ok(None);
        }
        let t = line.trim();
        if t.is_empty() {
            continue;
        }
        return Ok(Some(decode_command(t).map_err(|e| format!("{e}: {t}"))));
    }
}
