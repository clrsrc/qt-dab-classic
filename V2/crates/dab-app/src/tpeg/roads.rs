//! Strassenzuordnung fuer TPEG-Meldungen ohne Strassennamen: Die ARD sendet
//! nur OpenLR-Koordinaten. Aus einer eingebauten Tabelle der deutschen
//! Autobahnen und Anschlussstellen (OpenStreetMap, ODbL, erzeugt mit
//! `tools/build-autobahnen.py`, Quellenhinweis "(c) OpenStreetMap-Mitwirkende"
//! in App und README) wird die naechste Autobahn samt Anschlussstellen
//! bestimmt: "A 3 · AS Koeln-Muelheim -> AK Koeln-Ost". Kein Internet.
//!
//! Zuordnung: naechstes Autobahn-Teilstueck zum ersten Punkt der Meldung
//! (Gitterindex 0,02 Grad, Suchradius [`MAX_DIST_M`]); Anschlussstellen sind
//! die der gleichen Autobahn, die dem ersten bzw. letzten Punkt am naechsten
//! liegen. Die Tabelle wird beim ersten Zugriff aus dem eingebetteten
//! `data/autobahnen.bin` gelesen (Format siehe Python-Skript).

use std::collections::HashMap;
use std::sync::OnceLock;

/// Eingebettete Tabelle (leer = Zuordnung aus).
const DATA: &[u8] = include_bytes!("../../data/autobahnen.bin");
const MAGIC: &[u8; 8] = b"DABAUTO1";
/// Groesster Abstand Meldungspunkt - Autobahn, damit die Zuordnung gilt.
pub const MAX_DIST_M: f64 = 250.0;
/// Groesster Abstand zur Anschlussstelle, die genannt wird.
const MAX_JUNCTION_M: f64 = 20_000.0;
const CELL: f64 = 0.02;

#[derive(Clone, Debug, PartialEq, serde::Serialize, serde::Deserialize, Default)]
#[serde(default)]
pub struct RoadInfo {
    /// "A 3" (gemeinsame Abschnitte "A 3;A 4")
    pub road: String,
    /// Anschlussstelle am Anfang bzw. Ende ("Koeln-Muelheim", "Kreuz Koeln-Ost")
    pub from: Option<String>,
    pub to: Option<String>,
    /// Abstand des ersten Punkts zur Autobahn in Metern
    pub dist_m: u32,
}

struct Way {
    road: u16,
    pts: Vec<(i32, i32)>, // (lat_e6, lon_e6) in Fahrtrichtung
}

struct Junction {
    road: u16,
    lat: f64,
    lon: f64,
    name: String,
}

pub struct Roads {
    roads: Vec<String>,
    ways: Vec<Way>,
    junctions: Vec<Junction>,
    by_road: Vec<Vec<u32>>,
    grid: HashMap<(i32, i32), Vec<(u32, u16)>>,
    pub stamp: u32,
}

struct Cur<'a> {
    d: &'a [u8],
    p: usize,
}

impl<'a> Cur<'a> {
    fn take(&mut self, n: usize) -> Option<&'a [u8]> {
        let s = self.d.get(self.p..self.p + n)?;
        self.p += n;
        Some(s)
    }
    fn u8(&mut self) -> Option<u8> {
        self.take(1).map(|b| b[0])
    }
    fn u16(&mut self) -> Option<u16> {
        self.take(2).map(|b| u16::from_le_bytes([b[0], b[1]]))
    }
    fn u32(&mut self) -> Option<u32> {
        self.take(4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }
    fn i32(&mut self) -> Option<i32> {
        self.u32().map(|v| v as i32)
    }
    fn str8(&mut self) -> Option<String> {
        let n = self.u8()? as usize;
        Some(String::from_utf8_lossy(self.take(n)?).to_string())
    }
}

fn cell_of(lat: f64, lon: f64) -> (i32, i32) {
    ((lat / CELL).floor() as i32, (lon / CELL).floor() as i32)
}

