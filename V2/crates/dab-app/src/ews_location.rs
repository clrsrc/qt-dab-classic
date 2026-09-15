//! Uebersetzung der DAB-EWS-Ortscodes (ETSI TS 104 089 Annex F) in WGS84-
//! Koordinaten. Es gibt dafuer **keine Namenstabelle** im ueblichen Sinn: die
//! Codes sind ein Quadtree ueber der Erdkugel (6-Bit-Zone + bis zu sechs
//! 4-Bit-Unterteilungsstufen, je Stufe eine 4x4-Kachelung von jeweils 36
//! bzw. 4/4^n Grad Kantenlaenge), keine benannten Verwaltungsregionen. Sie
//! sind fuer den *automatischen* Abgleich "betrifft mich das?" gedacht
//! (Klausel 7.5/7.6 der Norm), nicht zum Vorlesen.
//!
//! [`decode`] rechnet einen Code auf Mittelpunkt + Naeherungsradius zurueck
//! (Herleitung als Umkehrung der Kodiervorschrift in Annex F.3-F.5, geprueft
//! an den beiden Rechenbeispielen der Norm, siehe Tests). Zusammen mit den
//! Heimatkoordinaten (Entscheidung 25) ergibt das Entfernung/Richtung, genau
//! wie bei der TII-Anzeige ([`crate::tii`]).
//!
//! Code-Format (aus dem Kern, `fibDecoder::readLocationCode`):
//! `Z<Zone>:<Ziffern>[+<16-Bit-Subcode-Hex>]`, z. B. `Z1:5C+F300`. Die
//! Subcode-Bitmaske (welche von bis zu 16 Enkel-Kacheln zum Alarmgebiet
//! gehoeren) wird beim Uebersetzen ignoriert: das Ergebnis ist die groebere
//! Elternflaeche, nie feiner als der Code selbst - fuer "wie weit ist das
//! von mir weg" reicht das.

use serde::{Deserialize, Serialize};

/// Ergebnis der Uebersetzung eines einzelnen Ortscodes.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
#[serde(default)]
pub struct LocationInfo {
    pub code: String,
    /// Naeherungsweiser Mittelpunkt der Flaeche; `None`, wenn der Code nicht
    /// geparst werden konnte (unbekanntes Format oder Zone).
    pub lat: Option<f64>,
    pub lon: Option<f64>,
    /// Naeherungsradius der Flaeche in km (halbe Bounding-Box-Diagonale).
    pub radius_km: Option<f64>,
    /// Nur gesetzt, wenn zusaetzlich Heimatkoordinaten vorliegen (Entscheidung 25).
    pub distance_km: Option<f32>,
    pub azimuth_deg: Option<f32>,
}

