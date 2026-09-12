//! EPG-Cache und -Parser (Entscheidung 14): Der Kern liefert je Dienst und
//! Tag ein fertiges XML im v1-Format (`epg_object`); die App legt es unter
//! `data/epg/<EID>/<yyyymmdd>_<SID>_SI.xml` ab (nur bei Aenderung), parst es
//! tolerant und haelt einen Index je (EId, SId, Tag). Daraus kommen die
//! Tages-/Dienst-/Programmlisten fuer das EPG-Panel und Now/Next fuer das
//! Display. Svelte stellt nur dar.
//!
//! Zeiten: Neue Dateien tragen am Wurzelelement `tz="local"` und enthalten
//! Ortszeit. Dateien ohne `tz` stammen aus dem alten Kern (Zeit = UTC + 2 min);
//! sie werden unveraendert angezeigt und mit `legacy_time: true` markiert.
//!
//! Der XML-Leser ist bewusst ein kleiner eigener Baumparser: Die Dateien
//! enthalten Steuerzeichen in Attributen, `width="257"`-Fehler und aehnliche
//! Eigenheiten; ein fehlerhaftes `<programme>` wird uebersprungen, die Datei
//! als Ganzes bleibt nutzbar.

use crate::app::{AppEvent, Effects};
use crate::{App, DataDirs};
use chrono::{Datelike, Local, NaiveDate, NaiveDateTime, NaiveTime, TimeZone};
use dab_api::{Event, ServiceSlot};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

// ---------------------------------------------------------------------------
// Kleiner toleranter XML-Baumparser
// ---------------------------------------------------------------------------

pub mod xml {
    /// Element mit Attributen, Kindern und (zusammengefasstem) Text.
    #[derive(Debug, Default, Clone, PartialEq)]
    pub struct Elem {
        pub name: String,
        pub attrs: Vec<(String, String)>,
        pub children: Vec<Elem>,
        pub text: String,
    }