impl Roads {
    pub fn parse(d: &[u8]) -> Option<Roads> {
        if d.len() < 8 || &d[..8] != MAGIC {
            return None;
        }
        let mut c = Cur { d, p: 8 };
        let n = c.u32()? as usize;
        let mut roads = Vec::with_capacity(n);
        for _ in 0..n {
            roads.push(c.str8()?);
        }
        let n = c.u32()? as usize;
        let mut ways = Vec::with_capacity(n);
        for _ in 0..n {
            let road = c.u16()?;
            let m = c.u16()? as usize;
            let mut pts = Vec::with_capacity(m);
            for _ in 0..m {
                let lat = c.i32()?;
                let lon = c.i32()?;
                pts.push((lat, lon));
            }
            ways.push(Way { road, pts });
        }
        let n = c.u32()? as usize;
        let mut junctions = Vec::with_capacity(n);
        let mut by_road: Vec<Vec<u32>> = vec![Vec::new(); roads.len()];
        for _ in 0..n {
            let road = c.u16()?;
            let lat = c.i32()? as f64 / 1e6;
            let lon = c.i32()? as f64 / 1e6;
            let name = c.str8()?;
            let _exit = c.str8()?;
            if let Some(v) = by_road.get_mut(road as usize) {
                v.push(junctions.len() as u32);
            }
            junctions.push(Junction { road, lat, lon, name });
        }
        let stamp = c.u32().unwrap_or(0);
        let mut grid: HashMap<(i32, i32), Vec<(u32, u16)>> = HashMap::new();
        for (wi, w) in ways.iter().enumerate() {
            for si in 0..w.pts.len().saturating_sub(1) {
                let a = w.pts[si];
                let b = w.pts[si + 1];
                let ca = cell_of(a.0 as f64 / 1e6, a.1 as f64 / 1e6);
                let cb = cell_of(b.0 as f64 / 1e6, b.1 as f64 / 1e6);
                grid.entry(ca).or_default().push((wi as u32, si as u16));
                if cb != ca {
                    grid.entry(cb).or_default().push((wi as u32, si as u16));
                }
            }
        }
        Some(Roads { roads, ways, junctions, by_road, grid, stamp })
    }

    /// Eingebettete Tabelle (einmal geparst); `None`, wenn keine eingebaut ist.
    pub fn builtin() -> Option<&'static Roads> {
        static R: OnceLock<Option<Roads>> = OnceLock::new();
        R.get_or_init(|| Roads::parse(DATA)).as_ref()
    }

    pub fn road_count(&self) -> usize {
        self.roads.len()
    }
    pub fn junction_count(&self) -> usize {
        self.junctions.len()
    }

    /// Naechstes Autobahn-Teilstueck zu (lon, lat): (Strassenindex, Abstand m).
    fn nearest_way(&self, lon: f64, lat: f64) -> Option<(u16, f64)> {
        let k = lat.to_radians().cos();
        let (cx, cy) = cell_of(lat, lon);
        let mut best: Option<(u16, f64)> = None;
        for dx in -1..=1 {
            for dy in -1..=1 {
                let Some(list) = self.grid.get(&(cx + dx, cy + dy)) else { continue };
                for &(wi, si) in list {
                    let w = &self.ways[wi as usize];
                    let a = w.pts[si as usize];
                    let b = w.pts[si as usize + 1];
                    let d = seg_dist_m(lon, lat, k, a, b);
                    if best.map_or(true, |(_, bd)| d < bd) {
                        best = Some((w.road, d));
                    }
                }
            }
        }
        best
    }

    fn nearest_junction(&self, road: u16, lon: f64, lat: f64, not: Option<&str>) -> Option<(&str, f64)> {
        let k = lat.to_radians().cos();
        let mut best: Option<(&str, f64)> = None;
        for &ji in self.by_road.get(road as usize)? {
            let j = &self.junctions[ji as usize];
            debug_assert_eq!(j.road, road, "by_road-Index passt nicht zur Anschlussstelle");
            // Zusammengelegte Knoten heissen "Dreieck X;Y": jede Teilbezeichnung zaehlt
            if not.is_some_and(|n| n.split(';').any(|a| j.name.split(';').any(|b| a.trim() == b.trim()))) {
                continue;
            }
            let d = ((j.lon - lon) * k).hypot(j.lat - lat) * 111_320.0;
            if d <= MAX_JUNCTION_M && best.map_or(true, |(_, bd)| d < bd) {
                best = Some((j.name.as_str(), d));
            }
        }
        best
    }

    /// Strasse und Anschlussstellen fuer eine Punktfolge (lon, lat) bestimmen;
    /// `max_dist_m` = zulaessiger Abstand zur Autobahn (Landstrassen laufen oft
    /// wenige hundert Meter parallel, daher fuer Nicht-Autobahn-Strassenarten klein).
    pub fn describe(&self, points: &[(f64, f64)], max_dist_m: f64) -> Option<RoadInfo> {
        let first = *points.first()?;
        let last = *points.last()?;
        let (road, dist) = self.nearest_way(first.0, first.1)?;
        if dist > max_dist_m {
            return None;
        }
        let from = self.nearest_junction(road, first.0, first.1, None).map(|(n, _)| n.to_string());
        let mut to = None;
        if points.len() > 1 && ((last.0 - first.0).abs() > 1e-5 || (last.1 - first.1).abs() > 1e-5) {
            to = self.nearest_junction(road, last.0, last.1, from.as_deref()).map(|(n, _)| n.to_string());
        }
        Some(RoadInfo { road: self.roads[road as usize].clone(), from, to, dist_m: dist.round() as u32 })
    }
}