/// Einen Ortscode in Mittelpunkt (WGS84-Grad) + Naeherungsradius (km)
/// uebersetzen. `None` bei unbekannter Zone oder leerer Ziffernfolge.
fn decode(code: &str) -> Option<(f64, f64, f64)> {
    let rest = code.strip_prefix('Z')?;
    let colon = rest.find(':')?;
    let zone: u8 = rest[..colon].parse().ok()?;
    let after = &rest[colon + 1..];
    let digits_str = match after.find('+') {
        Some(i) => &after[..i],
        None => after,
    };
    if digits_str.is_empty() {
        return None;
    }
    let digits: Vec<u8> = digits_str.chars().map(|c| c.to_digit(16).map(|d| d as u8)).collect::<Option<_>>()?;

    // Suedlicher Extent (SE = 90 - Breite; Nordpol 0, Aequator 90, Suedpol 180)
    // und Oestlicher Extent (EE = Laenge, negative Werte + 360) je min/max in Grad.
    let (mut se_min, mut se_max, mut ee_min, mut ee_max, start_idx): (f64, f64, f64, f64, usize);
    match zone {
        1..=40 => {
            // 40 Baender-Zonen, 36x36 Grad, 4 Reihen x 10 Spalten (Annex F.3).
            let z = (zone - 1) as f64;
            let row = (z / 10.0).floor();
            let col = z % 10.0;
            se_min = 18.0 + 36.0 * row;
            se_max = se_min + 36.0;
            ee_min = 36.0 * col;
            ee_max = ee_min + 36.0;
            start_idx = 0;
        }
        0 | 41 => {
            // Polzonen (Annex F.5): erste Ziffer waehlt einen Sektor um den
            // Pol (aussen Ring 1-10, innen Kappe 11-15; am Suedpol an der
            // Zonenmitte gespiegelt), ab der zweiten Ziffer wieder die
            // normale 4x4-Kachelung wie bei den Baender-Zonen.
            let d1 = *digits.first()?;
            let (zone_min, zone_max) = if zone == 0 { (0.0, 18.0) } else { (162.0, 180.0) };
            let mid = if zone == 0 { zone_min + 9.0 } else { zone_max - 9.0 };
            match d1 {
                1..=10 => {
                    let sector = (d1 - 1) as f64;
                    ee_min = 36.0 * sector;
                    ee_max = ee_min + 36.0;
                    (se_min, se_max) = if zone == 0 { (mid, zone_max) } else { (zone_min, mid) };
                }
                11..=15 => {
                    let sector = (d1 - 11) as f64;
                    ee_min = 72.0 * sector;
                    ee_max = ee_min + 72.0;
                    (se_min, se_max) = if zone == 0 { (zone_min, mid) } else { (mid, zone_max) };
                }
                _ => return None,
            }
            start_idx = 1;
        }
        _ => return None,
    }

    // Weitere Ziffern (Annex F.4, Figure F.3): obere 2 Bit = Nord-Sued-Reihe
    // (0-3), untere 2 Bit = West-Ost-Spalte (0-3); jede Ziffer teilt beide
    // Ausdehnungen durch 4 (Aufloesung x4 pro Ziffer, wie in der Norm beschrieben).
    for &d in digits.get(start_idx..).unwrap_or_default() {
        let row = (d >> 2) as f64;
        let col = (d & 3) as f64;
        let se_step = (se_max - se_min) / 4.0;
        let ee_step = (ee_max - ee_min) / 4.0;
        se_max = se_min + (row + 1.0) * se_step;
        se_min += row * se_step;
        ee_max = ee_min + (col + 1.0) * ee_step;
        ee_min += col * ee_step;
    }

    let se_c = (se_min + se_max) / 2.0;
    let ee_c = (ee_min + ee_max) / 2.0;
    let lat = 90.0 - se_c;
    let lon = if ee_c > 180.0 { ee_c - 360.0 } else { ee_c };

    // Naeherungsradius aus der Bounding-Box-Diagonale (1 Grad Breite ~ 111 km,
    // Laengengrad mit cos(Breite) gestaucht) - grob genug fuer "wie weit weg".
    let se_span_km = (se_max - se_min) * 111.0;
    let ee_span_km = (ee_max - ee_min) * 111.0 * lat.to_radians().cos().abs().max(0.05);
    let radius_km = (se_span_km.powi(2) + ee_span_km.powi(2)).sqrt() / 2.0;

    Some((lat, lon, radius_km))
}

/// ETSI TS 104 089 Annex A ("DAB location code presentation format"): die
/// fuer Zifferntasten/QR-Codes eingebbare Form eines Ortscodes, z. B.
/// "1253-3513-3668" (der ASA-"Standort-Code" von asa.radio bzw. der
/// ASA-Funktionstest-Anleitung). Kein eigenes Geokodierverfahren - derselbe
/// Ortscode wie in FIG 0/15 (max. Aufloesung, 6 Hexziffern, keine
/// Subcode-Bitmaske, da ein Standort ein Punkt ist), nur oktal + 6-Bit-
/// Pruefsumme (modulo 61) kodiert: 3 Gruppen a 4 Ziffern "1".."8"
/// (Oktalziffer 0..7 + 1), macht 36 Bit = 30-Bit-Code (6 Bit Zone + 24 Bit
/// Ortscode) gefolgt von der Pruefsumme. Verifiziert an beiden
/// Rechenbeispielen der Norm (BBC Broadcasting House, Svalbard Museum) und
/// am realen ASA-DE-Funktionstestcode (Eiffelturm), siehe Tests.
pub fn decode_presentation_code(code: &str) -> Option<(f64, f64)> {
    let stripped: String = code.chars().filter(|c| !c.is_whitespace()).collect();
    let digits: String = stripped.split('-').collect();
    if digits.len() != 12 {
        return None;
    }
    let mut bits: u64 = 0;
    for c in digits.chars() {
        let d = c.to_digit(10)?;
        if !(1..=8).contains(&d) {
            return None;
        }
        bits = (bits << 3) | (d as u64 - 1);
    }
    let checksum = bits & 0x3F;
    let value30 = (bits >> 6) & 0x3FFF_FFFF;
    if value30 % 61 != checksum {
        return None; // Tippfehler oder kein gueltiger Standort-Code
    }
    let zone = (value30 >> 24) & 0x3F;
    let loc24 = value30 & 0xFF_FFFF;
    let hex: String = (0..6)
        .rev()
        .map(|i| std::char::from_digit(((loc24 >> (i * 4)) & 0xF) as u32, 16).unwrap().to_ascii_uppercase())
        .collect();
    decode(&format!("Z{zone}:{hex}")).map(|(lat, lon, _radius)| (lat, lon))
}

