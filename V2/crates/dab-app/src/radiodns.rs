//! Hybrid Radio: RadioDNS (ETSI TS 103 270) und SPI ueber IP (ETSI TS 102 818
//! v3.1). Fuer Dienste, die im Ensemble keinen (vollstaendigen) SPI-
//! Paketdienst ausstrahlen, holt die App Logos und Sendeplaene aus dem
//! Internet - nur, wenn der Sender das per DNS selbst anbietet.
//!
//! Ablauf (Auftrag Stefan 17.09.2026, Punkt 1; Standard AUS,
//! `Settings::radiodns_enabled`):
//! 1. Aus ECC (FIG 0/9, `EnsembleState::ecc`), EId, SId und SCIdS wird je
//!    Audiodienst der Name `<scids>.<sid>.<eid>.<gcc>.dab.radiodns.org`
//!    gebildet; `gcc` = Country-Id-Nibble der SId + ECC ("de0" fuer
//!    Deutschland). Ein CNAME darauf nennt den zustaendigen Host des Senders
//!    (z. B. `dlf.rdns.deutschlandradio.de`), NXDOMAIN = kein Angebot.
//! 2. SRV `_radiospi._tcp.<host>` (SPI 3.1, meist HTTPS) oder ersatzweise
//!    `_radioepg._tcp.<host>` liefert Server und Port.
//! 3. `/radiodns/spi/3.1/SI.xml` (Service Information: Namen, Bearer,
//!    Logo-URLs) und je Tag `/radiodns/spi/3.1/dab/<gcc>/<eid>/<sid>/<scids>/
//!    <jjjjmmtt>_PI.xml` (Programme Information, gleiches `<epg>`-Format wie
//!    der Broadcast-SPI, nur mit Zeitzonen-Offset in den Zeiten).
//! 4. Logos landen im vorhandenen Logo-Cache (`data/logos/<EID>/`, Name
//!    `<sid>_<name>_<WxH>.png` wie beim Broadcast), Sendeplaene im EPG-Cache
//!    mit Markierung `src="radiodns"` am Wurzelelement. **Broadcast hat
//!    Vorrang**: geholt wird nur, was im Cache fehlt, und ein spaeter
//!    empfangener Broadcast-Sendeplan ueberschreibt den aus dem Netz.
//!
//! Aufteilung: Die App-Schicht plant Auftraege ([`App::radiodns_take_job`])
//! und uebernimmt Ergebnisse ([`App::radiodns_apply`]) ohne Netz und ohne
//! Threads; ein Worker-Thread des Hosts (Tauri `radiodns_cmds`) fuehrt
//! [`run_job`] mit einem [`Fetcher`] aus (echtes Netz: [`NetFetcher`];
//! Tests: Attrappe). DNS geht ueber den System-Resolver (hickory), HTTP ueber
//! ureq mit dem Windows-Zertifikatspeicher (schannel).
//!
//! Live geprueft 17.09.2026: Deutschlandradio (alle drei Programme,
//! `rdns.deutschlandradio.de`, HTTP und HTTPS), WDR/ARD (`dewdr.radiodns.
//! ard.de`, HTTPS) und Absolut relax (`radiodns.radio.cloud`, HTTP).
//! Bekannte Grenzen: keine RadioVIS-Slideshow, kein `_radiotag`; nur das
//! aktuell abgestimmte Ensemble; die Senderliste anderer Ensembles wird
//! nicht nachgeladen.

use crate::app::{App, AppEvent, Effects};
use crate::epg::xml::{parse, Elem};
use crate::Settings;
use dab_api::Event;
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, HashMap};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

/// Ruhezeit nach dem letzten `service_added`, bevor ein Auftrag entsteht.
pub const SETTLE: Duration = Duration::from_secs(10);
/// Ein Ensemble wird hoechstens so oft auf fehlende Logos/Tage geprueft.
pub const RECHECK_EVERY: Duration = Duration::from_secs(6 * 3600);
/// Nach einem Fehler (DNS/HTTP) so lange warten, bevor es wieder losgeht.
pub const RETRY_AFTER_ERROR: Duration = Duration::from_secs(15 * 60);
/// Gueltigkeit eines DNS-Ergebnisses in `hosts.json` (positiv wie negativ).
pub const RESOLVE_TTL_S: i64 = 24 * 3600;
/// Nach einem DNS-Fehler (kein Netz, Timeout) frueher erneut versuchen.
pub const RESOLVE_ERROR_TTL_S: i64 = 3600;
/// Gueltigkeit einer zwischengespeicherten SI.xml (Dateialter).
pub const SI_TTL_S: u64 = 24 * 3600;
pub const MAX_LOGO_BYTES: u64 = 1024 * 1024;
pub const MAX_XML_BYTES: u64 = 4 * 1024 * 1024;
/// Logo-Groessen, die der Cache kennt (crate::logos), in Abrufreihenfolge.
pub const LOGO_SIZES: [(u32, u32); 4] = [(32, 32), (128, 128), (320, 240), (112, 32)];
/// Markierung am `<epg>`-Wurzelelement fuer Sendeplaene aus dem Netz.
pub const SRC_ATTR: &str = "radiodns";
const HOSTS_FILE: &str = "hosts.json";
const MAX_ERRORS: usize = 5;

// ---------------------------------------------------------------------------
// Namen (TS 103 270 5.2.2, TS 102 818 Bearer-URI "dab:")
// ---------------------------------------------------------------------------

/// Global Country Code: Country-Id-Nibble der SId + ECC, z. B. `de0`.
/// 32-Bit-SIds tragen den ECC in den oberen 8 Bits selbst.
pub fn gcc(ecc: u8, sid: u32) -> String {
    if sid > 0xFFFF {
        format!("{:x}{:02x}", (sid >> 20) & 0xF, (sid >> 24) & 0xFF)
    } else {
        format!("{:x}{:02x}", (sid >> 12) & 0xF, ecc)
    }
}

fn sid_hex(sid: u32) -> String {
    if sid > 0xFFFF {
        format!("{sid:08x}")
    } else {
        format!("{sid:04x}")
    }
}

/// `<scids>.<sid>.<eid>.<gcc>.dab.radiodns.org`
pub fn fqdn(ecc: u8, eid: u16, sid: u32, scids: u8) -> String {
    format!("{:x}.{}.{eid:04x}.{}.dab.radiodns.org", scids, sid_hex(sid), gcc(ecc, sid))
}

