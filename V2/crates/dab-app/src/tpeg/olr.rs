//! Ortsreferenz-Container (ISO 21219-7 LRC) mit OpenLR (ISO 21219-22, OLR)
//! in der TPEG-Binaerform. Ohne Karte lassen sich daraus Koordinaten,
//! Strassenklasse (FRC), Strassenart (FOW), Richtung und Laenge gewinnen;
//! Strassennamen nicht (die ARD sendet keine Ortsbeschreibung mit).
//!
//! Komponenten-Kennungen (Java-Referenz, Mitschnitt ARD 17.09.2026):
//! LRC = 2; darin Methode 8 = OpenLR (2 = TMC, 1 = ULR ...). OpenLR:
//! Attribut Version (1), Kind 0 = Linie, 1 = Geokoordinate, 2 = Punkt an
//! Linie, 11 = Ortsbeschreibung; in der Linie Komponenten 9 = LineProperties,
//! 10 = PathProperties, 12 = Shape (Stuetzpunkte).
//!
//! Koordinaten: absolut 24 Bit vorzeichenbehaftet in 360/2^24 Grad (wie
//! OpenLR-Binaerformat), relativ 16 Bit in 1/100000 Grad zum Vorgaenger.

use super::ubcr::{Component, Reader};

pub const LRC_ID: u8 = 2;
pub const METHOD_OPENLR: u8 = 8;
pub const METHOD_TMC: u8 = 2;

#[derive(Clone, Debug, PartialEq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum LocationKind {
    Linear,
    Point,
    Geo,
    Tmc,
    Other,
}

#[derive(Clone, Debug, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct Location {
    pub kind: LocationKind,
    /// Stuetzpunkte (lon, lat) in Grad: Shape, sonst die Referenzpunkte.
    pub points: Vec<(f64, f64)>,
    /// Strassenklasse 0 (Autobahn) .. 7 und Strassenart (olr002) des ersten Punkts.
    pub frc: Option<u8>,
    pub fow: Option<u8>,
    /// Fahrtrichtung am ersten Punkt in Grad (0 = Nord).
    pub bearing_deg: Option<u16>,
    /// Summe der Teilstuecke (DNP) in Metern, bei Linien.
    pub length_m: Option<u32>,
    pub pos_offset_m: Option<u32>,
    pub neg_offset_m: Option<u32>,
    /// TMC-Ortsreferenz (Methode 2), falls gesendet: Ortscode, Tabellennummer,
    /// Ausdehnung in Ortscodes, Richtung negativ.
    pub tmc_code: Option<u16>,
    pub tmc_table: Option<u8>,
    pub tmc_extent: Option<u8>,
    pub tmc_negative: Option<bool>,
    pub description: Vec<String>,
}

impl Location {
    fn empty(kind: LocationKind) -> Self {
        Location {
            kind,
            points: Vec::new(),
            frc: None,
            fow: None,
            bearing_deg: None,
            length_m: None,
            pos_offset_m: None,
            neg_offset_m: None,
            tmc_code: None,
            tmc_table: None,
            tmc_extent: None,
            tmc_negative: None,
            description: Vec::new(),
        }
    }
    /// Erster Punkt (lon, lat).
    pub fn first(&self) -> Option<(f64, f64)> {
        self.points.first().copied()
    }
    /// Mittelpunkt der Stuetzpunkte (lon, lat) - fuer Entfernungen.
    pub fn center(&self) -> Option<(f64, f64)> {
        if self.points.is_empty() {
            return None;
        }
        let n = self.points.len() as f64;
        Some((self.points.iter().map(|p| p.0).sum::<f64>() / n, self.points.iter().map(|p| p.1).sum::<f64>() / n))
    }
}

fn abs_deg(v: i32) -> f64 {
    v as f64 * 360.0 / 16_777_216.0
}

/// Absolute Koordinate: lon (3), lat (3), Selektor (Hoehe -> ueberspringen).
fn abs_coord(r: &mut Reader) -> Option<(f64, f64)> {
    let lon = r.i24()?;
    let lat = r.i24()?;
    let sel = r.selector()?;
    if sel.has(0) {
        r.i16()?; // Hoehe (nicht benutzt)
    }
    Some((abs_deg(lon), abs_deg(lat)))
}