/// Alle Ortscodes eines Alarms uebersetzen; mit Heimatkoordinaten (falls in
/// den Einstellungen gesetzt) zusaetzlich Entfernung/Richtung wie bei TII.
pub fn translate(locations: &[String], home: Option<(f64, f64)>) -> Vec<LocationInfo> {
    locations
        .iter()
        .map(|code| {
            let decoded = decode(code);
            let (lat, lon, radius_km) = match decoded {
                Some((lat, lon, radius_km)) => (Some(lat), Some(lon), Some(radius_km)),
                None => (None, None, None),
            };
            let (distance_km, azimuth_deg) = match (decoded, home) {
                (Some((lat, lon, _)), Some((hlat, hlon))) => {
                    (Some(crate::tii::haversine_km(hlat, hlon, lat, lon) as f32), Some(crate::tii::azimuth_deg(hlat, hlon, lat, lon) as f32))
                }
                _ => (None, None),
            };
            LocationInfo { code: code.clone(), lat, lon, radius_km, distance_km, azimuth_deg }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Rechenbeispiel aus Annex F.4: BBC Broadcasting House, London.
    #[test]
    fn decodes_bbc_broadcasting_house_example() {
        let (lat, lon, _) = decode("Z10:B736BB").expect("Code sollte sich dekodieren lassen");
        // 6 Ziffern = ~977 m Aufloesung; Mittelpunkt der Kachel weicht vom
        // Referenzpunkt der Norm entsprechend leicht ab.
        assert!((lat - 51.5187412).abs() < 0.01, "lat={lat}");
        assert!((lon - -0.1434571).abs() < 0.01, "lon={lon}");
    }

    /// Rechenbeispiel aus Annex F.5: Svalbard Museum, Longyearbyen (Nordpol-Zone).
    #[test]
    fn decodes_svalbard_polar_example() {
        let (lat, lon, _) = decode("Z0:152FF1").expect("Code sollte sich dekodieren lassen");
        assert!((lat - 78.222609).abs() < 0.01, "lat={lat}");
        assert!((lon - 15.651605).abs() < 0.02, "lon={lon}");
    }

    /// Kein Rechenbeispiel der Norm, aber eine reale Nachbarzone (Suedpol,
    /// aeusserer Ring): grobe Plausibilitaet statt exaktem Wert.
    #[test]
    fn decodes_south_polar_zone_into_the_southern_hemisphere() {
        let (lat, _, _) = decode("Z41:5").expect("Code sollte sich dekodieren lassen");
        assert!(lat < -70.0, "lat={lat} sollte nahe am Suedpol liegen");
    }

    /// Ortscodes aus dem Warntag-2026-Mitschnitt (Bundesmux 5C, Zone 1 =
    /// Mitteleuropa) muessen grob in Deutschland/Mitteleuropa landen.
    #[test]
    fn warntag_2026_codes_land_in_central_europe() {
        for code in ["Z1:5C+F300", "Z1:95+7FFF", "Z1:86+88CC", "Z1:9+0113", "Z1:8"] {
            let (lat, lon, radius_km) = decode(code).unwrap_or_else(|| panic!("{code} sollte sich dekodieren lassen"));
            assert!((35.0..72.0).contains(&lat), "{code}: lat={lat} ausserhalb Zone 1");
            assert!((0.0..36.0).contains(&lon), "{code}: lon={lon} ausserhalb Zone 1");
            assert!(radius_km > 0.0);
        }
    }

    #[test]
    fn unknown_or_malformed_codes_decode_to_none() {
        assert_eq!(decode("nonsense"), None);
        assert_eq!(decode("Z1:"), None); // keine Ziffern
        assert_eq!(decode("Z99:5C"), None); // Zone ausserhalb 0..41
        assert_eq!(decode("Z1:GG"), None); // kein Hex
    }

    /// Annex A Beispiel 1: BBC Broadcasting House (Z10:B736BB).
    #[test]
    fn decodes_presentation_code_bbc_example() {
        let (lat, lon) = decode_presentation_code("2366-7443-8484").expect("gueltiger Standort-Code");
        assert!((lat - 51.5187412).abs() < 0.01, "lat={lat}");
        assert!((lon - -0.1434571).abs() < 0.01, "lon={lon}");
    }

    /// Annex A Beispiel 2: Svalbard Museum (Z0:152FF1), Polzone.
    #[test]
    fn decodes_presentation_code_svalbard_example() {
        let (lat, lon) = decode_presentation_code("1116-3388-7268").expect("gueltiger Standort-Code");
        assert!((lat - 78.222609).abs() < 0.01, "lat={lat}");
        assert!((lon - 15.651605).abs() < 0.02, "lon={lon}");
    }

    /// Realer ASA-DE-Funktionstestcode aus der Handout-Anleitung
    /// (dabplus.de/asa.radio): liegt am Eiffelturm (48,8584N 2,2945E).
    #[test]
    fn decodes_presentation_code_asa_functional_test() {
        let (lat, lon) = decode_presentation_code("1253-3513-3668").expect("gueltiger Standort-Code");
        assert!((lat - 48.8584).abs() < 0.1, "lat={lat}");
        assert!((lon - 2.2945).abs() < 0.1, "lon={lon}");
    }

    /// Ohne Bindestriche/mit Leerzeichen muss dieselbe Position herauskommen.
    #[test]
    fn decodes_presentation_code_ignores_hyphen_placement_and_whitespace() {
        let a = decode_presentation_code("1253-3513-3668").unwrap();
        let b = decode_presentation_code(" 125335133668 ").unwrap();
        let c = decode_presentation_code("12-53-35-13-36-68").unwrap();
        assert_eq!(a, b);
        assert_eq!(a, c);
    }

    /// Ein einziger falscher Zeichenwert muss an der Pruefsumme scheitern
    /// (Tippfehler beim manuellen Eintippen erkennen).
    #[test]
    fn rejects_presentation_code_with_bad_checksum() {
        assert_eq!(decode_presentation_code("1253-3513-3669"), None);
    }

    #[test]
    fn rejects_malformed_presentation_codes() {
        assert_eq!(decode_presentation_code("1253-3513-366"), None); // zu kurz
        assert_eq!(decode_presentation_code("1253-3513-93668"), None); // zu lang
        assert_eq!(decode_presentation_code("1253-3513-9668"), None); // Ziffer 9 ungueltig (nur 1..8)
        assert_eq!(decode_presentation_code("abcd-efgh-ijkl"), None); // keine Ziffern
    }

    #[test]
    fn translate_adds_distance_and_azimuth_only_with_home() {
        let locations = vec!["Z1:5C+F300".to_string(), "bad-code".to_string()];
        let without_home = translate(&locations, None);
        assert_eq!(without_home.len(), 2);
        assert!(without_home[0].lat.is_some() && without_home[0].distance_km.is_none());
        assert!(without_home[1].lat.is_none() && without_home[1].distance_km.is_none());

        // Duesseldorf als Heimat (Sendestandort aus dem Warntag-Test).
        let with_home = translate(&locations, Some((51.2180, 6.7617)));
        assert!(with_home[0].distance_km.is_some());
        assert!(with_home[0].distance_km.unwrap() < 1000.0, "Ortscode 5C sollte in Mitteleuropa liegen");
        assert!(with_home[1].distance_km.is_none(), "unbekannter Code bleibt ohne Entfernung");
    }
}
