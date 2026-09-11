//! Einmaliger Import der Qt-DAB-Favoriten (`.qt-dab-presets.xml`,
//! Entscheidung 8). Die Datei wird nur gelesen. Format:
//! `<preset_db><PRESET_ELEMENT CHANNEL="5C" SERVICE_NAME="Dlf   "/></preset_db>`
//! (aeltere Versionen: `<preset SERVICE_NAME="…" CHANNEL="…"/>`). Namen sind
//! auf 16 Zeichen aufgefuellt. Kein XML-Parser noetig: nur Attribute lesen.

use std::path::PathBuf;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Favorite {
    pub channel: String,
    pub name: String,
}

/// Bekannte Ablageorte: Benutzerprofil, portable Qt-DAB-Installation.
pub fn locate() -> Option<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(home) = std::env::var_os("USERPROFILE").or_else(|| std::env::var_os("HOME")) {
        candidates.push(PathBuf::from(&home).join(".qt-dab-presets.xml"));
    }
    if let Ok(exe) = std::env::current_exe() {
        let mut dir = exe.parent().map(|p| p.to_path_buf());
        for _ in 0..5 {
            let Some(d) = dir else { break };
            candidates.push(d.join("Qt-DAB-portable").join("data").join(".qt-dab-presets.xml"));
            dir = d.parent().map(|p| p.to_path_buf());
        }
    }
    candidates.into_iter().find(|p| p.is_file())
}

/// Liest alle Favoriten aus dem XML-Text (Reihenfolge der Datei).
pub fn parse(xml: &str) -> Vec<Favorite> {
    let mut out = Vec::new();
    let mut rest = xml;
    while let Some(start) = rest.find('<') {
        rest = &rest[start + 1..];
        let Some(end) = rest.find('>') else { break };
        let tag = &rest[..end];
        rest = &rest[end + 1..];
        let lower = tag.to_ascii_lowercase();
        if !(lower.starts_with("preset_element") || lower.starts_with("preset ")) {
            continue;
        }
        let channel = attr(tag, "CHANNEL");
        let name = attr(tag, "SERVICE_NAME");
        if let (Some(channel), Some(name)) = (channel, name) {
            let channel = channel.trim().to_uppercase();
            let name = unescape(name.trim());
            if !channel.is_empty() && !name.is_empty() {
                out.push(Favorite { channel, name });
            }
        }
    }
    out
}

fn attr<'a>(tag: &'a str, key: &str) -> Option<&'a str> {
    let lower = tag.to_ascii_lowercase();
    let k = format!("{}=", key.to_ascii_lowercase());
    let mut from = 0;
    while let Some(pos) = lower[from..].find(&k) {
        let at = from + pos;
        // Attributname muss am Wortanfang stehen
        if at > 0 && !lower.as_bytes()[at - 1].is_ascii_whitespace() {
            from = at + k.len();
            continue;
        }
        let after = &tag[at + k.len()..];
        let quote = after.chars().next()?;
        if quote != '"' && quote != '\'' {
            return None;
        }
        let body = &after[1..];
        let close = body.find(quote)?;
        return Some(&body[..close]);
    }
    None
}

fn unescape(s: &str) -> String {
    s.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"").replace("&apos;", "'")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_both_element_forms() {
        let xml = r#"<?xml version="1.0"?>
<preset_db>
 <PRESET_ELEMENT CHANNEL="5C" SERVICE_NAME="Dlf             "/>
 <PRESET_ELEMENT CHANNEL="11D" SERVICE_NAME="WDR 2 RHEIN-RUHR"/>
 <preset SERVICE_NAME="Rock &amp; Pop" CHANNEL="9b"/>
 <other CHANNEL="1A" SERVICE_NAME="nein"/>
</preset_db>"#;
        let f = parse(xml);
        assert_eq!(f.len(), 3);
        assert_eq!(f[0], Favorite { channel: "5C".into(), name: "Dlf".into() });
        assert_eq!(f[1].name, "WDR 2 RHEIN-RUHR");
        assert_eq!(f[2], Favorite { channel: "9B".into(), name: "Rock & Pop".into() });
    }
}