/// Relative Koordinate zum Vorgaenger: lon (2), lat (2) in 1/100000 Grad, Selektor.
fn rel_coord(r: &mut Reader, prev: (f64, f64)) -> Option<(f64, f64)> {
    let dlon = r.i16()? as f64 / 100_000.0;
    let dlat = r.i16()? as f64 / 100_000.0;
    let sel = r.selector()?;
    if sel.has(0) {
        r.i16()?;
    }
    Some((prev.0 + dlon, prev.1 + dlat))
}

#[derive(Default)]
struct LineProps {
    frc: Option<u8>,
    fow: Option<u8>,
    bearing: Option<u16>,
}

/// LineProperties (9): frc, fow, bearing (1 Byte, 360/256 Grad), Selektor.
fn line_props(c: &Component) -> LineProps {
    let mut a = c.attrs();
    let frc = a.u8();
    let fow = a.u8();
    let bearing = a.u8().map(|b| (b as u32 * 360 / 256) as u16);
    LineProps { frc, fow, bearing }
}

/// PathProperties (10): lfrcnp, dnp (IntUnLoMB, Meter), Selektor.
fn path_dnp(c: &Component) -> Option<u32> {
    let mut a = c.attrs();
    a.u8()?;
    a.int_unlomb()
}

/// Ein Referenzpunkt: Koordinate steht in den Attributen des Aufrufers,
/// LineProperties/PathProperties folgen als Komponenten *innerhalb der
/// Attributfolge* (so sendet es die ARD, so liest es die Java-Referenz).
fn lrp_components(r: &mut Reader, with_path: bool, props: &mut LineProps, len_sum: &mut u32) -> Option<()> {
    // LineProperties
    let lp = r.component()?;
    if lp.id == 9 {
        let p = line_props(&lp);
        if props.frc.is_none() {
            *props = p;
        }
    }
    if with_path {
        let pp = r.component()?;
        if pp.id == 10 {
            if let Some(d) = path_dnp(&pp) {
                *len_sum = len_sum.saturating_add(d);
            }
        }
    }
    Some(())
}

/// Lineare Ortsreferenz (Kind 0).
fn parse_linear(c: &Component) -> Option<Location> {
    let mut loc = Location::empty(LocationKind::Linear);
    let mut a = c.attrs();
    let mut props = LineProps::default();
    let mut len_sum = 0u32;
    // erster Punkt
    let first = abs_coord(&mut a)?;
    loc.points.push(first);
    lrp_components(&mut a, true, &mut props, &mut len_sum)?;
    // letzter Punkt (relativ zum ersten)
    let last = rel_coord(&mut a, first)?;
    lrp_components(&mut a, false, &mut props, &mut len_sum)?;
    let sel = a.selector()?;
    let mut prev = first;
    let mut mids = Vec::new();
    if sel.has(0) {
        let n = a.int_unlomb()? as usize;
        for _ in 0..n.min(64) {
            let p = rel_coord(&mut a, prev)?;
            lrp_components(&mut a, true, &mut props, &mut len_sum)?;
            mids.push(p);
            prev = p;
        }
    }
    if sel.has(1) {
        loc.pos_offset_m = a.int_unlomb();
    }
    if sel.has(2) {
        loc.neg_offset_m = a.int_unlomb();
    }
    // Zwischenpunkte sind relativ zum jeweiligen Vorgaenger, der letzte Punkt
    // relativ zum ersten (Java-Referenz: last vor intermediates gelesen).
    loc.points.extend(mids);
    loc.points.push(last);
    loc.frc = props.frc;
    loc.fow = props.fow;
    loc.bearing_deg = props.bearing;
    loc.length_m = Some(len_sum);
    // Shape (12): n, dann absolute Koordinaten - genauer als die Referenzpunkte
    for k in c.children().components() {
        if k.id == 12 {
            let mut s = k.attrs();
            if let Some(n) = s.int_unlomb() {
                let mut pts = Vec::new();
                for _ in 0..n.min(256) {
                    match abs_coord(&mut s) {
                        Some(p) => pts.push(p),
                        None => break,
                    }
                }
                if pts.len() >= 2 {
                    loc.points = pts;
                }
            }
        }
    }
    Some(loc)
}