    impl Elem {
        pub fn attr(&self, key: &str) -> Option<&str> {
            self.attrs.iter().find(|(k, _)| k == key).map(|(_, v)| v.as_str())
        }
        pub fn child(&self, name: &str) -> Option<&Elem> {
            self.children.iter().find(|c| c.name == name)
        }
        pub fn children<'a>(&'a self, name: &'a str) -> impl Iterator<Item = &'a Elem> + 'a {
            self.children.iter().filter(move |c| c.name == name)
        }
        pub fn text(&self) -> &str {
            self.text.trim()
        }
    }

    /// Entfernt Steuerzeichen (< 0x20 ausser Tab/LF/CR), die in `genre href`
    /// und anderen Attributen vorkommen.
    pub fn strip_control(src: &str) -> String {
        src.chars().filter(|c| !((*c as u32) < 0x20 && !matches!(c, '\t' | '\n' | '\r'))).collect()
    }

    pub fn unescape(s: &str) -> String {
        if !s.contains('&') {
            return s.to_string();
        }
        let mut out = String::with_capacity(s.len());
        let mut rest = s;
        while let Some(i) = rest.find('&') {
            out.push_str(&rest[..i]);
            rest = &rest[i..];
            let Some(end) = rest.find(';').filter(|e| *e <= 10) else {
                out.push('&');
                rest = &rest[1..];
                continue;
            };
            let ent = &rest[1..end];
            let rep = match ent {
                "amp" => Some('&'),
                "lt" => Some('<'),
                "gt" => Some('>'),
                "quot" => Some('"'),
                "apos" => Some('\''),
                _ if ent.starts_with("#x") || ent.starts_with("#X") => u32::from_str_radix(&ent[2..], 16).ok().and_then(char::from_u32),
                _ if ent.starts_with('#') => ent[1..].parse::<u32>().ok().and_then(char::from_u32),
                _ => None,
            };
            match rep {
                Some(c) => {
                    out.push(c);
                    rest = &rest[end + 1..];
                }
                None => {
                    out.push('&');
                    rest = &rest[1..];
                }
            }
        }
        out.push_str(rest);
        out
    }

    fn parse_attrs(s: &str) -> Vec<(String, String)> {
        let mut out = Vec::new();
        let b = s.as_bytes();
        let mut i = 0;
        while i < b.len() {
            while i < b.len() && (b[i] as char).is_whitespace() {
                i += 1;
            }
            let ks = i;
            while i < b.len() && b[i] != b'=' && !(b[i] as char).is_whitespace() {
                i += 1;
            }
            let key = &s[ks..i];
            while i < b.len() && (b[i] as char).is_whitespace() {
                i += 1;
            }
            if i >= b.len() || b[i] != b'=' {
                if !key.is_empty() {
                    out.push((key.to_string(), String::new()));
                }
                continue;
            }
            i += 1;
            while i < b.len() && (b[i] as char).is_whitespace() {
                i += 1;
            }
            if i >= b.len() {
                break;
            }
            let q = b[i];
            let val = if q == b'"' || q == b'\'' {
                i += 1;
                let vs = i;
                while i < b.len() && b[i] != q {
                    i += 1;
                }
                let v = &s[vs..i.min(b.len())];
                i += 1;
                v
            } else {
                let vs = i;
                while i < b.len() && !(b[i] as char).is_whitespace() {
                    i += 1;
                }
                &s[vs..i]
            };
            if !key.is_empty() {
                out.push((key.to_string(), unescape(val)));
            }
        }
        out
    }

    /// Ende eines Start-Tags finden (`>` ausserhalb von Anfuehrungszeichen).
    fn tag_end(s: &str) -> Option<usize> {
        let mut quote: Option<u8> = None;
        for (i, &c) in s.as_bytes().iter().enumerate() {
            match (quote, c) {
                (Some(q), _) if c == q => quote = None,
                (Some(_), _) => {}
                (None, b'"') | (None, b'\'') => quote = Some(c),
                (None, b'>') => return Some(i),
                _ => {}
            }
        }
        None
    }

    /// Parst ein Dokument zu einer Liste von Wurzelelementen. Nie ein Fehler:
    /// Unpassende End-Tags werden ignoriert, ein abgeschnittenes Dokument
    /// liefert, was bis dahin vollstaendig war.
    pub fn parse(src: &str) -> Vec<Elem> {
        let src = strip_control(src);
        let mut stack: Vec<Elem> = vec![Elem { name: String::new(), ..Default::default() }];
        let mut pos = 0;
        while let Some(lt) = src[pos..].find('<') {
            let text = &src[pos..pos + lt];
            if !text.trim().is_empty() {
                stack.last_mut().unwrap().text.push_str(&unescape(text));
            }
            let rest = &src[pos + lt..];
            if let Some(r) = rest.strip_prefix("<?") {
                let Some(e) = r.find("?>") else { break };
                pos += lt + 2 + e + 2;
            } else if let Some(r) = rest.strip_prefix("<!--") {
                let Some(e) = r.find("-->") else { break };
                pos += lt + 4 + e + 3;
            } else if let Some(r) = rest.strip_prefix("<![CDATA[") {
                let Some(e) = r.find("]]>") else { break };
                stack.last_mut().unwrap().text.push_str(&r[..e]);
                pos += lt + 9 + e + 3;
            } else if let Some(r) = rest.strip_prefix("<!") {
                let Some(e) = r.find('>') else { break };
                pos += lt + 2 + e + 1;
            } else if let Some(r) = rest.strip_prefix("</") {
                let Some(e) = r.find('>') else { break };
                let name = r[..e].trim();
                if let Some(idx) = stack.iter().rposition(|el| el.name == name) {
                    if idx > 0 {
                        while stack.len() > idx + 1 {
                            let done = stack.pop().unwrap();
                            stack.last_mut().unwrap().children.push(done);
                        }
                        let done = stack.pop().unwrap();
                        stack.last_mut().unwrap().children.push(done);
                    }
                }
                pos += lt + 2 + e + 1;
            } else {
                let Some(e) = tag_end(&rest[1..]) else { break };
                let inner = &rest[1..1 + e];
                let (inner, empty) = match inner.strip_suffix('/') {
                    Some(i) => (i, true),
                    None => (inner, false),
                };
                let name_end = inner.find(|c: char| c.is_whitespace()).unwrap_or(inner.len());
                let name = inner[..name_end].to_string();
                let el = Elem { name, attrs: parse_attrs(&inner[name_end..]), children: Vec::new(), text: String::new() };
                if empty || el.name.is_empty() {
                    if !el.name.is_empty() {
                        stack.last_mut().unwrap().children.push(el);
                    }
                } else {
                    stack.push(el);
                }
                pos += lt + 1 + e + 1;
            }
        }
        // Abgeschnittenes Dokument: offene Elemente schliessen.
        while stack.len() > 1 {
            let done = stack.pop().unwrap();
            stack.last_mut().unwrap().children.push(done);
        }
        stack.pop().unwrap().children
    }
}

// ---------------------------------------------------------------------------
// Datenmodell
// ---------------------------------------------------------------------------

/// Eine Sendung.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Programme {
    pub sid: u32,
    /// Beginn in Ortszeit (bzw. unveraendert aus alten Dateien, siehe `legacy_time`).
    pub start_local: NaiveDateTime,
    /// Beginn als Unix-Zeit (UTC-Epoche, `start_local` als Ortszeit interpretiert).
    pub start_unix: i64,
    pub duration_min: u32,
    pub medium_name: String,
    pub long_name: String,
    pub short_desc: String,
    pub long_desc: String,
    pub genres: Vec<String>,
    /// Datei ohne `tz="local"` (alter Kern: Zeit = UTC + 2 min).
    pub legacy_time: bool,
}