/// Bearer-Id im SPI-XML: `dab:<gcc>.<eid>.<sid>.<scids>`.
pub fn bearer_id(ecc: u8, eid: u16, sid: u32, scids: u8) -> String {
    format!("dab:{}.{eid:04x}.{}.{:x}", gcc(ecc, sid), sid_hex(sid), scids)
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Bearer {
    pub gcc: String,
    pub eid: u16,
    pub sid: u32,
    pub scids: u8,
}

/// `dab:de0.10bc.d210.0` -> Bearer; andere Schemata (`fm:`, `http`) -> None.
pub fn parse_bearer(id: &str) -> Option<Bearer> {
    let rest = id.trim().strip_prefix("dab:")?;
    let parts: Vec<&str> = rest.split('.').collect();
    if parts.len() < 4 {
        return None;
    }
    Some(Bearer {
        gcc: parts[0].to_ascii_lowercase(),
        eid: u16::from_str_radix(parts[1], 16).ok()?,
        sid: u32::from_str_radix(parts[2], 16).ok()?,
        scids: u8::from_str_radix(parts[3], 16).ok()?,
    })
}

/// Pfad der Programme Information eines Tages (TS 102 818 v3.1, 8.2).
pub fn pi_path(ecc: u8, eid: u16, sid: u32, scids: u8, day: u32) -> String {
    format!("/radiodns/spi/3.1/dab/{}/{eid:04x}/{}/{:x}/{day}_PI.xml", gcc(ecc, sid), sid_hex(sid), scids)
}

pub const SI_PATH: &str = "/radiodns/spi/3.1/SI.xml";

// ---------------------------------------------------------------------------
// Netz-Abstraktion
// ---------------------------------------------------------------------------

/// Ergebnis der DNS-Aufloesung: Server des SPI-Dienstes.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Endpoint {
    pub host: String,
    pub port: u16,
    pub https: bool,
}

impl Endpoint {
    pub fn base_url(&self) -> String {
        let default = if self.https { 443 } else { 80 };
        if self.port == default {
            format!("{}://{}", if self.https { "https" } else { "http" }, self.host)
        } else {
            format!("{}://{}:{}", if self.https { "https" } else { "http" }, self.host, self.port)
        }
    }
    /// Dateiname-tauglicher Schluessel fuer die SI-Zwischendatei.
    fn key(&self) -> String {
        crate::logos::sanitize_name(&format!("{}_{}", self.host, self.port)).to_ascii_lowercase()
    }
}

/// DNS + HTTP, austauschbar (Tests). Alle Aufrufe blockieren.
pub trait Fetcher: Send {
    /// CNAME und SRV. `Ok(None)` = kein RadioDNS-Eintrag (NXDOMAIN / kein SRV).
    fn resolve(&self, fqdn: &str) -> Result<Option<Endpoint>, String>;
    /// GET; `Ok(None)` = nicht verfuegbar: 404 (kein Sendeplan fuer den
    /// Tag), 401/403 (Server verlangt Anmeldung, z. B. radioplayer.org), 410.
    fn get(&self, url: &str, max_bytes: u64) -> Result<Option<Vec<u8>>, String>;
}

// ---------------------------------------------------------------------------
// SI.xml
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SiLogo {
    pub url: String,
    pub width: u32,
    pub height: u32,
    pub mime: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Default)]
pub struct SiService {
    pub short_name: String,
    pub medium_name: String,
    pub bearers: Vec<String>,
    pub logos: Vec<SiLogo>,
}

/// `<serviceInformation><services><service>...` (SPI 3.1). Tolerant: Groesse
/// aus den Attributen, sonst aus dem Dateinamen (`_32x32.png`); MIME aus
/// `mimeValue`, sonst aus der Endung.
pub fn parse_si(xml: &str) -> Vec<SiService> {
    let doc = parse(xml);
    let Some(root) = doc.iter().find(|e| e.name.eq_ignore_ascii_case("serviceInformation")) else { return Vec::new() };
    let mut out = Vec::new();
    fn walk(e: &Elem, out: &mut Vec<SiService>) {
        if e.name == "service" {
            let mut s = SiService::default();
            s.short_name = e.child("shortName").map(|c| c.text().to_string()).unwrap_or_default();
            s.medium_name = e.child("mediumName").map(|c| c.text().to_string()).unwrap_or_default();
            s.bearers = e.children("bearer").filter_map(|b| b.attr("id").map(|s| s.trim().to_string())).collect();
            for md in e.children("mediaDescription") {
                for mm in md.children("multimedia") {
                    let Some(url) = mm.attr("url").map(str::trim).filter(|u| !u.is_empty()) else { continue };
                    let (w, h) = match (mm.attr("width").and_then(|v| v.parse().ok()), mm.attr("height").and_then(|v| v.parse().ok())) {
                        (Some(w), Some(h)) if w > 0 && h > 0 => (w, h),
                        _ => crate::logos::dims_from_name(url.rsplit('/').next().unwrap_or(url)).unwrap_or((0, 0)),
                    };
                    let lower = url.to_ascii_lowercase();
                    let mime = mm
                        .attr("mimeValue")
                        .map(|m| m.trim().to_ascii_lowercase())
                        .filter(|m| !m.is_empty())
                        .or_else(|| {
                            if lower.ends_with(".png") {
                                Some("image/png".into())
                            } else if lower.ends_with(".jpg") || lower.ends_with(".jpeg") {
                                Some("image/jpeg".into())
                            } else {
                                None
                            }
                        });
                    if let Some(mime) = mime {
                        s.logos.push(SiLogo { url: url.to_string(), width: w, height: h, mime });
                    }
                }
            }
            out.push(s);
            return;
        }
        for c in &e.children {
            walk(c, out);
        }
    }
    walk(root, &mut out);
    out
}

/// Dienst mit passendem Bearer (Vergleich ohne Gross/Klein).
pub fn find_service<'a>(services: &'a [SiService], bearer: &str) -> Option<&'a SiService> {
    services.iter().find(|s| s.bearers.iter().any(|b| b.eq_ignore_ascii_case(bearer)))
}

/// `<epg ...>` -> `<epg src="radiodns" ...>` (fuer den EPG-Cache: Herkunft).
pub fn mark_source(xml: &str) -> String {
    match xml.find("<epg") {
        Some(i) if xml[i + 4..].starts_with(|c: char| c.is_whitespace() || c == '>') => {
            format!("{}<epg src=\"{SRC_ATTR}\"{}", &xml[..i], &xml[i + 4..])
        }
        _ => xml.to_string(),
    }
}

