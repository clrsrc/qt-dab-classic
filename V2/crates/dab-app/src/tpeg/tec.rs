//! TPEG2-TEC: Verkehrsereignisse (ISO 21219-15) mit Nachrichtenverwaltung
//! (ISO 21219-6 MMC) und Ortsreferenz (siehe `olr`).
//!
//! Komponenten (Java-Referenz `fenghlkevin/tpeg-item`, Mitschnitt ARD TPEG
//! 17.09.2026, TEC 3.2):
//!
//! ```text
//! TEC-Feld: Gruppenprioritaet (1) | Anzahl Meldungen (1) | Meldungen | CRC (2)
//! Meldung   = Komponente 0 (TECMessage), keine Attribute, Kinder:
//!   1 MMC   : messageID (IntUnLoMB) | versionID (1) | Ablauf (DateTime) | Selektor
//!             [0 cancelFlag] [1 Erzeugungszeit (DateTime)] [2 Prioritaet (1)]
//!   3 Event : effectCode (1) | Selektor [0 Beginn] [1 Ende] [2 Tendenz (1)]
//!             [3 Laenge m (IntUnLoMB)] [4 Durchschnittsgeschw. m/s (1)] [5 Verzoegerung min (IntUnLoMB)]
//!             [6 Tempolimit m/s (1)] [7 erwartete Geschw. (1)] [8 Anschlussstellen (1)]
//!             Kinder: 4 Cause, 6 Advice, weitere (Fahrzeugbeschraenkung, Umleitung, Tempolimit) uebersprungen
//!   4 Cause : mainCause (1) | warningLevel (1) | Selektor [0 unbestaetigt] [1 Unterursache (1)]
//!             [2 Laenge m] [3 Spurbeschraenkung (1)] [4 Spuren (IntUnLoMB)] [5 Freitext n + LocalisedShortString]
//!             [6 Versatz m]
//!   6 Advice: Selektor [0 adviceCode (1)] [1 Unterhinweis (1)] [2 Freitext n + LocalisedShortString]
//!   2 LRC   : Ortsreferenz (olr.rs)
//! ```

use super::olr::{self, Location};
use super::ubcr::{Component, Reader};
use serde::{Deserialize, Serialize};

