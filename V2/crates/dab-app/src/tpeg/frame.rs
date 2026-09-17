//! TPEG-Transportrahmen und Dienstrahmen (ISO 21219-5 SFW), wie sie ueber
//! DAB ankommen (ETSI TS 103 551): Der Kern liefert die Nutzdaten einer
//! MSC-Datengruppe (`tdc_group`); darin liegen ein oder mehrere komplette
//! Transportrahmen, dahinter ggf. Null-Padding.
//!
//! Aufbau (am Mitschnitt "ARD TPEG" 17.09.2026 nachvollzogen, Java-Referenz
//! `fenghlkevin/tpeg-item`):
//!
//! ```text
//! Transportrahmen: FF 0F | Laenge (2, Bytes nach dem Rahmentyp) | Kopf-CRC (2) | Rahmentyp (1) | Body
//!   Rahmentyp 0x01 = Dienstrahmen: SID-A/B/C (3) | Kennbyte (1) | Komponentenrahmen ...
//!                    ARD sendet die Komponentenrahmen zlib-gepackt (78 9C ...).
//!   Rahmentyp 0x00 = Stream-Directory (hier nicht ausgewertet)
//! Komponentenrahmen: SCID (1) | Feldlaenge (2) | Kopf-CRC (2) | Feld (Feldlaenge Bytes, endet mit CRC (2))
//!   SCID 0 = SNI (Dienst- und Netzinformation), andere SCIDs laut SNI-Tabelle (TEC, TFP ...)
//! ```
//!
//! CRCs werden nicht geprueft: Paket-CRC und Reed-Solomon-FEC des DAB-
//! Paketdienstes sichern die Daten bereits, der Kern verwirft fehlerhafte
//! Pakete (TS 103 551, 5.2).

use flate2::read::ZlibDecoder;
use std::io::Read;

pub const SYNC: [u8; 2] = [0xFF, 0x0F];
pub const FRAME_SERVICE: u8 = 0x01;
pub const FRAME_STREAM_DIRECTORY: u8 = 0x00;
/// Sicherheitsgrenze fuer entpackte Dienstrahmen (16 kbit/s-Dienst, ~8 KB je Datengruppe).
const MAX_INFLATED: usize = 1 << 20;

/// Ein Transportrahmen: Typ und Body (ohne Kopf).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TransportFrame<'a> {
    pub frame_type: u8,
    pub body: &'a [u8],
}

/// Ein Komponentenrahmen eines Dienstrahmens.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ComponentFrame {
    pub scid: u8,
    /// Feldinhalt nach dem Kopf-CRC, inklusive der abschliessenden 2 CRC-Bytes.
    pub field: Vec<u8>,
}