/// Abstand (m) von (lon, lat) zum Teilstueck a-b (lat_e6, lon_e6); `k` = cos(lat).
fn seg_dist_m(lon: f64, lat: f64, k: f64, a: (i32, i32), b: (i32, i32)) -> f64 {
    let ax = (a.1 as f64 / 1e6 - lon) * k;
    let ay = a.0 as f64 / 1e6 - lat;
    let bx = (b.1 as f64 / 1e6 - lon) * k;
    let by = b.0 as f64 / 1e6 - lat;
    let dx = bx - ax;
    let dy = by - ay;
    let l2 = dx * dx + dy * dy;
    let t = if l2 == 0.0 { 0.0 } else { (-(ax * dx + ay * dy) / l2).clamp(0.0, 1.0) };
    (ax + t * dx).hypot(ay + t * dy) * 111_320.0
}

/// Strasse plus Anschlussstellen fuer eine Meldung; `fow` = OpenLR-Strassenart
/// (1 Autobahn, 6 Auf-/Abfahrt, 2 mehrbahnig: bis [`MAX_DIST_M`], sonst 40 m).
pub fn lookup(points: &[(f64, f64)], fow: Option<u8>) -> Option<RoadInfo> {
    let max = match fow {
        None | Some(0) | Some(1) | Some(2) | Some(6) => MAX_DIST_M,
        _ => 40.0,
    };
    Roads::builtin()?.describe(points, max)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> Vec<u8> {
        // Eine "A 1" von Koeln-Nord (50.99, 6.93) nach Norden mit zwei Anschlussstellen
        let mut v = Vec::new();
        v.extend_from_slice(MAGIC);
        v.extend_from_slice(&1u32.to_le_bytes());
        v.push(3);
        v.extend_from_slice(b"A 1");
        v.extend_from_slice(&1u32.to_le_bytes());
        v.extend_from_slice(&0u16.to_le_bytes());
        v.extend_from_slice(&3u16.to_le_bytes());
        for (lat, lon) in [(50.99, 6.93), (51.02, 6.94), (51.05, 6.95)] {
            v.extend_from_slice(&((lat * 1e6) as i32).to_le_bytes());
            v.extend_from_slice(&((lon * 1e6) as i32).to_le_bytes());
        }
        v.extend_from_slice(&2u32.to_le_bytes());
        for (lat, lon, name) in [(50.99, 6.93, "Koeln-Nord"), (51.05, 6.95, "Leverkusen-West")] {
            v.extend_from_slice(&0u16.to_le_bytes());
            v.extend_from_slice(&((lat * 1e6) as i32).to_le_bytes());
            v.extend_from_slice(&((lon * 1e6) as i32).to_le_bytes());
            v.push(name.len() as u8);
            v.extend_from_slice(name.as_bytes());
            v.push(0);
        }
        v.extend_from_slice(&20260901u32.to_le_bytes());
        v
    }

    #[test]
    fn zuordnung_auf_der_autobahn_und_daneben() {
        let r = Roads::parse(&sample()).unwrap();
        assert_eq!(r.road_count(), 1);
        assert_eq!(r.junction_count(), 2);
        assert_eq!(r.stamp, 20260901);
        // Punkt nahe der Linie zwischen den Stuetzpunkten
        let info = r.describe(&[(6.9351, 51.005), (6.949, 51.045)], MAX_DIST_M).unwrap();
        assert_eq!(info.road, "A 1");
        assert!(info.dist_m < 60, "{}", info.dist_m);
        assert_eq!(info.from.as_deref(), Some("Koeln-Nord"));
        assert_eq!(info.to.as_deref(), Some("Leverkusen-West"));
        // Punktmeldung: nur "from"
        let info = r.describe(&[(6.949, 51.045)], MAX_DIST_M).unwrap();
        assert_eq!(info.from.as_deref(), Some("Leverkusen-West"));
        assert!(info.to.is_none());
        // 2 km daneben: keine Zuordnung
        assert!(r.describe(&[(6.96, 51.0)], MAX_DIST_M).is_none());
        assert!(Roads::parse(b"nix").is_none());
    }

    #[test]
    fn eingebaute_tabelle_falls_vorhanden() {
        if let Some(r) = Roads::builtin() {
            assert!(r.road_count() > 50, "{} Autobahnen", r.road_count());
            assert!(r.junction_count() > 1000);
            // Koelner Ring: Kreuz Koeln-Ost liegt an A 3/A 4
            let info = r.describe(&[(7.0433, 50.9445)], MAX_DIST_M).expect("Kreuz Koeln-Ost");
            assert!(info.road.contains("A 3") || info.road.contains("A 4"), "{}", info.road);
        }
    }
}