pub const COMP_MESSAGE: u8 = 0;
pub const COMP_MMC: u8 = 1;
pub const COMP_LRC: u8 = 2;
pub const COMP_EVENT: u8 = 3;
pub const COMP_CAUSE: u8 = 4;
pub const COMP_ADVICE: u8 = 6;

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct TecCause {
    pub main: u8,
    pub warning_level: u8,
    pub unverified: bool,
    pub sub: Option<u8>,
    pub length_m: Option<u32>,
    pub lane_restriction: Option<u8>,
    pub lanes: Option<u32>,
    pub offset_m: Option<u32>,
    pub text: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct TecAdvice {
    pub code: Option<u8>,
    pub sub: Option<u8>,
    pub text: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct TecEvent {
    /// tec001: 1 unbekannt, 2 frei, 3 dicht, 4 zaehfluessig, 5 stockend, 6 Stau, 7 kein Verkehr (gesperrt)
    pub effect: u8,
    pub start_unix: Option<u32>,
    pub stop_unix: Option<u32>,
    pub tendency: Option<u8>,
    pub length_m: Option<u32>,
    /// Durchschnittsgeschwindigkeit in m/s (wie gesendet)
    pub avg_speed_ms: Option<u8>,
    pub delay_min: Option<u32>,
    pub speed_limit_ms: Option<u8>,
    pub expected_speed_ms: Option<u8>,
    pub junction_closure: Option<u8>,
    pub causes: Vec<TecCause>,
    pub advices: Vec<TecAdvice>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct TecMessage {
    pub message_id: u32,
    pub version: u8,
    pub expiry_unix: u32,
    pub cancel: bool,
    pub generation_unix: Option<u32>,
    pub priority: Option<u8>,
    pub event: Option<TecEvent>,
    pub location: Option<Location>,
}

fn parse_mmc(c: &Component, m: &mut TecMessage) -> Option<()> {
    let mut a = c.attrs();
    m.message_id = a.int_unlomb()?;
    m.version = a.u8()?;
    m.expiry_unix = a.u32()?;
    let sel = a.selector()?;
    m.cancel = sel.has(0);
    if sel.has(1) {
        m.generation_unix = a.u32();
    }
    if sel.has(2) {
        m.priority = a.u8();
    }
    Some(())
}

fn free_text(a: &mut Reader) -> Vec<String> {
    let mut out = Vec::new();
    if let Some(n) = a.u8() {
        for _ in 0..n {
            match a.localised_short_string() {
                Some((_, s)) if !s.is_empty() => out.push(s),
                Some(_) => {}
                None => break,
            }
        }
    }
    out
}

fn parse_cause(c: &Component) -> Option<TecCause> {
    let mut a = c.attrs();
    let mut cause = TecCause { main: a.u8()?, warning_level: a.u8()?, ..Default::default() };
    let Some(sel) = a.selector() else { return Some(cause) };
    cause.unverified = sel.has(0);
    // Ab hier tolerant: was fehlt, bleibt None.
    let _ = (|| -> Option<()> {
        if sel.has(1) {
            cause.sub = Some(a.u8()?);
        }
        if sel.has(2) {
            cause.length_m = Some(a.int_unlomb()?);
        }
        if sel.has(3) {
            cause.lane_restriction = Some(a.u8()?);
        }
        if sel.has(4) {
            cause.lanes = Some(a.int_unlomb()?);
        }
        if sel.has(5) {
            cause.text = free_text(&mut a);
        }
        if sel.has(6) {
            cause.offset_m = Some(a.int_unlomb()?);
        }
        Some(())
    })();
    Some(cause)
}

fn parse_advice(c: &Component) -> Option<TecAdvice> {
    let mut a = c.attrs();
    let sel = a.selector()?;
    let mut adv = TecAdvice::default();
    let _ = (|| -> Option<()> {
        if sel.has(0) {
            adv.code = Some(a.u8()?);
        }
        if sel.has(1) {
            adv.sub = Some(a.u8()?);
        }
        if sel.has(2) {
            adv.text = free_text(&mut a);
        }
        Some(())
    })();
    Some(adv)
}

fn parse_event(c: &Component) -> Option<TecEvent> {
    let mut a = c.attrs();
    let mut ev = TecEvent { effect: a.u8()?, ..Default::default() };
    if let Some(sel) = a.selector() {
        let _ = (|| -> Option<()> {
            if sel.has(0) {
                ev.start_unix = Some(a.u32()?);
            }
            if sel.has(1) {
                ev.stop_unix = Some(a.u32()?);
            }
            if sel.has(2) {
                ev.tendency = Some(a.u8()?);
            }
            if sel.has(3) {
                ev.length_m = Some(a.int_unlomb()?);
            }
            if sel.has(4) {
                ev.avg_speed_ms = Some(a.u8()?);
            }
            if sel.has(5) {
                ev.delay_min = Some(a.int_unlomb()?);
            }
            if sel.has(6) {
                ev.speed_limit_ms = Some(a.u8()?);
            }
            if sel.has(7) {
                ev.expected_speed_ms = Some(a.u8()?);
            }
            if sel.has(8) {
                ev.junction_closure = Some(a.u8()?);
            }
            Some(())
        })();
    }
    for k in c.children().components() {
        match k.id {
            COMP_CAUSE => {
                if let Some(x) = parse_cause(&k) {
                    ev.causes.push(x);
                }
            }
            COMP_ADVICE => {
                if let Some(x) = parse_advice(&k) {
                    ev.advices.push(x);
                }
            }
            _ => {}
        }
    }
    Some(ev)
}

/// Eine TECMessage-Komponente (id 0) auswerten; `None` ohne gueltigen MMC.
pub fn parse_message(c: &Component) -> Option<TecMessage> {
    if c.id != COMP_MESSAGE {
        return None;
    }
    let mut m = TecMessage::default();
    let mut have_mmc = false;
    for k in c.children().components() {
        match k.id {
            COMP_MMC => have_mmc = parse_mmc(&k, &mut m).is_some(),
            COMP_EVENT => m.event = parse_event(&k),
            COMP_LRC => m.location = olr::parse_lrc(&k),
            _ => {}
        }
    }
    have_mmc.then_some(m)
}

/// TEC-Komponentenrahmen-Feld (ohne die 2 CRC-Bytes) in Meldungen zerlegen.
/// Gibt zusaetzlich die angekuendigte Anzahl zurueck (Diagnose).
pub fn parse_tec_field(payload: &[u8]) -> (u8, Vec<TecMessage>) {
    let mut r = Reader::new(payload);
    let Some(_priority) = r.u8() else { return (0, Vec::new()) };
    let Some(count) = r.u8() else { return (0, Vec::new()) };
    let mut out = Vec::new();
    for _ in 0..count {
        let Some(c) = r.component() else { break };
        if let Some(m) = parse_message(&c) {
            out.push(m);
        }
    }
    (count, out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tpeg::sni::tests::unhex;

    /// Erste TEC-Meldung des Mitschnitts (97 Byte Body hinter "00 61").
    const MSG0: &str = "0061 00 010b0a84e652026aaba2b51001 03110a07606ab219606abe8380 040403030200 023e00083b011000372408d413248ceb0009050402065c000a0504028522000253fe63000905040206040030000 00c100f0208d413248ceb0008d529248c2b00";
    /// Meldung mit Ende-Zeit, Spurangabe und Hinweis (msg 3 des Mitschnitts).
    const MSG3: &str = "0062 00 010a098b4e006aaba2b51001 031306 07206ae22a10 040403030200 06040360 0801 023e00083b011000372405b41325388700090504010 62c000a05040182420 0ffe6ffcc00090504010 66c00300000 0c100f0205b413253887000 5b40725386f00";

    fn field(msgs: &[&str]) -> Vec<u8> {
        let mut v = vec![0x00, msgs.len() as u8];
        for m in msgs {
            v.extend(unhex(&m.replace(' ', "")));
        }
        v
    }

    #[test]
    fn meldung_mit_bauarbeiten_und_ort() {
        let (count, msgs) = parse_tec_field(&field(&[MSG0]));
        assert_eq!(count, 1);
        assert_eq!(msgs.len(), 1);
        let m = &msgs[0];
        assert_eq!(m.message_id, 78674);
        assert_eq!(m.version, 2);
        assert_eq!(m.expiry_unix, 0x6aaba2b5);
        assert!(!m.cancel);
        assert_eq!(m.priority, Some(1));
        assert_eq!(m.generation_unix, None);
        let ev = m.event.as_ref().unwrap();
        assert_eq!(ev.effect, 7);
        assert_eq!(ev.start_unix, Some(0x6ab21960));
        assert_eq!(ev.stop_unix, Some(0x6abe8380));
        assert_eq!(ev.causes.len(), 1);
        assert_eq!((ev.causes[0].main, ev.causes[0].warning_level), (3, 2));
        assert!(ev.advices.is_empty());
        let loc = m.location.as_ref().unwrap();
        assert_eq!(loc.length_m, Some(674));
        assert!((loc.points[0].1 - 51.399).abs() < 0.01);
    }

    #[test]
    fn meldung_mit_hinweis_und_zwei_meldungen_im_feld() {
        let (count, msgs) = parse_tec_field(&field(&[MSG0, MSG3]));
        assert_eq!(count, 2);
        assert_eq!(msgs.len(), 2);
        let m = &msgs[1];
        assert_eq!(m.message_id, (0x0b << 7) | 0x4e);
        let ev = m.event.as_ref().unwrap();
        assert_eq!(ev.effect, 7);
        assert_eq!(ev.start_unix, None);
        assert_eq!(ev.stop_unix, Some(0x6ae22a10));
        assert_eq!(ev.advices.len(), 1);
        assert_eq!((ev.advices[0].code, ev.advices[0].sub), (Some(8), Some(1)));
        let loc = m.location.as_ref().unwrap();
        assert_eq!(loc.frc, Some(1));
        assert!((loc.points[0].0 - 8.05).abs() < 0.05, "{:?}", loc.points);
    }

    #[test]
    fn selektor_ordnung_der_ursache() {
        // Cause mit Spurbeschraenkung 4 und 1 Spur (Selektor 0x0c = Elemente 3, 4)
        let raw = unhex("0406050302 0c0401".replace(' ', "").as_str());
        let c = Reader::new(&raw).component().unwrap();
        let cause = parse_cause(&c).unwrap();
        assert_eq!(cause.lane_restriction, Some(4));
        assert_eq!(cause.lanes, Some(1));
        assert!(!cause.unverified);
        // Unterursache 1 (Selektor 0x20 = Element 1)
        let raw = unhex("040504030220 01".replace(' ', "").as_str());
        let c = Reader::new(&raw).component().unwrap();
        assert_eq!(parse_cause(&c).unwrap().sub, Some(1));
    }
}