impl ComponentFrame {
    /// Feld ohne die 2 CRC-Bytes am Ende.
    pub fn payload(&self) -> &[u8] {
        let n = self.field.len().saturating_sub(2);
        &self.field[..n]
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ServiceFrame {
    /// TPEG-Dienstkennung SID-A.SID-B.SID-C (ARD: 22.98.211).
    pub sid: [u8; 3],
    /// Kennbyte hinter der SID (Verschluesselung/Kennung; ARD: 0x6B, Inhalt zlib).
    pub flags: u8,
    pub compressed: bool,
    pub components: Vec<ComponentFrame>,
}

/// Alle Transportrahmen einer Datengruppe; endet am Padding oder am ersten
/// Byte ohne Sync-Wort.
pub fn split_transport_frames(dg: &[u8]) -> Vec<TransportFrame<'_>> {
    let mut out = Vec::new();
    let mut p = 0usize;
    while p + 7 <= dg.len() {
        if dg[p..p + 2] != SYNC {
            break;
        }
        let len = u16::from_be_bytes([dg[p + 2], dg[p + 3]]) as usize;
        let frame_type = dg[p + 6];
        let start = p + 7;
        let end = start + len;
        if end > dg.len() {
            break;
        }
        out.push(TransportFrame { frame_type, body: &dg[start..end] });
        p = end;
    }
    out
}

/// Dienstrahmen (Rahmentyp 0x01) zerlegen; `None` bei zu kurzem Body.
pub fn parse_service_frame(body: &[u8]) -> Option<ServiceFrame> {
    if body.len() < 4 {
        return None;
    }
    let sid = [body[0], body[1], body[2]];
    let flags = body[3];
    let rest = &body[4..];
    let (data, compressed) = match inflate_if_zlib(rest) {
        Some(d) => (d, true),
        None => (rest.to_vec(), false),
    };
    Some(ServiceFrame { sid, flags, compressed, components: split_component_frames(&data) })
}

/// zlib-Strom erkennen (CMF/FLG-Pruefsumme, Deflate) und entpacken.
fn inflate_if_zlib(d: &[u8]) -> Option<Vec<u8>> {
    if d.len() < 2 || d[0] & 0x0F != 8 || (u16::from(d[0]) << 8 | u16::from(d[1])) % 31 != 0 {
        return None;
    }
    let mut out = Vec::new();
    let mut dec = ZlibDecoder::new(d).take(MAX_INFLATED as u64);
    dec.read_to_end(&mut out).ok()?;
    Some(out)
}

/// Komponentenrahmen: SCID (1) | Feldlaenge (2) | Kopf-CRC (2) | Feld.
pub fn split_component_frames(d: &[u8]) -> Vec<ComponentFrame> {
    let mut out = Vec::new();
    let mut p = 0usize;
    while p + 5 <= d.len() {
        let scid = d[p];
        let len = u16::from_be_bytes([d[p + 1], d[p + 2]]) as usize;
        let start = p + 5;
        let end = start + len;
        if end > d.len() {
            break;
        }
        out.push(ComponentFrame { scid, field: d[start..end].to_vec() });
        p = end;
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::write::ZlibEncoder;
    use flate2::Compression;
    use std::io::Write;

    fn frame(frame_type: u8, body: &[u8]) -> Vec<u8> {
        let mut v = vec![0xFF, 0x0F];
        v.extend_from_slice(&(body.len() as u16).to_be_bytes());
        v.extend_from_slice(&[0, 0]); // Kopf-CRC (nicht geprueft)
        v.push(frame_type);
        v.extend_from_slice(body);
        v
    }

    fn component_frame(scid: u8, payload: &[u8]) -> Vec<u8> {
        let mut v = vec![scid];
        v.extend_from_slice(&((payload.len() + 2) as u16).to_be_bytes());
        v.extend_from_slice(&[0, 0]);
        v.extend_from_slice(payload);
        v.extend_from_slice(&[0xAB, 0xCD]); // Feld-CRC
        v
    }

    #[test]
    fn transportrahmen_mit_padding() {
        let mut dg = frame(1, b"abc");
        dg.extend(frame(0, b"x"));
        dg.extend([0u8; 5]);
        let f = split_transport_frames(&dg);
        assert_eq!(f.len(), 2);
        assert_eq!(f[0].frame_type, 1);
        assert_eq!(f[0].body, b"abc");
        assert_eq!(f[1].body, b"x");
        // abgeschnittener Rahmen wird verworfen
        let mut cut = frame(1, b"abcdef");
        cut.truncate(cut.len() - 2);
        assert!(split_transport_frames(&cut).is_empty());
    }

    #[test]
    fn dienstrahmen_gepackt_und_roh() {
        let mut comps = component_frame(0, b"sni");
        comps.extend(component_frame(1, b"tec-data"));
        let mut z = ZlibEncoder::new(Vec::new(), Compression::default());
        z.write_all(&comps).unwrap();
        let packed = z.finish().unwrap();
        let mut body = vec![0x16, 0x62, 0xd3, 0x6b];
        body.extend_from_slice(&packed);
        let sf = parse_service_frame(&body).unwrap();
        assert_eq!(sf.sid, [0x16, 0x62, 0xd3]);
        assert!(sf.compressed);
        assert_eq!(sf.components.len(), 2);
        assert_eq!(sf.components[1].scid, 1);
        assert_eq!(sf.components[1].payload(), b"tec-data");

        let mut raw = vec![1, 2, 3, 0];
        raw.extend_from_slice(&comps);
        let sf = parse_service_frame(&raw).unwrap();
        assert!(!sf.compressed);
        assert_eq!(sf.components.len(), 2);
        assert!(parse_service_frame(&[1, 2]).is_none());
    }
}