/// Punkt an Linie (Kind 2): erster/letzter Punkt, Strassenseite, Orientierung, Selektor (Offset).
fn parse_point_along_line(c: &Component) -> Option<Location> {
    let mut loc = Location::empty(LocationKind::Point);
    let mut a = c.attrs();
    let mut props = LineProps::default();
    let mut len_sum = 0u32;
    let first = abs_coord(&mut a)?;
    lrp_components(&mut a, true, &mut props, &mut len_sum)?;
    let last = rel_coord(&mut a, first)?;
    lrp_components(&mut a, false, &mut props, &mut len_sum)?;
    let _side = a.u8();
    let _orientation = a.u8();
    if let Some(sel) = a.selector() {
        if sel.has(0) {
            loc.pos_offset_m = a.int_unlomb();
        }
    }
    loc.points.push(first);
    loc.points.push(last);
    loc.frc = props.frc;
    loc.fow = props.fow;
    loc.bearing_deg = props.bearing;
    loc.length_m = Some(len_sum);
    Some(loc)
}

fn parse_geo(c: &Component) -> Option<Location> {
    let mut loc = Location::empty(LocationKind::Geo);
    let mut a = c.attrs();
    loc.points.push(abs_coord(&mut a)?);
    Some(loc)
}

/// OpenLR-Methode (8): Version, dann Ortsreferenz + optionale Beschreibung.
fn parse_openlr(c: &Component) -> Option<Location> {
    let mut loc: Option<Location> = None;
    let mut description = Vec::new();
    for k in c.children().components() {
        match k.id {
            0 => loc = parse_linear(&k),
            1 => loc = parse_geo(&k),
            2 => loc = parse_point_along_line(&k),
            11 => {
                let mut a = k.attrs();
                if let Some(n) = a.u8() {
                    for _ in 0..n {
                        let _lang = a.u8();
                        let Some(len) = a.u16() else { break };
                        let Some(b) = a.bytes(len as usize) else { break };
                        description.push(String::from_utf8_lossy(b).to_string());
                    }
                }
            }
            _ => {}
        }
    }
    let mut loc = loc.or_else(|| Some(Location::empty(LocationKind::Other)))?;
    loc.description = description;
    Some(loc)
}

/// TMC-Methode (2, ISO 21219-? TLR): locationID (2) | Laendercode (1) |
/// Tabellennummer (1) | Selektor [0 Richtung] [1 beide Richtungen] [2 Ausdehnung (1)]
/// [3 erweiterter Laendercode (1)] [4 Tabellenversion (IntUnLoMB)] [5 Praezisierung].
/// Ohne TMC-Ortstabelle nicht aufloesbar; Code und Ausdehnung werden gemerkt.
fn parse_tmc(c: &Component, loc: &mut Location) {
    let mut a = c.attrs();
    loc.tmc_code = a.u16();
    let _country = a.u8();
    loc.tmc_table = a.u8();
    if let Some(sel) = a.selector() {
        loc.tmc_negative = Some(sel.has(0));
        if sel.has(2) {
            loc.tmc_extent = a.u8();
        }
    }
}

/// Ortsreferenz-Container (Komponente 2) auswerten: die ARD sendet meist TMC
/// *und* OpenLR; OpenLR liefert die Koordinaten, der TMC-Code kommt dazu.
pub fn parse_lrc(c: &Component) -> Option<Location> {
    let methods = c.children().components();
    let mut loc = methods.iter().find(|m| m.id == METHOD_OPENLR).and_then(parse_openlr);
    if let Some(tmc) = methods.iter().find(|m| m.id == METHOD_TMC) {
        let mut l = loc.take().unwrap_or_else(|| Location::empty(LocationKind::Tmc));
        parse_tmc(tmc, &mut l);
        loc = Some(l);
    }
    loc
}