impl Programme {
    pub fn end_local(&self) -> NaiveDateTime {
        self.start_local + chrono::Duration::minutes(self.duration_min as i64)
    }
    /// Anzeigetitel: langer Name, sonst mittlerer.
    pub fn title(&self) -> &str {
        if self.long_name.is_empty() {
            &self.medium_name
        } else {
            &self.long_name
        }
    }
    pub fn is_running_at(&self, now: NaiveDateTime) -> bool {
        self.start_local <= now && now < self.end_local()
    }
}

/// Kurzform fuer das Display.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct ProgrammeBrief {
    pub title: String,
    pub start_unix: i64,
    pub end_unix: i64,
    pub duration_min: u32,
    pub legacy_time: bool,
}

impl From<&Programme> for ProgrammeBrief {
    fn from(p: &Programme) -> Self {
        Self {
            title: p.title().to_string(),
            start_unix: p.start_unix,
            end_unix: p.start_unix + p.duration_min as i64 * 60,
            duration_min: p.duration_min,
            legacy_time: p.legacy_time,
        }
    }
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
pub struct NowNext {
    pub sid: u32,
    pub now: Option<ProgrammeBrief>,
    pub next: Option<ProgrammeBrief>,
}

// ---------------------------------------------------------------------------
// Parser des v1-Formats
// ---------------------------------------------------------------------------

/// `yyyy-M-dTHH:mm` (ein- oder zweistellig, optional Sekunden, optional `Z`/Offset ignoriert).
pub fn parse_time(s: &str) -> Option<NaiveDateTime> {
    let s = s.trim();
    let (d, t) = s.split_once('T').or_else(|| s.split_once(' '))?;
    let mut dp = d.split('-');
    let y: i32 = dp.next()?.trim().parse().ok()?;
    let m: u32 = dp.next()?.trim().parse().ok()?;
    let day: u32 = dp.next()?.trim().parse().ok()?;
    let t: String = t.chars().take_while(|c| c.is_ascii_digit() || *c == ':').collect();
    let mut tp = t.split(':');
    let h: u32 = tp.next()?.trim().parse().ok()?;
    let mi: u32 = tp.next()?.trim().parse().ok()?;
    let sec: u32 = tp.next().and_then(|x| x.trim().parse().ok()).unwrap_or(0);
    let date = NaiveDate::from_ymd_opt(y, m, day)?;
    let time = NaiveTime::from_hms_opt(h, mi, sec)?;
    Some(NaiveDateTime::new(date, time))
}

/// `PT04H55M`, `PT05M`, `PT1H`, auch `P1DT2H` -> Minuten.
pub fn parse_duration_min(s: &str) -> Option<u32> {
    let s = s.trim().to_ascii_uppercase();
    let s = s.strip_prefix('P')?;
    let mut total: u64 = 0;
    let mut num = String::new();
    let mut in_time = false;
    let mut any = false;
    for c in s.chars() {
        match c {
            '0'..='9' => num.push(c),
            'T' => in_time = true,
            'D' | 'H' | 'M' | 'S' => {
                let n: u64 = num.parse().ok()?;
                num.clear();
                any = true;
                total += match c {
                    'D' => n * 24 * 60,
                    'H' => n * 60,
                    'M' if in_time => n,
                    'M' => n * 30 * 24 * 60,
                    _ => 0,
                };
            }
            _ => return None,
        }
    }
    any.then_some(total.min(u32::MAX as u64) as u32)
}

fn local_unix(dt: NaiveDateTime) -> i64 {
    match Local.from_local_datetime(&dt) {
        chrono::LocalResult::Single(t) => t.timestamp(),
        chrono::LocalResult::Ambiguous(a, _) => a.timestamp(),
        chrono::LocalResult::None => Local.from_local_datetime(&(dt + chrono::Duration::hours(1))).earliest().map(|t| t.timestamp()).unwrap_or(0),
    }
}

/// Ergebnis eines geparsten Sendeplans.
#[derive(Debug, Clone, PartialEq)]
pub struct ParsedSchedule {
    pub programmes: Vec<Programme>,
    pub legacy_time: bool,
    /// Uebersprungene `<programme>`-Elemente (fehlende/unlesbare Zeit).
    pub skipped: usize,
}

/// Parst ein `<epg system="DAB">`-Dokument. Fehlerhafte Programme werden
/// uebersprungen; ein Dokument ohne `<epg>`-Wurzel ist ein Fehler.
pub fn parse_epg_xml(sid: u32, xml: &str) -> Result<ParsedSchedule, String> {
    let doc = xml::parse(xml);
    let root = doc.iter().find(|e| e.name == "epg").ok_or_else(|| "no <epg> root".to_string())?;
    let legacy = !root.attr("tz").map(|v| v.eq_ignore_ascii_case("local")).unwrap_or(false);
    let mut programmes = Vec::new();
    let mut skipped = 0;
    fn text_of(e: &Elem, name: &str) -> String {
        e.child(name).map(|c| c.text().to_string()).unwrap_or_default()
    }
    use xml::Elem;
    let mut visit = |prog: &Elem| {
        let Some(time) = prog.child("location").and_then(|l| l.child("time")) else {
            skipped += 1;
            return;
        };
        let (Some(start), Some(dur)) = (time.attr("time").and_then(parse_time), time.attr("duration").and_then(parse_duration_min)) else {
            skipped += 1;
            return;
        };
        let medium_name = text_of(prog, "mediumName");
        let long_name = text_of(prog, "longName");
        if medium_name.is_empty() && long_name.is_empty() {
            skipped += 1;
            return;
        }
        let mut short_desc = String::new();
        let mut long_desc = String::new();
        for md in prog.children("mediaDescription") {
            if short_desc.is_empty() {
                short_desc = text_of(md, "shortDescription");
            }
            if long_desc.is_empty() {
                long_desc = text_of(md, "longDescription");
            }
        }
        let genres: Vec<String> = prog.children("genre").map(|g| g.text().to_string()).filter(|g| !g.is_empty()).collect();
        programmes.push(Programme {
            sid,
            start_local: start,
            start_unix: local_unix(start),
            duration_min: dur,
            medium_name,
            long_name,
            short_desc,
            long_desc,
            genres,
            legacy_time: legacy,
        });
    };
    for sched in root.children("schedule") {
        for prog in sched.children("programme") {
            visit(prog);
        }
    }
    // Manche Dateien haben <programme> direkt unter <epg>
    for prog in root.children("programme") {
        visit(prog);
    }
    programmes.sort_by(|a, b| a.start_local.cmp(&b.start_local));
    programmes.dedup_by(|a, b| a.start_local == b.start_local && a.medium_name == b.medium_name);
    Ok(ParsedSchedule { programmes, legacy_time: legacy, skipped })
}

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------

pub fn day_of(date: NaiveDate) -> u32 {
    date.year() as u32 * 10000 + date.month() * 100 + date.day()
}

pub fn date_of(day: u32) -> Option<NaiveDate> {
    NaiveDate::from_ymd_opt((day / 10000) as i32, day / 100 % 100, day % 100)
}

/// Dateiname `<yyyymmdd>_<SID>_SI.xml` -> (Tag, SId).
pub fn parse_file_name(name: &str) -> Option<(u32, u32)> {
    let stem = name.strip_suffix("_SI.xml").or_else(|| name.strip_suffix("_si.xml"))?;
    let (day, sid) = stem.split_once('_')?;
    if day.len() != 8 {
        return None;
    }
    Some((day.parse().ok()?, u32::from_str_radix(sid, 16).ok()?))
}

pub struct EpgCache {
    root: PathBuf,
    index: BTreeMap<(u16, u32, u32), Vec<Programme>>,
    last_tick: Option<Instant>,
}

/// Abstand zweier Now/Next-Pruefungen im Zeitgeber.
const TICK_EVERY: Duration = Duration::from_secs(5);

impl EpgCache {
    pub fn new(root: impl Into<PathBuf>) -> Self {
        Self { root: root.into(), index: BTreeMap::new(), last_tick: None }
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    fn dir_for(&self, eid: u16) -> PathBuf {
        self.root.join(format!("{eid:04X}"))
    }

    pub fn file_for(&self, eid: u16, sid: u32, day: u32) -> PathBuf {
        self.dir_for(eid).join(format!("{day}_{sid:04X}_SI.xml"))
    }

    /// Alle Dateien einlesen (Start). Liefert die Zahl der geladenen Dateien.
    pub fn load(&mut self) -> usize {
        let mut n = 0;
        let Ok(dirs) = std::fs::read_dir(&self.root) else { return 0 };
        for d in dirs.flatten() {
            let Some(eid) = d.file_name().to_str().filter(|s| s.len() == 4).and_then(|s| u16::from_str_radix(s, 16).ok()) else { continue };
            let Ok(files) = std::fs::read_dir(d.path()) else { continue };
            for f in files.flatten() {
                let Some((day, sid)) = f.file_name().to_str().and_then(parse_file_name) else { continue };
                let Ok(xml) = std::fs::read_to_string(f.path()) else { continue };
                match parse_epg_xml(sid, &xml) {
                    Ok(p) => {
                        self.index.insert((eid, sid, day), p.programmes);
                        n += 1;
                    }
                    Err(e) => log::warn!("EPG {}: {e}", f.path().display()),
                }
            }
        }
        n
    }

    /// Dateien und Eintraege aelter als gestern entfernen.
    pub fn prune(&mut self, today: NaiveDate) -> usize {
        let keep_from = day_of(today - chrono::Duration::days(1));
        let old: Vec<(u16, u32, u32)> = self.index.keys().filter(|(_, _, day)| *day < keep_from).copied().collect();
        let mut n = 0;
        for key in old {
            self.index.remove(&key);
            let p = self.file_for(key.0, key.1, key.2);
            if p.is_file() {
                if std::fs::remove_file(&p).is_ok() {
                    n += 1;
                }
            }
        }
        // Auch Dateien, die nicht geparst werden konnten
        if let Ok(dirs) = std::fs::read_dir(&self.root) {
            for d in dirs.flatten() {
                if let Ok(files) = std::fs::read_dir(d.path()) {
                    for f in files.flatten() {
                        if let Some((day, _)) = f.file_name().to_str().and_then(parse_file_name) {
                            if day < keep_from && std::fs::remove_file(f.path()).is_ok() {
                                n += 1;
                            }
                        }
                    }
                }
            }
        }
        n
    }

    /// `epg_object` (sid != 0) verarbeiten: Datei bei Aenderung schreiben,
    /// parsen, indexieren. Liefert `Ok(true)`, wenn sich der Inhalt geaendert
    /// hat oder der Tag neu ist.
    pub fn store_object(&mut self, eid: u16, sid: u32, day: u32, xml: &str) -> Result<bool, String> {
        if sid == 0 || day == 0 {
            return Err("service information, not a schedule".into());
        }
        let parsed = parse_epg_xml(sid, xml)?;
        let path = self.file_for(eid, sid, day);
        let same = std::fs::read_to_string(&path).map(|old| old == xml).unwrap_or(false);
        if !same {
            std::fs::create_dir_all(self.dir_for(eid)).map_err(|e| e.to_string())?;
            std::fs::write(&path, xml).map_err(|e| format!("{}: {e}", path.display()))?;
        }
        let known = self.index.contains_key(&(eid, sid, day));
        let changed = !same || !known;
        self.index.insert((eid, sid, day), parsed.programmes);
        Ok(changed)
    }

    /// Tage mit Daten (aufsteigend).
    pub fn days(&self, eid: u16) -> Vec<u32> {
        let mut v: Vec<u32> = self.index.keys().filter(|(e, _, _)| *e == eid).map(|(_, _, d)| *d).collect();
        v.sort_unstable();
        v.dedup();
        v
    }

    /// Dienste mit Daten an einem Tag.
    pub fn services(&self, eid: u16, day: u32) -> Vec<u32> {
        let mut v: Vec<u32> = self.index.keys().filter(|(e, _, d)| *e == eid && *d == day).map(|(_, s, _)| *s).collect();
        v.sort_unstable();
        v
    }

    /// Alle Dienste mit irgendwelchen Daten.
    pub fn all_services(&self, eid: u16) -> Vec<u32> {
        let mut v: Vec<u32> = self.index.keys().filter(|(e, _, _)| *e == eid).map(|(_, s, _)| *s).collect();
        v.sort_unstable();
        v.dedup();
        v
    }

    pub fn programmes(&self, eid: u16, sid: u32, day: u32) -> &[Programme] {
        self.index.get(&(eid, sid, day)).map(Vec::as_slice).unwrap_or(&[])
    }

    /// Sendungen, die an `day` laufen (auch aus der Datei des Vortags, wenn
    /// sie in den Tag hineinreichen), sortiert.
    pub fn programmes_for_day(&self, eid: u16, sid: u32, day: u32) -> Vec<Programme> {
        let Some(date) = date_of(day) else { return Vec::new() };
        let start = date.and_hms_opt(0, 0, 0).unwrap();
        let end = start + chrono::Duration::days(1);
        let prev = day_of(date - chrono::Duration::days(1));
        let mut v: Vec<Programme> = self
            .programmes(eid, sid, prev)
            .iter()
            .chain(self.programmes(eid, sid, day).iter())
            .filter(|p| p.start_local < end && p.end_local() > start)
            .cloned()
            .collect();
        v.sort_by(|a, b| a.start_local.cmp(&b.start_local));
        v.dedup_by(|a, b| a.start_local == b.start_local && a.medium_name == b.medium_name);
        v
    }

    /// Laufende und naechste Sendung zu `now` (Ortszeit).
    pub fn now_next(&self, eid: u16, sid: u32, now: NaiveDateTime) -> Option<NowNext> {
        let today = day_of(now.date());
        let tomorrow = day_of(now.date() + chrono::Duration::days(1));
        let yesterday = day_of(now.date() - chrono::Duration::days(1));
        let mut all: Vec<&Programme> = [yesterday, today, tomorrow].iter().flat_map(|d| self.programmes(eid, sid, *d).iter()).collect();
        if all.is_empty() {
            return None;
        }
        all.sort_by(|a, b| a.start_local.cmp(&b.start_local));
        let running = all.iter().filter(|p| p.is_running_at(now)).max_by_key(|p| p.start_local).copied();
        let next = match running {
            Some(r) => all.iter().find(|p| p.start_local >= r.end_local() || (p.start_local > now && p.start_local > r.start_local)).copied(),
            None => all.iter().find(|p| p.start_local > now).copied(),
        };
        if running.is_none() && next.is_none() {
            return None;
        }
        Some(NowNext { sid, now: running.map(ProgrammeBrief::from), next: next.map(ProgrammeBrief::from) })
    }

    pub fn len(&self) -> usize {
        self.index.len()
    }

    pub fn is_empty(&self) -> bool {
        self.index.is_empty()
    }
}

/// Beide Caches aus dem Datenordner oeffnen (Start), alte EPG-Dateien entfernen.
pub fn open_caches(dirs: &DataDirs) -> (crate::logos::LogoCache, EpgCache) {
    let mut logos = crate::logos::LogoCache::new(dirs.logos_dir());
    let nl = logos.load();
    let mut epg = EpgCache::new(dirs.epg_dir());
    let ne = epg.load();
    let pruned = epg.prune(Local::now().date_naive());
    if nl + ne > 0 || pruned > 0 {
        log::info!("Cache: {nl} Logos, {ne} EPG-Dateien geladen, {pruned} alte entfernt");
    }
    (logos, epg)
}

// ---------------------------------------------------------------------------
// Anbindung an die App
// ---------------------------------------------------------------------------

impl App {
    /// Kern-Ereignisse mit Logo/EPG-Bezug verarbeiten (Hook in `handle_event`).
    pub fn media_on_event(&mut self, ev: &Event) -> Effects {
        let mut fx = Effects::default();
        match ev {
            Event::MotObject { eid, sid, content_type, name, data_b64 } => {
                if let Some((eid, sid)) = self.logos.store_object(*eid, *sid, *content_type, name, data_b64) {
                    fx.events.push(AppEvent::LogoUpdated { eid, sid });
                    fx.append(self.refresh_preset_logos(eid, sid));
                    fx.append(self.refresh_current_media(true));
                }
            }
            Event::EpgObject { eid, sid, date_yyyymmdd, xml, .. } => {
                if *sid == 0 {
                    for s in self.logos.apply_service_information(*eid, xml) {
                        fx.events.push(AppEvent::LogoUpdated { eid: *eid, sid: s });
                        fx.append(self.refresh_preset_logos(*eid, s));
                    }
                    fx.append(self.refresh_current_media(true));
                } else {
                    match self.epg.store_object(*eid, *sid, *date_yyyymmdd, xml) {
                        Ok(true) => {
                            fx.events.push(AppEvent::EpgUpdated { eid: *eid, sid: *sid, day: *date_yyyymmdd });
                            fx.append(self.refresh_current_media(true));
                        }
                        Ok(false) => {}
                        Err(e) => log::warn!("epg_object {eid:04X}/{sid:04X}/{date_yyyymmdd}: {e}"),
                    }
                }
            }
            Event::ServiceStarted { slot: ServiceSlot::Primary, .. }
            | Event::ServiceStopped { slot: ServiceSlot::Primary, .. }
            | Event::EnsembleFound { .. }
            | Event::DeviceClosed
            | Event::StateSnapshot { .. }
            | Event::Exiting { .. } => fx.append(self.refresh_current_media(true)),
            _ => {}
        }
        fx
    }

