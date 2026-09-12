//! Logo-Cache (Entscheidung 14): Senderlogos aus dem SPI-Paketdienst
//! (`mot_object`, PNG/JPEG) landen als Dateien unter
//! `data/logos/<EID>/<name>` (Name wie im MOT-Verzeichnis, v1-kompatibel:
//! `<SId>_<Name>_<WxH>.png`). Ein Index je (EId, SId) kennt die verfuegbaren
//! Groessen; die Auswahl liefert je Wunschgroesse die beste vorhandene Datei
//! (320x240 -> 128x128 -> 112x32 -> 32x32). Logos, deren SId der Kern nicht
//! aufloesen konnte (`sid` = 0), werden ueber die Service-Information
//! (`epg_object` mit `sid` 0, `<serviceInformation>`) nachtraeglich zugeordnet.
//!
//! Fuer die Shell (und spaeter die Web-Remote) werden Logos als
//! `data:`-URL geliefert, nicht als Pfad.

use crate::epg::xml::{parse, Elem};
use crate::{App, Effects, Preset};
use base64::Engine as _;
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, HashMap};
use std::path::{Path, PathBuf};

/// Wunschgroesse; die Auswahl faellt auf die naechstbeste vorhandene zurueck.
#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum LogoSize {
    /// 320x240 (Detailansicht)
    Large,
    /// 128x128 (Display)
    Medium,
    /// 32x32 (Speicherplaetze, Senderliste)
    Small,
}

impl LogoSize {
    /// Bevorzugte Abmessungen in Reihenfolge.
    fn preference(self) -> &'static [(u32, u32)] {
        match self {
            LogoSize::Large => &[(320, 240), (128, 128), (112, 32), (32, 32)],
            LogoSize::Medium => &[(128, 128), (320, 240), (112, 32), (32, 32)],
            LogoSize::Small => &[(32, 32), (112, 32), (128, 128), (320, 240)],
        }
    }
}

/// Eine Logo-Datei im Cache.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct LogoEntry {
    /// Dateiname innerhalb von `<EID>/`.
    pub name: String,
    pub width: u32,
    pub height: u32,
    /// "image/png" oder "image/jpeg"
    pub mime: String,
}

#[derive(Default)]
pub struct LogoCache {
    root: PathBuf,
    /// (eid, sid) -> Dateien (SId > 0).
    by_service: HashMap<(u16, u32), Vec<LogoEntry>>,
    /// Dateien je EId, deren SId (noch) nicht bekannt ist.
    unassigned: HashMap<u16, Vec<LogoEntry>>,
    /// Zuordnung aus der Service-Information: eid -> (Dateiname klein -> sid).
    si_names: HashMap<u16, HashMap<String, u32>>,
}

/// MOT-Content-Types (TS 101 756, Tabelle 17): Bild/JFIF und Bild/PNG.
pub const CONTENT_TYPE_JPEG: u16 = 0x0201;
pub const CONTENT_TYPE_PNG: u16 = 0x0203;