/// Entfernung (km) und Richtung (Grad) von `from` (lon, lat) nach `to`.
pub fn distance_bearing(from: (f64, f64), to: (f64, f64)) -> (f64, u16) {
    let (lon1, lat1) = (from.0.to_radians(), from.1.to_radians());
    let (lon2, lat2) = (to.0.to_radians(), to.1.to_radians());
    let dlat = lat2 - lat1;
    let dlon = lon2 - lon1;
    let a = (dlat / 2.0).sin().powi(2) + lat1.cos() * lat2.cos() * (dlon / 2.0).sin().powi(2);
    let d = 2.0 * 6371.0 * a.sqrt().asin();
    let y = dlon.sin() * lat2.cos();
    let x = lat1.cos() * lat2.sin() - lat1.sin() * lat2.cos() * dlon.cos();
    let b = (y.atan2(x).to_degrees() + 360.0) % 360.0;
    (d, b.round() as u16 % 360)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tpeg::sni::tests::unhex;

    /// LRC der ersten TEC-Meldung des Mitschnitts (62 Byte + Kopf "02 3e 00").
    const LRC_MSG0: &str = "023e00083b01100037240 8d41324 8ceb0009050402065c000a0504028522000253fe63000905040206040030 00000c100f0208d413248ceb0008d529248c2b00";

    #[test]
    fn openlr_linie_aus_dem_mitschnitt() {
        let raw = unhex(&LRC_MSG0.replace(' ', ""));
        let mut r = Reader::new(&raw);
        let c = r.component().unwrap();
        assert_eq!(c.id, LRC_ID);
        let loc = parse_lrc(&c).unwrap();
        assert_eq!(loc.kind, LocationKind::Linear);
        // Shape mit 2 Punkten ersetzt die Referenzpunkte
        assert_eq!(loc.points.len(), 2);
        let (lon, lat) = loc.points[0];
        assert!((lat - 51.399).abs() < 0.01, "lat {lat}");
        assert!((lon - 12.415).abs() < 0.01, "lon {lon}");
        assert_eq!(loc.frc, Some(2));
        assert_eq!(loc.fow, Some(6));
        assert_eq!(loc.bearing_deg, Some(129));
        assert_eq!(loc.length_m, Some(674));
        assert_eq!(loc.pos_offset_m, Some(0));
        assert_eq!(loc.neg_offset_m, Some(0));
    }

    /// LRC mit TMC- und OpenLR-Methode (zweite Meldung des Mitschnitts).
    const LRC_MSG1: &str = "024d00020d0c2824 0d011e02e0960028 0a0a083b0110003724097665 25619f0009050400 01ec000a05040088 0200fdf7034e0009 050400015c003000 000c100f02097665 25619f0009757325 632a00";

    #[test]
    fn tmc_und_openlr_gemeinsam() {
        let raw = unhex(&LRC_MSG1.replace(' ', ""));
        let c = Reader::new(&raw).component().unwrap();
        let loc = parse_lrc(&c).unwrap();
        assert_eq!(loc.kind, LocationKind::Linear);
        assert_eq!(loc.tmc_code, Some(0x2824));
        assert_eq!(loc.tmc_table, Some(1));
        assert_eq!(loc.tmc_extent, Some(2));
        assert_eq!(loc.tmc_negative, Some(false));
        assert_eq!(loc.points.len(), 2);
        // Berlin, A100/A111-Bereich
        assert!((loc.points[0].1 - 52.567).abs() < 0.01 && (loc.points[0].0 - 13.307).abs() < 0.01, "{:?}", loc.points);
        assert_eq!(loc.frc, Some(0));
    }

    #[test]
    fn entfernung_und_richtung() {
        let (d, b) = distance_bearing((6.96, 50.94), (13.40, 52.52)); // Koeln -> Berlin
        assert!((d - 477.0).abs() < 5.0, "{d}");
        assert!((60..=75).contains(&b), "{b}");
    }
}