// ---------------------------------------------------------------------------
// Auftrag / Ergebnis / Ausfuehrung (Worker-Seite, ohne App-Lock)
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct JobService {
    pub sid: u32,
    pub scids: u8,
    pub name: String,
    /// Logo-Groessen, die im Cache fehlen.
    pub logo_sizes: Vec<(u32, u32)>,
    /// Tage (jjjjmmtt), fuer die kein Sendeplan vorliegt.
    pub days: Vec<u32>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Job {
    pub eid: u16,
    pub ecc: u8,
    /// `data/radiodns/` (hosts.json, SI-Zwischendateien).
    pub cache_dir: PathBuf,
    pub services: Vec<JobService>,
}

#[derive(Clone, Debug, PartialEq, Eq, Default)]
pub struct JobResult {
    pub eid: u16,
    pub ecc: u8,
    /// Dienste des Auftrags mit RadioDNS-Angebot (SRV gefunden).
    pub services_found: u32,
    /// Dienste, die im SI.xml ihres Servers nicht vorkommen.
    pub services_unlisted: u32,
    /// (sid, Dateiname, Bytes)
    pub logos: Vec<(u32, String, Vec<u8>)>,
    /// (sid, Tag, XML mit `src="radiodns"`)
    pub schedules: Vec<(u32, u32, String)>,
    /// Tage ohne Sendeplan auf dem Server (404).
    pub schedules_missing: u32,
    pub errors: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct HostEntry {
    /// `None` = kein Angebot (NXDOMAIN/kein SRV) bzw. Fehler (`error`).
    pub endpoint: Option<Endpoint>,
    pub checked_unix: i64,
    #[serde(default)]
    pub error: bool,
}

/// `data/radiodns/hosts.json`: DNS-Ergebnisse je FQDN.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct HostCache {
    pub version: u32,
    pub entries: BTreeMap<String, HostEntry>,
}

impl HostCache {
    pub fn load(dir: &Path) -> Self {
        std::fs::read_to_string(dir.join(HOSTS_FILE)).ok().and_then(|s| serde_json::from_str(&s).ok()).unwrap_or_default()
    }
    pub fn save(&self, dir: &Path) {
        let me = HostCache { version: 1, entries: self.entries.clone() };
        if let Err(e) = crate::paths::write_atomic(&dir.join(HOSTS_FILE), serde_json::to_string_pretty(&me).unwrap_or_default()) {
            log::warn!("radiodns hosts.json: {e}");
        }
    }
    /// Gueltiger Eintrag (positiv 24 h, negativ 24 h, Fehler 1 h).
    fn valid(&self, fqdn: &str, now: i64) -> Option<&HostEntry> {
        let e = self.entries.get(fqdn)?;
        let ttl = if e.error { RESOLVE_ERROR_TTL_S } else { RESOLVE_TTL_S };
        (now - e.checked_unix < ttl && e.checked_unix <= now).then_some(e)
    }
}

fn push_error(res: &mut JobResult, msg: String) {
    log::warn!("radiodns: {msg}");
    if res.errors.len() < MAX_ERRORS {
        res.errors.push(msg);
    }
}

/// Dateiname im Logo-Cache: `<sid>_<Name>_<WxH>.<ext>` (v1-Schema, damit
/// `sid_from_name`/`dims_from_name` greifen). Name ohne Leer-/Sonderzeichen.
pub fn logo_file_name(sid: u32, name: &str, w: u32, h: u32, mime: &str) -> String {
    let mut n: String = name.chars().filter(|c| c.is_ascii_alphanumeric() || *c == '-').take(16).collect();
    if n.is_empty() {
        n = "logo".into();
    }
    let ext = if mime.contains("jpeg") || mime.contains("jpg") { "jpg" } else { "png" };
    format!("{:04X}_{n}_{w}x{h}.{ext}", sid)
}

/// SI.xml eines Servers: Zwischendatei bis [`SI_TTL_S`] alt, sonst neu laden
/// (bei Fehler die alte weiterverwenden).
fn load_si(f: &dyn Fetcher, ep: &Endpoint, dir: &Path, res: &mut JobResult) -> Option<Vec<SiService>> {
    let path = dir.join(format!("{}_SI.xml", ep.key()));
    let age_ok = path
        .metadata()
        .and_then(|m| m.modified())
        .ok()
        .and_then(|t| t.elapsed().ok())
        .map(|d| d.as_secs() < SI_TTL_S)
        .unwrap_or(false);
    if age_ok {
        if let Ok(xml) = std::fs::read_to_string(&path) {
            let s = parse_si(&xml);
            if !s.is_empty() {
                return Some(s);
            }
        }
    }
    let url = format!("{}{SI_PATH}", ep.base_url());
    match f.get(&url, MAX_XML_BYTES) {
        Ok(Some(bytes)) => {
            let xml = String::from_utf8_lossy(&bytes).into_owned();
            let s = parse_si(&xml);
            if s.is_empty() {
                push_error(res, format!("{url}: keine Dienste im SI.xml"));
            } else {
                let _ = std::fs::create_dir_all(dir);
                if let Err(e) = crate::paths::write_atomic(&path, xml.as_bytes()) {
                    log::warn!("radiodns SI-Cache {}: {e}", path.display());
                }
                return Some(s);
            }
        }
        Ok(None) => log::info!("radiodns: {url} nicht verfuegbar (401/403/404)"),
        Err(e) => push_error(res, format!("{url}: {e}")),
    }
    // Fallback: alte Datei
    std::fs::read_to_string(&path).ok().map(|xml| parse_si(&xml)).filter(|s| !s.is_empty())
}

/// Einen Auftrag ausfuehren (blockiert; Worker-Thread). Kein App-Zugriff.
pub fn run_job(f: &dyn Fetcher, job: &Job, now_unix: i64) -> JobResult {
    let mut res = JobResult { eid: job.eid, ecc: job.ecc, ..Default::default() };
    let _ = std::fs::create_dir_all(&job.cache_dir);
    let mut hosts = HostCache::load(&job.cache_dir);
    let mut hosts_changed = false;
    // Dienste je Server buendeln (Deutschlandradio: ein Server fuer alle).
    let mut by_endpoint: Vec<(Endpoint, Vec<&JobService>)> = Vec::new();
    for s in &job.services {
        let name = fqdn(job.ecc, job.eid, s.sid, s.scids);
        let entry = match hosts.valid(&name, now_unix) {
            Some(e) => e.clone(),
            None => {
                let e = match f.resolve(&name) {
                    Ok(ep) => HostEntry { endpoint: ep, checked_unix: now_unix, error: false },
                    Err(msg) => {
                        push_error(&mut res, format!("{name}: {msg}"));
                        HostEntry { endpoint: None, checked_unix: now_unix, error: true }
                    }
                };
                hosts.entries.insert(name.clone(), e.clone());
                hosts_changed = true;
                e
            }
        };
        if let Some(ep) = entry.endpoint {
            res.services_found += 1;
            match by_endpoint.iter_mut().find(|(e, _)| *e == ep) {
                Some((_, list)) => list.push(s),
                None => by_endpoint.push((ep, vec![s])),
            }
        }
    }
    if hosts_changed {
        hosts.save(&job.cache_dir);
    }
    for (ep, services) in by_endpoint {
        let Some(si) = load_si(f, &ep, &job.cache_dir, &mut res) else {
            res.services_unlisted += services.len() as u32;
            continue;
        };
        let base = ep.base_url();
        for s in services {
            let bearer = bearer_id(job.ecc, job.eid, s.sid, s.scids);
            let Some(svc) = find_service(&si, &bearer) else {
                res.services_unlisted += 1;
                log::info!("radiodns: {bearer} nicht im SI.xml von {base}");
                continue;
            };
            let display = if !svc.medium_name.trim().is_empty() {
                svc.medium_name.trim()
            } else if !svc.short_name.trim().is_empty() {
                svc.short_name.trim()
            } else {
                s.name.trim()
            };
            for &(w, h) in &s.logo_sizes {
                let Some(logo) = svc.logos.iter().find(|l| l.width == w && l.height == h && (l.mime.contains("png") || l.mime.contains("jpeg"))) else { continue };
                match f.get(&logo.url, MAX_LOGO_BYTES) {
                    Ok(Some(bytes)) if !bytes.is_empty() => {
                        res.logos.push((s.sid, logo_file_name(s.sid, display, w, h, &logo.mime), bytes));
                    }
                    Ok(_) => {}
                    Err(e) => push_error(&mut res, format!("{}: {e}", logo.url)),
                }
            }
            for &day in &s.days {
                let url = format!("{base}{}", pi_path(job.ecc, job.eid, s.sid, s.scids, day));
                match f.get(&url, MAX_XML_BYTES) {
                    Ok(Some(bytes)) => {
                        let xml = String::from_utf8_lossy(&bytes).into_owned();
                        if xml.contains("<epg") {
                            res.schedules.push((s.sid, day, mark_source(&xml)));
                        } else {
                            push_error(&mut res, format!("{url}: kein <epg>"));
                        }
                    }
                    Ok(None) => res.schedules_missing += 1,
                    Err(e) => push_error(&mut res, format!("{url}: {e}")),
                }
            }
        }
    }
    res
}

// ---------------------------------------------------------------------------
// Echtes Netz: hickory (System-Resolver) + ureq (schannel)
// ---------------------------------------------------------------------------

pub struct NetFetcher {
    resolver: hickory_resolver::Resolver,
    agent: ureq::Agent,
}

impl NetFetcher {
    pub fn new() -> Result<Self, String> {
        let resolver = Self::resolver()?;
        let tls = ureq::tls::TlsConfig::builder()
            .provider(ureq::tls::TlsProvider::NativeTls)
            .root_certs(ureq::tls::RootCerts::PlatformVerifier)
            .build();
        let agent: ureq::Agent = ureq::Agent::config_builder()
            .timeout_global(Some(Duration::from_secs(20)))
            .user_agent("DAB-Classic/3.0 (RadioDNS; +https://github.com/clrsrc/qt-dab-classic)")
            .tls_config(tls)
            .build()
            .into();
        Ok(Self { resolver, agent })
    }

    /// System-Resolver ohne die Windows-Platzhalter `fec0:0:0:ffff::1..3`
    /// (site-local, auf Adaptern ohne DNS eingetragen, antworten nie: jede
    /// Abfrage lief sonst in 5-s-Timeouts, gemessen 17.09.2026 20 s je
    /// Dienst) und ohne Duplikate. Ohne brauchbaren Eintrag: Cloudflare/
    /// Google (hickory-Standard) mit Log-Hinweis.
    fn resolver() -> Result<hickory_resolver::Resolver, String> {
        use hickory_resolver::config::ResolverConfig;
        let (sys, mut opts) = hickory_resolver::system_conf::read_system_conf().map_err(|e| format!("DNS-Konfiguration: {e}"))?;
        let mut cfg = ResolverConfig::new();
        let mut seen: Vec<(std::net::SocketAddr, hickory_resolver::config::Protocol)> = Vec::new();
        for ns in sys.name_servers() {
            let site_local = match ns.socket_addr.ip() {
                std::net::IpAddr::V6(v6) => (v6.segments()[0] & 0xffc0) == 0xfec0,
                std::net::IpAddr::V4(_) => false,
            };
            if site_local || seen.contains(&(ns.socket_addr, ns.protocol)) {
                continue;
            }
            seen.push((ns.socket_addr, ns.protocol));
            cfg.add_name_server(ns.clone());
        }
        let cfg = if cfg.name_servers().is_empty() {
            log::warn!("radiodns: kein System-Nameserver gefunden, nutze oeffentliche Resolver");
            ResolverConfig::default()
        } else {
            let mut ips: Vec<_> = cfg.name_servers().iter().map(|n| n.socket_addr.ip()).collect();
            ips.dedup();
            log::info!("radiodns: Nameserver {ips:?}");
            cfg
        };
        opts.timeout = Duration::from_secs(4);
        opts.attempts = 2;
        hickory_resolver::Resolver::new(cfg, opts).map_err(|e| format!("DNS-Resolver: {e}"))
    }

    /// `Name::from_ascii` statt `&str`: der `&str`-Weg schickt jedes Label
    /// durch IDNA/UTS46, und das lehnt z. B. "100d" (Schwarzwaldradio) mit
    /// "Label contains invalid characters" ab (Live 17.09.2026).
    fn name(s: &str) -> Result<hickory_resolver::Name, String> {
        hickory_resolver::Name::from_ascii(s).map_err(|e| format!("{s}: {e}"))
    }

    fn srv(&self, name: &str) -> Result<Option<Endpoint>, String> {
        use hickory_resolver::error::ResolveErrorKind;
        match self.resolver.srv_lookup(Self::name(name)?) {
            Ok(l) => {
                let best = l.iter().min_by_key(|r| (r.priority(), std::cmp::Reverse(r.weight())));
                Ok(best.map(|r| {
                    let host = r.target().to_utf8().trim_end_matches('.').to_string();
                    Endpoint { host, port: r.port(), https: r.port() == 443 }
                }))
            }
            Err(e) => match e.kind() {
                ResolveErrorKind::NoRecordsFound { .. } => Ok(None),
                _ => Err(e.to_string()),
            },
        }
    }
}

impl Fetcher for NetFetcher {
    fn resolve(&self, fqdn: &str) -> Result<Option<Endpoint>, String> {
        use hickory_resolver::error::ResolveErrorKind;
        use hickory_resolver::proto::op::ResponseCode;
        use hickory_resolver::proto::rr::{RData, RecordType};
        let target = match self.resolver.lookup(Self::name(fqdn)?, RecordType::CNAME) {
            Ok(l) => l
                .record_iter()
                .find_map(|r| match r.data() {
                    Some(RData::CNAME(c)) => Some(c.0.to_utf8().trim_end_matches('.').to_string()),
                    _ => None,
                })
                .unwrap_or_else(|| fqdn.to_string()),
            Err(e) => match e.kind() {
                ResolveErrorKind::NoRecordsFound { response_code, .. } => {
                    if *response_code == ResponseCode::NXDomain {
                        return Ok(None);
                    }
                    fqdn.to_string()
                }
                _ => return Err(e.to_string()),
            },
        };
        for svc in ["_radiospi._tcp.", "_radioepg._tcp."] {
            if let Some(ep) = self.srv(&format!("{svc}{target}"))? {
                return Ok(Some(ep));
            }
        }
        Ok(None)
    }

    fn get(&self, url: &str, max_bytes: u64) -> Result<Option<Vec<u8>>, String> {
        match self.agent.get(url).call() {
            Ok(mut resp) => resp.body_mut().with_config().limit(max_bytes).read_to_vec().map(Some).map_err(|e| e.to_string()),
            Err(ureq::Error::StatusCode(401 | 403 | 404 | 410)) => Ok(None),
            Err(e) => Err(e.to_string()),
        }
    }
}

// ---------------------------------------------------------------------------
// App-Seite: Zustand, Planung, Uebernahme
// ---------------------------------------------------------------------------

/// Fuer Frontend und `AppState::radiodns`.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq, Default)]
#[serde(default)]
pub struct RadioDnsStatus {
    pub enabled: bool,
    pub busy: bool,
    /// Ensemble des letzten Laufs.
    pub eid: u16,
    /// Dienste im Ensemble mit RadioDNS-Angebot / ohne.
    pub services_found: u32,
    pub services_none: u32,
    /// Seit dem Start uebernommene Logos und Sendeplaene.
    pub logos: u32,
    pub schedules: u32,
    pub last_unix: i64,
    pub error: Option<String>,
}

#[derive(Debug, Default)]
pub struct RadioDnsCtl {
    /// Letztes Ensemble-/Dienstereignis (Ruhezeit [`SETTLE`]).
    dirty: Option<Instant>,
    /// Ein Auftrag ist beim Worker.
    busy: bool,
    /// Zeitpunkt der letzten Pruefung je (EId, ECC).
    checked: HashMap<(u16, u8), Instant>,
    last_error: Option<Instant>,
}

impl App {
    /// Hook in `handle_event`.
    pub fn radiodns_on_event(&mut self, ev: &Event, now: Instant) -> Effects {
        match ev {
            Event::EnsembleFound { .. } | Event::EnsembleEcc { .. } | Event::ServiceAdded { .. } | Event::StateSnapshot { .. } => {
                if self.state.ensemble.is_some() && !self.state.scan.active {
                    self.radiodns.dirty = Some(now);
                }
            }
            Event::DeviceClosed | Event::Exiting { .. } | Event::ScanProgress { .. } => self.radiodns.dirty = None,
            _ => {}
        }
        Effects::default()
    }

    /// Hook in `update_settings`: Schalter umgelegt.
    pub fn radiodns_on_settings(&mut self, old: &Settings, now: Instant) -> Effects {
        if old.radiodns_enabled == self.settings.radiodns_enabled {
            return Effects::default();
        }
        self.state.radiodns.enabled = self.settings.radiodns_enabled;
        self.state.radiodns.error = None;
        if self.settings.radiodns_enabled {
            self.radiodns.checked.clear();
            self.radiodns.last_error = None;
            self.radiodns.dirty = Some(now);
        }
        self.radiodns_notify()
    }

    /// Manuell (Einstellungen "Jetzt abrufen"): Ruhezeit und Merker verwerfen.
    pub fn radiodns_refresh(&mut self, now: Instant) -> Effects {
        self.radiodns.checked.clear();
        self.radiodns.last_error = None;
        self.radiodns.dirty = Some(now - SETTLE);
        self.state.radiodns.error = None;
        self.radiodns_notify()
    }

    fn radiodns_notify(&self) -> Effects {
        let mut fx = Effects::default();
        fx.events.push(AppEvent::RadioDns { status: self.state.radiodns.clone() });
        fx
    }

    /// Vom Worker (alle paar Sekunden): naechster Auftrag, falls faellig.
    /// `None` = nichts zu tun. Setzt `busy`, bis [`radiodns_apply`](Self::radiodns_apply) kommt.
    pub fn radiodns_take_job(&mut self, now: Instant) -> Option<Job> {
        if !self.settings.radiodns_enabled || self.radiodns.busy || self.state.scan.active {
            return None;
        }
        let dirty = self.radiodns.dirty?;
        if now.saturating_duration_since(dirty) < SETTLE {
            return None;
        }
        if let Some(t) = self.radiodns.last_error {
            if now.saturating_duration_since(t) < RETRY_AFTER_ERROR {
                return None;
            }
        }
        let ens = self.state.ensemble.clone()?;
        if ens.ecc == 0 || self.state.services.is_empty() {
            return None;
        }
        let key = (ens.eid, ens.ecc);
        if let Some(t) = self.radiodns.checked.get(&key) {
            if now.saturating_duration_since(*t) < RECHECK_EVERY {
                self.radiodns.dirty = None;
                return None;
            }
        }
        let today = chrono::Local::now().date_naive();
        let days = [crate::epg::day_of(today), crate::epg::day_of(today + chrono::Duration::days(1))];
        let mut services = Vec::new();
        for s in self.state.services.iter().filter(|s| s.is_audio && s.is_primary) {
            let have = self.logos.sizes(ens.eid, s.sid);
            let logo_sizes: Vec<(u32, u32)> = LOGO_SIZES.iter().copied().filter(|sz| !have.contains(sz)).collect();
            let days_missing: Vec<u32> = days.iter().copied().filter(|d| !self.epg.has(ens.eid, s.sid, *d)).collect();
            if logo_sizes.is_empty() && days_missing.is_empty() {
                continue;
            }
            services.push(JobService { sid: s.sid, scids: s.scids, name: s.name.trim().to_string(), logo_sizes, days: days_missing });
        }
        self.radiodns.checked.insert(key, now);
        self.radiodns.dirty = None;
        if services.is_empty() {
            return None;
        }
        self.radiodns.busy = true;
        self.state.radiodns.busy = true;
        Some(Job { eid: ens.eid, ecc: ens.ecc, cache_dir: self.dirs.radiodns_dir(), services })
    }

    /// Ergebnis des Workers uebernehmen: Logos und Sendeplaene in die Caches,
    /// Presets/Display auffrischen, Status an das Frontend.
    pub fn radiodns_apply(&mut self, res: JobResult, now: Instant) -> Effects {
        self.radiodns.busy = false;
        let st = &mut self.state.radiodns;
        st.busy = false;
        st.eid = res.eid;
        // "mit Angebot" = per DNS gefunden UND im SI.xml gelistet.
        st.services_found = res.services_found.saturating_sub(res.services_unlisted);
        st.last_unix = crate::state::unix_now();
        st.error = res.errors.first().cloned();
        if !res.errors.is_empty() {
            self.radiodns.last_error = Some(now);
        }
        let mut fx = Effects::default();
        let mut touched_logo = false;
        for (sid, name, bytes) in &res.logos {
            if let Some((eid, sid)) = self.logos.store_bytes(res.eid, *sid, name, bytes) {
                self.state.radiodns.logos += 1;
                touched_logo = true;
                fx.events.push(AppEvent::LogoUpdated { eid, sid });
                fx.append(self.refresh_preset_logos(eid, sid));
            }
        }
        for (sid, day, xml) in &res.schedules {
            // Broadcast hat Vorrang: inzwischen empfangene Sendeplaene bleiben.
            if self.epg.has(res.eid, *sid, *day) && !self.epg.is_from_ip(res.eid, *sid, *day) {
                continue;
            }
            match self.epg.store_object(res.eid, *sid, *day, xml) {
                Ok(true) => {
                    self.state.radiodns.schedules += 1;
                    fx.events.push(AppEvent::EpgUpdated { eid: res.eid, sid: *sid, day: *day });
                }
                Ok(false) => {}
                Err(e) => log::warn!("radiodns PI {:04X}/{sid:04X}/{day}: {e}", res.eid),
            }
        }
        if touched_logo || !res.schedules.is_empty() {
            fx.append(self.media_refresh(true));
        }
        let n_services = self.state.services.iter().filter(|s| s.is_audio && s.is_primary).count() as u32;
        self.state.radiodns.services_none = n_services.saturating_sub(res.services_found);
        log::info!(
            "radiodns {:04X}: {} Dienste mit Angebot, {} Logos, {} Sendeplaene, {} Tage ohne, {} Fehler",
            res.eid,
            res.services_found,
            res.logos.len(),
            res.schedules.len(),
            res.schedules_missing,
            res.errors.len()
        );
        fx.append(self.radiodns_notify());
        fx
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::DataDirs;
    use std::cell::RefCell;

    fn data(name: &str) -> String {
        let p = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests").join("data").join("radiodns").join(name);
        std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("{}: {e}", p.display()))
    }

    #[test]
    fn names_follow_ts_103_270() {
        assert_eq!(gcc(0xE0, 0xD210), "de0");
        assert_eq!(gcc(0xE1, 0xC221), "ce1");
        assert_eq!(gcc(0x00, 0xE0D01006), "de0", "32-Bit-SId: ECC aus der SId");
        assert_eq!(fqdn(0xE0, 0x10BC, 0xD210, 0), "0.d210.10bc.de0.dab.radiodns.org");
        assert_eq!(fqdn(0xE0, 0x10FA, 0xE0D01006, 1), "1.e0d01006.10fa.de0.dab.radiodns.org");
        assert_eq!(bearer_id(0xE0, 0x10BC, 0xD210, 0), "dab:de0.10bc.d210.0");
        assert_eq!(pi_path(0xE0, 0x10BC, 0xD210, 0, 20260917), "/radiodns/spi/3.1/dab/de0/10bc/d210/0/20260917_PI.xml");
        assert_eq!(parse_bearer("dab:de0.10bc.d210.0"), Some(Bearer { gcc: "de0".into(), eid: 0x10BC, sid: 0xD210, scids: 0 }));
        assert_eq!(parse_bearer("fm:de.d210.10230"), None);
        assert_eq!(parse_bearer("https://x/y"), None);
        assert_eq!(Endpoint { host: "h".into(), port: 443, https: true }.base_url(), "https://h");
        assert_eq!(Endpoint { host: "h".into(), port: 8080, https: false }.base_url(), "http://h:8080");
        assert_eq!(logo_file_name(0xD210, "Dlf Kultur", 32, 32, "image/png"), "D210_DlfKultur_32x32.png");
        assert_eq!(crate::logos::sid_from_name(&logo_file_name(0xD210, "Dlf", 32, 32, "image/png")), Some(0xD210));
        assert_eq!(crate::logos::dims_from_name(&logo_file_name(0xD210, "Dlf", 320, 240, "image/jpeg")), Some((320, 240)));
    }

    #[test]
    fn si_dlf_parses_services_bearers_logos() {
        let si = parse_si(&data("SI_dlf.xml"));
        assert_eq!(si.len(), 4, "Dlf, Dlf Kultur, Dlf Nova, Dokumente und Debatten");
        let dlf = find_service(&si, "dab:de0.10bc.d210.0").expect("Dlf per Bearer");
        assert_eq!(dlf.medium_name, "Dlf");
        assert!(dlf.bearers.iter().any(|b| b.starts_with("https://")), "Stream-Bearer bleiben erhalten");
        let l32 = dlf.logos.iter().find(|l| l.width == 32 && l.height == 32).expect("32x32");
        assert!(l32.url.ends_with("/logos/dlf/Dlf_32x32.png"));
        assert_eq!(l32.mime, "image/png");
        assert!(dlf.logos.iter().any(|l| l.width == 320 && l.height == 240));
        assert!(find_service(&si, "dab:de0.10bc.ffff.0").is_none());
        assert!(find_service(&si, "DAB:DE0.10BC.D220.0").is_some(), "Vergleich ohne Gross/Klein");
    }

    #[test]
    fn pi_dlf_parses_with_offset_and_source_mark() {
        let xml = mark_source(&data("20260917_d210_PI.xml"));
        assert!(xml.contains("<epg src=\"radiodns\" xmlns="));
        let p = crate::epg::parse_epg_xml(0xD210, &xml).unwrap();
        assert!(p.from_ip);
        assert!(!p.legacy_time, "SPI-3.1-Datei mit xmlns zaehlt nicht als Altformat");
        assert_eq!(p.programmes.len(), 50);
        let first = &p.programmes[0];
        assert_eq!(first.medium_name, "Nachrichten");
        // 2026-09-17T00:00:00+02:00 -> lokale Zeit (MESZ) 00:00
        let expected = chrono::DateTime::parse_from_rfc3339("2026-09-17T00:00:00+02:00").unwrap().with_timezone(&chrono::Local).naive_local();
        assert_eq!(first.start_local, expected);
        assert_eq!(first.duration_min, 5);
        let long = p.programmes.iter().find(|p| p.long_name == "Deutschlandfunk Radionacht").unwrap();
        assert!(long.long_desc.contains("Radionacht Information"), "CDATA-Beschreibung");
        assert_eq!(long.duration_min, 4 * 60 + 55);
        assert_eq!(mark_source("<epgx/>"), "<epgx/>");
    }

    /// Attrappe: DNS-Tabelle + URL-Tabelle, protokolliert Aufrufe.
    struct Fake {
        dns: HashMap<String, Option<Endpoint>>,
        urls: HashMap<String, Vec<u8>>,
        calls: RefCell<Vec<String>>,
    }
    impl Fetcher for Fake {
        fn resolve(&self, fqdn: &str) -> Result<Option<Endpoint>, String> {
            self.calls.borrow_mut().push(format!("dns {fqdn}"));
            match self.dns.get(fqdn) {
                Some(e) => Ok(e.clone()),
                None => Err("kein Netz".into()),
            }
        }
        fn get(&self, url: &str, _max: u64) -> Result<Option<Vec<u8>>, String> {
            self.calls.borrow_mut().push(format!("get {url}"));
            Ok(self.urls.get(url).cloned())
        }
    }
    // RefCell ist nicht Sync, aber Fetcher verlangt nur Send.
    unsafe impl Send for Fake {}

    fn fake() -> Fake {
        let ep = Endpoint { host: "rdns.deutschlandradio.de".into(), port: 443, https: true };
        let mut dns = HashMap::new();
        dns.insert("0.d210.10bc.de0.dab.radiodns.org".to_string(), Some(ep.clone()));
        dns.insert("0.d220.10bc.de0.dab.radiodns.org".to_string(), Some(ep));
        // Absolut relax: SId 17FA -> Country-Id 1 (Deutschland hat 1 und D), gcc "1e0"
        dns.insert("0.17fa.10bc.1e0.dab.radiodns.org".to_string(), None);
        let mut urls = HashMap::new();
        urls.insert("https://rdns.deutschlandradio.de/radiodns/spi/3.1/SI.xml".to_string(), data("SI_dlf.xml").into_bytes());
        urls.insert("https://rdns.deutschlandradio.de/radiodns/spi/3.1/logos/dlf/Dlf_32x32.png".to_string(), b"\x89PNG32".to_vec());
        urls.insert("https://rdns.deutschlandradio.de/radiodns/spi/3.1/logos/dlf/Dlf_128x128.png".to_string(), b"\x89PNG128".to_vec());
        urls.insert("https://rdns.deutschlandradio.de/radiodns/spi/3.1/dab/de0/10bc/d210/0/20260917_PI.xml".to_string(), data("20260917_d210_PI.xml").into_bytes());
        Fake { dns, urls, calls: RefCell::new(Vec::new()) }
    }

    fn tmp(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("dabclassic-radiodns-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn run_job_resolves_groups_and_caches() {
        let dir = tmp("job");
        let f = fake();
        let job = Job {
            eid: 0x10BC,
            ecc: 0xE0,
            cache_dir: dir.clone(),
            services: vec![
                JobService { sid: 0xD210, scids: 0, name: "Dlf".into(), logo_sizes: vec![(32, 32), (128, 128), (112, 32)], days: vec![20260917, 20260918] },
                JobService { sid: 0xD220, scids: 0, name: "Dlf Kultur".into(), logo_sizes: vec![], days: vec![] },
                JobService { sid: 0x17FA, scids: 0, name: "Absolut relax".into(), logo_sizes: vec![(32, 32)], days: vec![20260917] },
            ],
        };
        let res = run_job(&f, &job, 1_800_000_000);
        assert_eq!(res.services_found, 2, "Dlf + Dlf Kultur; Absolut relax NXDOMAIN");
        assert_eq!(res.services_unlisted, 0);
        assert_eq!(res.logos.len(), 2, "32x32 und 128x128; 112x32 hat die Attrappe nicht");
        let names: Vec<&str> = res.logos.iter().map(|(_, n, _)| n.as_str()).collect();
        assert!(names.contains(&"D210_Dlf_32x32.png"), "{names:?}");
        assert!(names.contains(&"D210_Dlf_128x128.png"), "{names:?}");
        assert_eq!(res.schedules.len(), 1);
        assert_eq!(res.schedules[0].0, 0xD210);
        assert_eq!(res.schedules[0].1, 20260917);
        assert!(res.schedules[0].2.contains("src=\"radiodns\""));
        assert_eq!(res.schedules_missing, 1, "20260918 fehlt (404)");
        assert!(res.errors.is_empty(), "{:?}", res.errors);
        {
            let calls = f.calls.borrow();
            assert_eq!(calls.iter().filter(|c| c.starts_with("dns")).count(), 3);
            assert_eq!(calls.iter().filter(|c| c.ends_with("SI.xml")).count(), 1, "SI.xml einmal je Server");
        }
        // Zweiter Lauf: DNS aus hosts.json, SI aus der Zwischendatei.
        let hosts = HostCache::load(&dir);
        assert_eq!(hosts.entries.len(), 3);
        assert!(hosts.entries["0.17fa.10bc.1e0.dab.radiodns.org"].endpoint.is_none());
        f.calls.borrow_mut().clear();
        let res2 = run_job(&f, &job, 1_800_000_100);
        assert_eq!(res2.services_found, 2);
        let calls = f.calls.borrow();
        assert!(calls.iter().all(|c| !c.starts_with("dns")), "kein DNS mehr: {calls:?}");
        assert!(calls.iter().all(|c| !c.ends_with("SI.xml")), "kein SI.xml mehr: {calls:?}");
        drop(calls);
        // Abgelaufener Eintrag wird neu aufgeloest
        let res3 = run_job(&f, &job, 1_800_000_000 + RESOLVE_TTL_S + 1);
        assert_eq!(res3.services_found, 2);
        assert!(f.calls.borrow().iter().any(|c| c.starts_with("dns")));
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn run_job_reports_dns_errors_and_retries_sooner() {
        let dir = tmp("err");
        let f = Fake { dns: HashMap::new(), urls: HashMap::new(), calls: RefCell::new(Vec::new()) };
        let job = Job { eid: 0x10BC, ecc: 0xE0, cache_dir: dir.clone(), services: vec![JobService { sid: 0xD210, scids: 0, name: "Dlf".into(), logo_sizes: vec![(32, 32)], days: vec![] }] };
        let res = run_job(&f, &job, 1_000);
        assert_eq!(res.services_found, 0);
        assert_eq!(res.errors.len(), 1);
        assert!(res.errors[0].contains("kein Netz"));
        let hosts = HostCache::load(&dir);
        assert!(hosts.entries["0.d210.10bc.de0.dab.radiodns.org"].error);
        // innerhalb der Fehler-TTL kein neuer Versuch, danach schon
        f.calls.borrow_mut().clear();
        run_job(&f, &job, 1_000 + RESOLVE_ERROR_TTL_S - 1);
        assert!(f.calls.borrow().is_empty());
        run_job(&f, &job, 1_000 + RESOLVE_ERROR_TTL_S + 1);
        assert_eq!(f.calls.borrow().len(), 1);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    fn app(tag: &str) -> (App, PathBuf) {
        let dir = tmp(tag);
        let mut a = App::with(DataDirs::with_root(&dir, true), Settings::default(), crate::Presets::default());
        a.settings.radiodns_enabled = true;
        a.state.radiodns.enabled = true;
        (a, dir)
    }

    fn svc(sid: u32, name: &str) -> dab_api::ServiceInfo {
        dab_api::ServiceInfo { sid, scids: 0, name: name.into(), is_audio: true, is_primary: true, sub_ch: 1, bitrate_kbps: 96, pty: 0, short_name: String::new(), language: 0 }
    }

    #[test]
    fn app_plans_job_after_settle_and_applies_result() {
        let (mut a, dir) = app("app");
        let t0 = Instant::now();
        a.handle_event(&Event::EnsembleFound { eid: 0x10BC, name: "DR Deutschland".into(), channel: "5C".into(), ecc: 0 }, t0);
        a.handle_event(&Event::ServiceAdded { service: svc(0xD210, "Dlf") }, t0);
        a.handle_event(&Event::ServiceAdded { service: svc(0x17FA, "Absolut relax") }, t0);
        assert!(a.radiodns_take_job(t0 + SETTLE * 2).is_none(), "ohne ECC kein Auftrag");
        a.handle_event(&Event::EnsembleEcc { ecc: 0xE0 }, t0);
        assert_eq!(a.state.ensemble.as_ref().unwrap().ecc, 0xE0);
        assert!(a.radiodns_take_job(t0 + Duration::from_secs(1)).is_none(), "Ruhezeit");
        let job = a.radiodns_take_job(t0 + SETTLE + Duration::from_secs(1)).expect("Auftrag");
        assert_eq!(job.eid, 0x10BC);
        assert_eq!(job.ecc, 0xE0);
        assert_eq!(job.cache_dir, dir.join("radiodns"));
        assert_eq!(job.services.len(), 2);
        assert_eq!(job.services[0].logo_sizes, LOGO_SIZES.to_vec());
        assert_eq!(job.services[0].days.len(), 2);
        assert!(a.state.radiodns.busy);
        assert!(a.radiodns_take_job(t0 + SETTLE * 3).is_none(), "busy: kein zweiter Auftrag");
        let f = fake();
        let today = crate::epg::day_of(chrono::Local::now().date_naive());
        let mut res = run_job(&f, &job, 1_800_000_000);
        // Testdatei ist vom 17.09.2026 - fuer den Cache als "heute" ausgeben
        for s in res.schedules.iter_mut() {
            s.1 = today;
        }
        let fx = a.radiodns_apply(res, t0 + SETTLE * 2);
        assert!(!a.state.radiodns.busy);
        assert_eq!(a.state.radiodns.services_found, 1, "Dlf; Absolut relax = NXDOMAIN");
        assert_eq!(a.state.radiodns.services_none, 1);
        assert_eq!(a.state.radiodns.logos, 2);
        assert_eq!(a.state.radiodns.schedules, 1);
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::LogoUpdated { eid: 0x10BC, sid: 0xD210 })));
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::EpgUpdated { eid: 0x10BC, sid: 0xD210, .. })));
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::RadioDns { .. })));
        assert_eq!(a.logos.sizes(0x10BC, 0xD210).len(), 2);
        assert!(a.logos.data_url(0x10BC, 0xD210, crate::LogoSize::Small).is_some());
        assert!(a.epg.has(0x10BC, 0xD210, today));
        assert!(a.epg.is_from_ip(0x10BC, 0xD210, today));
        assert!(dir.join("epg").join("10BC").join(format!("{today}_D210_SI.xml")).is_file());
        // Broadcast hat Vorrang: derselbe Tag per epg_object ueberschreibt ...
        let bc = "<epg system=\"DAB\" tz=\"local\"><schedule><programme><mediumName>Broadcast</mediumName><location><time duration=\"PT05M\" time=\"2026-9-17T07:02\"/></location></programme></schedule></epg>";
        a.handle_event(&Event::EpgObject { eid: 0x10BC, sid: 0xD210, date_yyyymmdd: today, name: "x".into(), xml: bc.into() }, t0);
        assert!(!a.epg.is_from_ip(0x10BC, 0xD210, today));
        // ... und ein spaeteres Netz-Ergebnis fuer den Tag wird verworfen
        let res = JobResult { eid: 0x10BC, ecc: 0xE0, services_found: 1, schedules: vec![(0xD210, today, mark_source(&data("20260917_d210_PI.xml")))], ..Default::default() };
        a.radiodns_apply(res, t0);
        assert_eq!(a.epg.programmes(0x10BC, 0xD210, today).len(), 1, "Broadcast-Datei bleibt");
        assert_eq!(a.state.radiodns.schedules, 1);
        // Nach RECHECK_EVERY erneut, nur noch fehlende Groessen/Tage
        a.radiodns.dirty = Some(t0);
        assert!(a.radiodns_take_job(t0 + SETTLE * 2).is_none(), "innerhalb RECHECK_EVERY nichts");
        a.radiodns.checked.clear();
        a.radiodns.dirty = Some(t0);
        let job2 = a.radiodns_take_job(t0 + SETTLE * 2).expect("zweiter Auftrag");
        let dlf = job2.services.iter().find(|s| s.sid == 0xD210).unwrap();
        assert_eq!(dlf.logo_sizes, vec![(320, 240), (112, 32)]);
        assert_eq!(dlf.days.len(), 1, "nur morgen fehlt noch");
        std::fs::remove_dir_all(&dir).unwrap();
    }

    /// Echtes Netz (nur auf Zuruf: `cargo test -p dab-app live_ -- --ignored`).
    #[test]
    #[ignore]
    fn live_dlf_resolves_and_fetches() {
        let t = Instant::now();
        let f = NetFetcher::new().expect("Resolver/HTTP");
        println!("Resolver: {:?}", t.elapsed());
        let t = Instant::now();
        let ep = f.resolve(&fqdn(0xE0, 0x10BC, 0xD210, 0)).expect("DNS").expect("Deutschlandradio bietet RadioDNS");
        println!("Dlf -> {ep:?} ({:?})", t.elapsed());
        assert_eq!(ep.host, "rdns.deutschlandradio.de");
        // 17.09.2026: Absolut relax (17FA, gcc 1e0) -> radiodns.radio.cloud:80
        let t = Instant::now();
        println!("Absolut relax -> {:?} ({:?})", f.resolve(&fqdn(0xE0, 0x10BC, 0x17FA, 0)).expect("DNS"), t.elapsed());
        let t = Instant::now();
        let none = f.resolve(&fqdn(0xE0, 0x10BC, 0xFFFE, 0)).expect("DNS");
        println!("FFFE -> {none:?} ({:?})", t.elapsed());
        assert!(none.is_none(), "SId FFFE hat keinen Eintrag: {none:?}");
        let t = Instant::now();
        let si = f.get(&format!("{}{SI_PATH}", ep.base_url()), MAX_XML_BYTES).expect("HTTP").expect("200");
        println!("SI.xml GET: {:?}", t.elapsed());
        let services = parse_si(&String::from_utf8_lossy(&si));
        println!("SI.xml: {} Bytes, {} Dienste", si.len(), services.len());
        assert!(find_service(&services, "dab:de0.10bc.d210.0").is_some());
        let missing = f.get(&format!("{}{}", ep.base_url(), pi_path(0xE0, 0x10BC, 0xD210, 0, 19990101)), MAX_XML_BYTES).expect("HTTP");
        assert!(missing.is_none(), "404 -> None");
    }

    /// Diagnose: welche Nameserver hickory aus der Systemkonfiguration liest.
    #[test]
    #[ignore]
    fn live_dns_servers() {
        let (cfg, opts) = hickory_resolver::system_conf::read_system_conf().expect("system conf");
        for ns in cfg.name_servers() {
            println!("NS {:?} {:?} trust_negative={}", ns.socket_addr, ns.protocol, ns.trust_negative_responses);
        }
        println!("opts: timeout={:?} attempts={} concurrent={} strategy={:?} tcp={:?}", opts.timeout, opts.attempts, opts.num_concurrent_reqs, opts.ip_strategy, opts.try_tcp_on_error);
        for ns in cfg.name_servers() {
            let mut c = hickory_resolver::config::ResolverConfig::new();
            c.add_name_server(ns.clone());
            let r = hickory_resolver::Resolver::new(c, hickory_resolver::config::ResolverOpts::default()).unwrap();
            let t = Instant::now();
            let res = r.lookup("0.d210.10bc.de0.dab.radiodns.org", hickory_resolver::proto::rr::RecordType::CNAME);
            println!("  {:?}: {} in {:?}", ns.socket_addr, res.as_ref().map(|l| l.iter().count()).map_err(|e| e.to_string()).unwrap_or_else(|e| { println!("   err {e}"); 0 }), t.elapsed());
        }
    }

    #[test]
    fn app_switch_off_stops_jobs_and_error_backoff() {
        let (mut a, dir) = app("switch");
        let t0 = Instant::now();
        a.handle_event(&Event::EnsembleFound { eid: 0x10BC, name: "DR".into(), channel: "5C".into(), ecc: 0xE0 }, t0);
        a.handle_event(&Event::ServiceAdded { service: svc(0xD210, "Dlf") }, t0);
        let mut s = a.settings.clone();
        s.radiodns_enabled = false;
        let fx = a.update_settings(s);
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::RadioDns { status } if !status.enabled)));
        assert!(a.radiodns_take_job(t0 + SETTLE * 2).is_none());
        let mut s = a.settings.clone();
        s.radiodns_enabled = true;
        a.update_settings(s);
        let job = a.radiodns_take_job(t0 + SETTLE * 2).expect("nach Einschalten");
        let res = JobResult { eid: job.eid, ecc: job.ecc, errors: vec!["DNS kaputt".into()], ..Default::default() };
        a.radiodns_apply(res, t0 + SETTLE * 2);
        assert_eq!(a.state.radiodns.error.as_deref(), Some("DNS kaputt"));
        a.radiodns.checked.clear();
        a.radiodns.dirty = Some(t0);
        assert!(a.radiodns_take_job(t0 + SETTLE * 3).is_none(), "Fehler-Wartezeit");
        assert!(a.radiodns_take_job(t0 + SETTLE * 2 + RETRY_AFTER_ERROR + Duration::from_secs(1)).is_some());
        // Jetzt abrufen setzt alles zurueck
        a.radiodns.busy = false;
        a.state.radiodns.busy = false;
        a.radiodns_refresh(t0 + SETTLE * 4);
        assert!(a.state.radiodns.error.is_none());
        assert!(a.radiodns_take_job(t0 + SETTLE * 4).is_some());
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