    /// Zeitgeber: Now/Next hoechstens alle 5 s neu bestimmen (Hook in `tick`).
    pub fn media_tick(&mut self, now: Instant) -> Effects {
        let due = self.epg.last_tick.map(|t| now.duration_since(t) >= TICK_EVERY).unwrap_or(true);
        if !due {
            return Effects::default();
        }
        let first = self.epg.last_tick.is_none();
        self.epg.last_tick = Some(now);
        let mut fx = if first { self.backfill_preset_logos() } else { Effects::default() };
        fx.append(self.refresh_current_media(false));
        fx
    }

    /// Logo (128x128) und Now/Next des aktuellen Dienstes in den Zustand
    /// uebernehmen; bei Aenderung `current_media` an das Frontend.
    fn refresh_current_media(&mut self, include_logo: bool) -> Effects {
        let key = self.state.ensemble.as_ref().zip(self.state.current.as_ref()).map(|(e, c)| (e.eid, c.sid));
        let (logo, now_next) = match key {
            Some((eid, sid)) => {
                let logo = if include_logo { self.logos.data_url(eid, sid, crate::logos::LogoSize::Medium) } else { self.state.logo_data_url.clone() };
                (logo, self.epg.now_next(eid, sid, Local::now().naive_local()))
            }
            None => (None, None),
        };
        if logo == self.state.logo_data_url && now_next == self.state.now_next {
            return Effects::default();
        }
        self.state.logo_data_url = logo.clone();
        self.state.now_next = now_next.clone();
        let mut fx = Effects::default();
        fx.events.push(AppEvent::CurrentMedia { logo_data_url: logo, now_next });
        fx
    }