impl LogoCache {
    pub fn new(root: impl Into<PathBuf>) -> Self {
        Self { root: root.into(), ..Default::default() }
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    fn dir_for(&self, eid: u16) -> PathBuf {
        self.root.join(format!("{eid:04X}"))
    }

    /// Ordner `<EID>` -> EId (Hex, 4 Stellen, Gross- oder Kleinschreibung).
    fn parse_eid_dir(name: &str) -> Option<u16> {
        (name.len() == 4).then(|| u16::from_str_radix(name, 16).ok()).flatten()
    }

    /// Cache vom Dateisystem einlesen (Start). Bereits vorhandene Eintraege
    /// bleiben erhalten, Dateien werden ergaenzt.
    pub fn load(&mut self) -> usize {
        let mut n = 0;
        let Ok(dirs) = std::fs::read_dir(&self.root) else { return 0 };
        for d in dirs.flatten() {
            let Some(eid) = d.file_name().to_str().and_then(Self::parse_eid_dir) else { continue };
            let Ok(files) = std::fs::read_dir(d.path()) else { continue };
            let mut si_xml: Option<String> = None;
            for f in files.flatten() {
                let Some(name) = f.file_name().to_str().map(str::to_string) else { continue };
                let lower = name.to_ascii_lowercase();
                if lower == "list.xml" {
                    si_xml = std::fs::read_to_string(f.path()).ok();
                    continue;
                }
                let Some(mime) = mime_for_name(&lower) else { continue };
                let (w, h) = dims_from_name(&name).or_else(|| std::fs::read(f.path()).ok().and_then(|b| png_dims(&b))).unwrap_or((0, 0));
                let sid = sid_from_name(&name).unwrap_or(0);
                self.insert(eid, sid, LogoEntry { name, width: w, height: h, mime: mime.into() });
                n += 1;
            }
            if let Some(xml) = si_xml {
                self.apply_service_information(eid, &xml);
            }
        }
        n
    }

    fn insert(&mut self, eid: u16, sid: u32, entry: LogoEntry) -> bool {
        let list = if sid != 0 { self.by_service.entry((eid, sid)).or_default() } else { self.unassigned.entry(eid).or_default() };
        if let Some(e) = list.iter_mut().find(|e| e.name.eq_ignore_ascii_case(&entry.name)) {
            let changed = *e != entry;
            *e = entry;
            changed
        } else {
            list.push(entry);
            true
        }
    }

    /// `mot_object` verarbeiten: Datei schreiben (nur bei neuem Inhalt) und
    /// indexieren. Liefert `Some((eid, sid))`, wenn sich fuer einen Dienst
    /// etwas geaendert hat (fuer `logo_updated`).
    pub fn store_object(&mut self, eid: u16, sid: u32, content_type: u16, name: &str, data_b64: &str) -> Option<(u16, u32)> {
        let mime = match content_type {
            CONTENT_TYPE_PNG => "image/png",
            CONTENT_TYPE_JPEG => "image/jpeg",
            _ => mime_for_name(&name.to_ascii_lowercase())?,
        };
        let bytes = base64::engine::general_purpose::STANDARD.decode(data_b64.trim()).ok()?;
        if bytes.is_empty() {
            return None;
        }
        let clean = sanitize_name(name);
        let dir = self.dir_for(eid);
        let path = dir.join(&clean);
        let same = std::fs::read(&path).map(|old| old == bytes).unwrap_or(false);
        let mut changed = !same;
        if !same {
            if let Err(e) = std::fs::create_dir_all(&dir).and_then(|_| std::fs::write(&path, &bytes)) {
                log::warn!("Logo {}: {e}", path.display());
                return None;
            }
        }
        let (w, h) = dims_from_name(&clean).or_else(|| png_dims(&bytes)).unwrap_or((0, 0));
        let sid = if sid != 0 {
            sid
        } else {
            sid_from_name(&clean)
                .filter(|s| *s != 0)
                .or_else(|| self.si_names.get(&eid).and_then(|m| m.get(&clean.to_ascii_lowercase()).copied()))
                .unwrap_or(0)
        };
        changed |= self.insert(eid, sid, LogoEntry { name: clean, width: w, height: h, mime: mime.into() });
        (changed && sid != 0).then_some((eid, sid))
    }

    /// Service-Information (`<serviceInformation>`, v1 `list.xml`) auswerten:
    /// `<bearer id="e0:10bc:d210"/>` + `<multimedia url="..."/>` je Dienst.
    /// Bisher nicht zugeordnete Dateien werden ihrem Dienst zugeschlagen.
    /// Liefert die SIds, die dadurch neue Logos bekommen haben.
    pub fn apply_service_information(&mut self, eid: u16, xml: &str) -> Vec<u32> {
        let map = parse_service_information(eid, xml);
        if map.is_empty() {
            return Vec::new();
        }
        let dir = self.dir_for(eid);
        if dir.is_dir() {
            if let Err(e) = std::fs::write(dir.join("list.xml"), xml) {
                log::warn!("list.xml: {e}");
            }
        }
        let mut touched = Vec::new();
        if let Some(pending) = self.unassigned.remove(&eid) {
            for entry in pending {
                match map.get(&entry.name.to_ascii_lowercase()) {
                    Some(&sid) => {
                        if self.insert(eid, sid, entry) && !touched.contains(&sid) {
                            touched.push(sid);
                        }
                    }
                    None => {
                        self.unassigned.entry(eid).or_default().push(entry);
                    }
                }
            }
        }
        self.si_names.insert(eid, map);
        touched
    }

    /// Alle Dateien eines Dienstes (unsortiert).
    pub fn entries(&self, eid: u16, sid: u32) -> &[LogoEntry] {
        self.by_service.get(&(eid, sid)).map(Vec::as_slice).unwrap_or(&[])
    }

    /// Verfuegbare Groessen je Dienst, aufsteigend nach Flaeche.
    pub fn sizes(&self, eid: u16, sid: u32) -> Vec<(u32, u32)> {
        let mut v: Vec<(u32, u32)> = self.entries(eid, sid).iter().map(|e| (e.width, e.height)).collect();
        v.sort_by_key(|(w, h)| (w * h, *w));
        v.dedup();
        v
    }

    /// Alle Dienste mit Logos je Ensemble.
    pub fn services(&self, eid: u16) -> Vec<u32> {
        let mut v: Vec<u32> = self.by_service.keys().filter(|(e, _)| *e == eid).map(|(_, s)| *s).collect();
        v.sort_unstable();
        v
    }

    /// Beste Datei fuer die Wunschgroesse.
    pub fn pick(&self, eid: u16, sid: u32, size: LogoSize) -> Option<&LogoEntry> {
        let list = self.entries(eid, sid);
        if list.is_empty() {
            return None;
        }
        for (w, h) in size.preference() {
            if let Some(e) = list.iter().find(|e| e.width == *w && e.height == *h && e.mime == "image/png") {
                return Some(e);
            }
            if let Some(e) = list.iter().find(|e| e.width == *w && e.height == *h) {
                return Some(e);
            }
        }
        // Unbekannte Abmessungen: naechstliegende Flaeche zur ersten Praeferenz.
        let (pw, ph) = size.preference()[0];
        let want = (pw * ph) as i64;
        list.iter().min_by_key(|e| ((e.width * e.height) as i64 - want).abs())
    }

    pub fn logo_path(&self, eid: u16, sid: u32, size: LogoSize) -> Option<PathBuf> {
        self.pick(eid, sid, size).map(|e| self.dir_for(eid).join(&e.name))
    }

    /// Logo als `data:`-URL (Base64), fuer Display, Slots und Web-Remote.
    pub fn data_url(&self, eid: u16, sid: u32, size: LogoSize) -> Option<String> {
        let e = self.pick(eid, sid, size)?;
        let bytes = std::fs::read(self.dir_for(eid).join(&e.name)).ok()?;
        Some(format!("data:{};base64,{}", e.mime, base64::engine::general_purpose::STANDARD.encode(bytes)))
    }

    /// Anzahl indexierter Dateien (Tests, Log).
    pub fn len(&self) -> usize {
        self.by_service.values().map(Vec::len).sum::<usize>() + self.unassigned.values().map(Vec::len).sum::<usize>()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

/// `<serviceInformation>` -> Dateiname (klein) -> SId. Bearer mit passender
/// EId werden bevorzugt, sonst der erste Bearer des Dienstes.
pub fn parse_service_information(eid: u16, xml: &str) -> HashMap<String, u32> {
    let mut out = HashMap::new();
    let doc = parse(xml);
    let Some(root) = doc.iter().find(|e| e.name.eq_ignore_ascii_case("serviceInformation")) else { return out };
    fn walk(e: &Elem, eid: u16, out: &mut HashMap<String, u32>) {
        if e.name == "service" {
            let mut sid: Option<u32> = None;
            for b in e.children("bearer") {
                if let Some(id) = b.attr("id") {
                    let parts: Vec<&str> = id.split(':').collect();
                    if parts.len() >= 3 {
                        let e_ok = u16::from_str_radix(parts[1], 16).map(|x| x == eid).unwrap_or(false);
                        if let Ok(s) = u32::from_str_radix(parts[2], 16) {
                            if e_ok || sid.is_none() {
                                sid = Some(s);
                            }
                            if e_ok {
                                break;
                            }
                        }
                    }
                }
            }
            if let Some(sid) = sid.filter(|s| *s != 0) {
                for md in e.children("mediaDescription") {
                    for mm in md.children("multimedia") {
                        if let Some(url) = mm.attr("url") {
                            out.insert(sanitize_name(url).to_ascii_lowercase(), sid);
                        }
                    }
                }
            }
            return;
        }
        for c in &e.children {
            walk(c, eid, out);
        }
    }
    walk(root, eid, &mut out);
    out
}

/// Unerlaubte Zeichen im Dateinamen ersetzen (wie dab-cli).
pub fn sanitize_name(name: &str) -> String {
    let s: String = name
        .trim()
        .chars()
        .map(|c| if matches!(c, '/' | '\\' | ':' | '*' | '?' | '"' | '<' | '>' | '|') || (c as u32) < 0x20 { '_' } else { c })
        .collect();
    if s.is_empty() {
        "logo.png".into()
    } else {
        s
    }
}

fn mime_for_name(lower: &str) -> Option<&'static str> {
    if lower.ends_with(".png") {
        Some("image/png")
    } else if lower.ends_with(".jpg") || lower.ends_with(".jpeg") || lower.ends_with(".jfif") {
        Some("image/jpeg")
    } else {
        None
    }
}

/// `d210_Dlf_320x240.png` -> 0xD210 (Hex vor dem ersten `_`).
pub fn sid_from_name(name: &str) -> Option<u32> {
    let head = name.split('_').next()?;
    if head.is_empty() || head.len() > 8 {
        return None;
    }
    u32::from_str_radix(head, 16).ok()
}

/// `..._320x240.png` -> (320, 240).
pub fn dims_from_name(name: &str) -> Option<(u32, u32)> {
    let stem = name.rsplit_once('.').map(|(s, _)| s).unwrap_or(name);
    let last = stem.rsplit('_').next()?;
    let (w, h) = last.split_once('x')?;
    Some((w.parse().ok()?, h.parse().ok()?))
}

/// Breite/Hoehe aus dem PNG-IHDR.
pub fn png_dims(bytes: &[u8]) -> Option<(u32, u32)> {
    if bytes.len() < 24 || &bytes[..8] != b"\x89PNG\r\n\x1a\n" || &bytes[12..16] != b"IHDR" {
        return None;
    }
    let w = u32::from_be_bytes(bytes[16..20].try_into().ok()?);
    let h = u32::from_be_bytes(bytes[20..24].try_into().ok()?);
    Some((w, h))
}

/// Groessenuebersicht fuer Debug/Tests.
pub fn size_table(cache: &LogoCache, eid: u16) -> BTreeMap<u32, Vec<(u32, u32)>> {
    cache.services(eid).into_iter().map(|sid| (sid, cache.sizes(eid, sid))).collect()
}

// ---------------------------------------------------------------------------
// Anbindung an die App (Presets, Entscheidung 8: Slot enthaelt Logo-Cache)
// ---------------------------------------------------------------------------

impl App {
    /// Logo-Felder eines Presets aus dem Cache fuellen (beim Belegen).
    pub fn decorate_preset(&self, p: &mut Preset) {
        if p.eid == 0 || p.sid == 0 {
            return;
        }
        p.logo_path = self.logos.logo_path(p.eid, p.sid, LogoSize::Small);
        p.logo_data_url = self.logos.data_url(p.eid, p.sid, LogoSize::Small);
    }

    /// Beim Start: Presets ohne Logo aus dem geladenen Cache nachtragen.
    pub(crate) fn backfill_preset_logos(&mut self) -> Effects {
        let keys: Vec<(u16, u32)> = self
            .presets
            .slots
            .iter()
            .flatten()
            .filter(|p| p.logo_data_url.is_none() && p.eid != 0 && p.sid != 0)
            .map(|p| (p.eid, p.sid))
            .collect();
        let mut fx = Effects::default();
        for (eid, sid) in keys {
            if !self.logos.entries(eid, sid).is_empty() {
                fx.append(self.refresh_preset_logos(eid, sid));
            }
        }
        fx
    }

    /// Nach einem neuen Logo: Presets desselben Dienstes ohne Logo nachtragen.
    pub(crate) fn refresh_preset_logos(&mut self, eid: u16, sid: u32) -> Effects {
        let mut changed = false;
        let path = self.logos.logo_path(eid, sid, LogoSize::Small);
        let url = self.logos.data_url(eid, sid, LogoSize::Small);
        for p in self.presets.slots.iter_mut().flatten() {
            if p.eid == eid && p.sid == sid && (p.logo_data_url != url || p.logo_path != path) {
                p.logo_path = path.clone();
                p.logo_data_url = url.clone();
                changed = true;
            }
        }
        if changed {
            self.save_presets()
        } else {
            Effects::default()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn data_dir() -> PathBuf {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests").join("data")
    }

    fn tmp(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("dabclassic-logos-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn name_parsing() {
        assert_eq!(sid_from_name("d210_Dlf_320x240.png"), Some(0xD210));
        assert_eq!(sid_from_name("10C4_ASA DE_32x32.png"), Some(0x10C4));
        assert_eq!(sid_from_name("logo.png"), None);
        assert_eq!(dims_from_name("d220_Dlf_Kult_112x32.png"), Some((112, 32)));
        assert_eq!(dims_from_name("d220_Dlf_Kult.png"), None);
        let png = std::fs::read(data_dir().join("10BC").join("d210_Dlf_128x128.png")).unwrap();
        assert_eq!(png_dims(&png), Some((128, 128)));
    }

    #[test]
    fn load_v1_cache_and_pick_sizes() {
        let mut c = LogoCache::new(data_dir());
        let n = c.load();
        assert!(n >= 5, "{n} Dateien");
        assert_eq!(c.sizes(0x10BC, 0xD210), vec![(32, 32), (112, 32), (128, 128), (320, 240)]);
        assert!(c.logo_path(0x10BC, 0xD210, LogoSize::Large).unwrap().ends_with("d210_Dlf_320x240.png"));
        assert!(c.logo_path(0x10BC, 0xD210, LogoSize::Medium).unwrap().ends_with("d210_Dlf_128x128.png"));
        assert!(c.logo_path(0x10BC, 0xD210, LogoSize::Small).unwrap().ends_with("d210_Dlf_32x32.png"));
        // Dlf Kultur hat nur 32x32: alle Wuensche fallen darauf zurueck
        assert!(c.logo_path(0x10BC, 0xD220, LogoSize::Large).unwrap().ends_with("d220_Dlf_Kult_32x32.png"));
        assert!(c.logo_path(0x10BC, 0xD230, LogoSize::Small).is_none());
        let url = c.data_url(0x10BC, 0xD210, LogoSize::Small).unwrap();
        assert!(url.starts_with("data:image/png;base64,iVBOR"), "{url}");
    }

    #[test]
    fn store_object_writes_once_and_assigns_via_service_information() {
        let root = tmp("store");
        let mut c = LogoCache::new(&root);
        let png = std::fs::read(data_dir().join("10BC").join("d210_Dlf_32x32.png")).unwrap();
        let b64 = base64::engine::general_purpose::STANDARD.encode(&png);
        // Kern kennt den Dienst
        assert_eq!(c.store_object(0x10BC, 0xD210, CONTENT_TYPE_PNG, "d210_Dlf_32x32.png", &b64), Some((0x10BC, 0xD210)));
        assert!(root.join("10BC").join("d210_Dlf_32x32.png").is_file());
        // Wiederholung mit gleichem Inhalt: keine Aenderung
        assert_eq!(c.store_object(0x10BC, 0xD210, CONTENT_TYPE_PNG, "d210_Dlf_32x32.png", &b64), None);
        // SId nicht aufloesbar (weder vom Kern noch aus dem Namen)
        assert_eq!(c.store_object(0x10BC, 0, CONTENT_TYPE_PNG, "logo_nova_32x32.png", &b64), None);
        assert_eq!(c.len(), 2);
        // Service-Information ordnet zu
        let si = std::fs::read_to_string(data_dir().join("10BC").join("list.xml")).unwrap();
        let si = si.replace("d230_Dlf_Nova_32x32.png", "logo_nova_32x32.png");
        assert_eq!(c.apply_service_information(0x10BC, &si), vec![0xD230]);
        assert!(c.logo_path(0x10BC, 0xD230, LogoSize::Small).unwrap().ends_with("logo_nova_32x32.png"));
        // Danach werden auch neue unbekannte Namen direkt zugeordnet
        assert_eq!(c.store_object(0x10BC, 0, CONTENT_TYPE_PNG, "d230_Dlf_Nova_128x128.png", &b64), Some((0x10BC, 0xD230)));
        assert!(root.join("10BC").join("list.xml").is_file());
        // Neustart: Scan liest alles wieder ein, inkl. list.xml
        let mut again = LogoCache::new(&root);
        again.load();
        assert_eq!(again.sizes(0x10BC, 0xD230), vec![(32, 32), (128, 128)]);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn service_information_map() {
        let si = std::fs::read_to_string(data_dir().join("10BC").join("list.xml")).unwrap();
        let m = parse_service_information(0x10BC, &si);
        assert_eq!(m.get("d210_dlf_320x240.png"), Some(&0xD210));
        assert_eq!(m.get("100d_schwarzw_32x32.png"), Some(&0x100D));
        assert_eq!(m.len(), 16, "vier Dienste x vier Logos");
    }
}