    /// Now/Next fuer einen beliebigen Dienst des aktuellen Ensembles (Kommando).
    pub fn now_next_for(&self, sid: u32) -> Option<NowNext> {
        let eid = self.state.ensemble.as_ref()?.eid;
        self.epg.now_next(eid, sid, Local::now().naive_local())
    }

    /// Dienste mit EPG-Daten an einem Tag, mit Namen aus der Senderliste.
    pub fn epg_services_named(&self, eid: u16, day: u32) -> Vec<(u32, String)> {
        self.epg
            .services(eid, day)
            .into_iter()
            .map(|sid| {
                let name = self
                    .state
                    .services
                    .iter()
                    .find(|s| s.sid == sid)
                    .map(|s| s.name.trim().to_string())
                    .unwrap_or_else(|| format!("{sid:04X}"));
                (sid, name)
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn data_dir() -> PathBuf {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests").join("data")
    }

    fn read(name: &str) -> String {
        std::fs::read_to_string(data_dir().join("10BC").join(name)).unwrap()
    }

    fn tmp(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("dabclassic-epg-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn time_and_duration_formats() {
        assert_eq!(parse_time("2026-4-13T22:02"), NaiveDate::from_ymd_opt(2026, 4, 13).unwrap().and_hms_opt(22, 2, 0));
        assert_eq!(parse_time("2026-04-14T07:32"), NaiveDate::from_ymd_opt(2026, 4, 14).unwrap().and_hms_opt(7, 32, 0));
        assert_eq!(parse_time("2026-4-14T7:05:30Z"), NaiveDate::from_ymd_opt(2026, 4, 14).unwrap().and_hms_opt(7, 5, 30));
        assert_eq!(parse_time("garbage"), None);
        assert_eq!(parse_duration_min("PT05M"), Some(5));
        assert_eq!(parse_duration_min("PT04H55M"), Some(295));
        assert_eq!(parse_duration_min("PT1H"), Some(60));
        assert_eq!(parse_duration_min("P1DT2H"), Some(26 * 60));
        assert_eq!(parse_duration_min("5 min"), None);
    }

    #[test]
    fn xml_tolerates_control_chars_and_entities() {
        let src = "<epg system=\"DAB\">\n<schedule><programme id=\"x\"><mediumName>Campus &amp; Karrier</mediumName><location><time duration=\"PT05M\" time=\"2026-4-14T07:02\"/></location><genre href=\"OriginationCS.FormatCS.\u{10}\">Zeitgeschehen</genre></programme></schedule></epg>";
        let p = parse_epg_xml(0xD210, src).unwrap();
        assert_eq!(p.programmes.len(), 1);
        assert_eq!(p.programmes[0].medium_name, "Campus & Karrier");
        assert_eq!(p.programmes[0].genres, vec!["Zeitgeschehen"]);
        assert!(p.legacy_time);
        let src2 = "<epg system=\"DAB\" tz=\"local\"><schedule><programme><mediumName>A</mediumName><location><time duration=\"PT05M\" time=\"2026-4-14T07:02\"/></location></programme><programme><mediumName>Kaputt</mediumName><location><time duration=\"PT05M\" time=\"heute\"/></location></programme><programme><mediumName>B</mediumName><location><time duration=\"PT10M\" time=\"2026-4-14T07:07\"/></location></programme></schedule></epg>";
        let p = parse_epg_xml(1, src2).unwrap();
        assert_eq!((p.programmes.len(), p.skipped), (2, 1), "kaputtes Programm wird uebersprungen");
        assert!(!p.legacy_time);
        assert!(parse_epg_xml(1, "<serviceInformation/>").is_err());
        // Abgeschnittene Datei liefert die vollstaendigen Programme
        let cut = &src2[..src2.find("<programme><mediumName>B").unwrap() + 30];
        assert_eq!(parse_epg_xml(1, cut).unwrap().programmes.len(), 1);
    }

    #[test]
    fn real_v1_files_dlf() {
        let p = parse_epg_xml(0xD210, &read("20260414_D210_SI.xml")).unwrap();
        assert_eq!(p.programmes.len(), 49, "Dlf 2026-04-14: alle 49 Programme");
        assert_eq!(p.skipped, 0);
        assert!(p.legacy_time);
        let iam = p.programmes.iter().find(|x| x.long_name == "Informationen am Morgen").expect("IaM");
        assert_eq!(iam.start_local, NaiveDate::from_ymd_opt(2026, 4, 14).unwrap().and_hms_opt(3, 7, 0).unwrap());
        assert_eq!(iam.duration_min, 3 * 60 + 55);
        assert_eq!(iam.medium_name, "Informationen am");
        assert!(iam.short_desc.starts_with("05:30-05:35 Nachrichten"));
        assert_eq!(iam.genres, vec!["Politik", "Zeitgeschehen"], "Steuerzeichen im genre href stoert nicht");
        assert!(p.programmes.iter().any(|x| x.long_name == "Campus & Karriere"), "Entity aufgeloest");
        // sortiert
        assert!(p.programmes.windows(2).all(|w| w[0].start_local <= w[1].start_local));
    }

    #[test]
    fn real_v1_files_kultur_and_nova() {
        let k = parse_epg_xml(0xD220, &read("20260414_D220_SI.xml")).unwrap();
        assert_eq!(k.programmes.len(), 31);
        let n = parse_epg_xml(0xD230, &read("20260414_D230_SI.xml")).unwrap();
        assert_eq!(n.programmes.len(), 9);
        assert_eq!(n.skipped, 0);
        let first = &n.programmes[0];
        assert_eq!(first.start_local.date(), NaiveDate::from_ymd_opt(2026, 4, 13).unwrap(), "Vortag 22:xx (UTC-Datei)");
        assert!(first.duration_min > 0);
    }

    #[test]
    fn cache_load_index_now_next_prune() {
        let root = tmp("cache");
        std::fs::create_dir_all(root.join("10BC")).unwrap();
        for f in ["20260414_D210_SI.xml", "20260414_D220_SI.xml", "20260414_D230_SI.xml"] {
            std::fs::copy(data_dir().join("10BC").join(f), root.join("10BC").join(f)).unwrap();
        }
        let mut c = EpgCache::new(&root);
        assert_eq!(c.load(), 3);
        assert_eq!(c.days(0x10BC), vec![20260414]);
        assert_eq!(c.services(0x10BC, 20260414), vec![0xD210, 0xD220, 0xD230]);
        assert_eq!(c.programmes(0x10BC, 0xD210, 20260414).len(), 49);
        // Now/Next am 14.04.2026 05:00 (Dateizeit): Informationen am Morgen laeuft (03:07 + 3h55)
        let now = NaiveDate::from_ymd_opt(2026, 4, 14).unwrap().and_hms_opt(5, 0, 0).unwrap();
        let nn = c.now_next(0x10BC, 0xD210, now).unwrap();
        assert_eq!(nn.now.as_ref().unwrap().title, "Informationen am Morgen");
        assert_eq!(nn.now.as_ref().unwrap().duration_min, 235);
        assert_eq!(nn.next.as_ref().unwrap().title, "Nachrichten");
        assert_eq!(nn.next.as_ref().unwrap().start_unix - nn.now.as_ref().unwrap().end_unix, 0);
        // Vor der ersten Sendung: nur next
        let early = NaiveDate::from_ymd_opt(2026, 4, 13).unwrap().and_hms_opt(20, 0, 0).unwrap();
        let nn = c.now_next(0x10BC, 0xD210, early).unwrap();
        assert!(nn.now.is_none() && nn.next.is_some());
        assert!(c.now_next(0x10BC, 0xD210, early - chrono::Duration::days(5)).is_none());
        // Tagesliste mit Uebertrag: Programm vom 13. 22:07 (4h55) laeuft in den 14. hinein
        let day = c.programmes_for_day(0x10BC, 0xD210, 20260414);
        assert!(day.iter().any(|p| p.long_name == "Deutschlandfunk Radionacht"));
        // store_object: gleicher Inhalt -> false, anderer Tag -> true
        let xml = read("20260414_D210_SI.xml");
        assert!(!c.store_object(0x10BC, 0xD210, 20260414, &xml).unwrap());
        assert!(c.store_object(0x10BC, 0xD210, 20260415, &xml.replace("2026-4-14", "2026-4-15")).unwrap());
        assert!(root.join("10BC").join("20260415_D210_SI.xml").is_file());
        assert!(c.store_object(0x10BC, 0, 0, "<serviceInformation/>").is_err());
        // Bereinigung: heute = 16.04. -> 14.04. weg, 15.04. bleibt
        let removed = c.prune(NaiveDate::from_ymd_opt(2026, 4, 16).unwrap());
        assert_eq!(removed, 3);
        assert_eq!(c.days(0x10BC), vec![20260415]);
        assert!(!root.join("10BC").join("20260414_D210_SI.xml").exists());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn day_helpers() {
        assert_eq!(day_of(NaiveDate::from_ymd_opt(2026, 9, 12).unwrap()), 20260912);
        assert_eq!(date_of(20260912), NaiveDate::from_ymd_opt(2026, 9, 12));
        assert_eq!(parse_file_name("20260414_D210_SI.xml"), Some((20260414, 0xD210)));
        assert_eq!(parse_file_name("list.xml"), None);
    }
}
