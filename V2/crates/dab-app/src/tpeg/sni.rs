//! TPEG-SNI: Dienst- und Netzinformation (ISO 21219-9), Komponentenrahmen
//! SCID 0. Liefert Dienstname, Beschreibung, Anbieter/Encoder und die
//! Zuordnung SCID -> Anwendung (AID 5 = TEC, 7 = TFP) samt Versionen.
//!
//! Aufbau des Felds (Java-Referenz `SNIFrame`, Mitschnitt ARD 17.09.2026):
//! Tabellenzahl (1) | Tabellen: id (1) | Laenge (2) | Inhalt | ... | CRC (2)
//!
//! * Tabelle 1 (Fast Tuning): Version (1), Zeichensatz (1), Zeilen
//!   SCID (1) | Schalter (1) | [SID-A/B/C (3)] | Inhaltskennung (1) | AID (2) | [Betriebszeit (8)] | [Verschluesselung (1)]
//! * Tabelle 14 (Versionen): Version (1), Zeilen SCID (1) | Major (1) | Minor (1)
//! * Tabelle 0: Dienstname (ShortString), Beschreibung (ShortString)
//! * Tabelle 3: Version (1), Zeilen SCID (1) | Name (ShortString)
//! * Tabelle 11: Version (1), Freitext (Encoder/Anbieter)

use super::ubcr::Reader;

pub const AID_SNI: u16 = 0;
pub const AID_TEC: u16 = 5;
pub const AID_TFP: u16 = 7;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SniInfo {
    pub service_name: String,
    pub description: String,
    pub provider: String,
    /// (SCID, AID, Komponentenname, Version major.minor)
    pub components: Vec<SniComponent>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SniComponent {
    pub scid: u8,
    pub aid: u16,
    pub name: String,
    pub major: u8,
    pub minor: u8,
}

impl SniInfo {
    pub fn scid_for_aid(&self, aid: u16) -> Option<u8> {
        self.components.iter().find(|c| c.aid == aid).map(|c| c.scid)
    }
    pub fn aid_for_scid(&self, scid: u8) -> Option<u16> {
        self.components.iter().find(|c| c.scid == scid).map(|c| c.aid)
    }
    fn comp(&mut self, scid: u8) -> &mut SniComponent {
        if let Some(i) = self.components.iter().position(|c| c.scid == scid) {
            return &mut self.components[i];
        }
        self.components.push(SniComponent { scid, ..Default::default() });
        self.components.last_mut().unwrap()
    }
}

/// SNI-Feld (ohne Kopf, mit oder ohne CRC am Ende) auswerten.
pub fn parse_sni(payload: &[u8]) -> Option<SniInfo> {
    let mut r = Reader::new(payload);
    let count = r.u8()? as usize;
    let mut info = SniInfo::default();
    for _ in 0..count {
        let id = r.u8()?;
        let len = r.u16()? as usize;
        let body = r.bytes(len)?;
        let mut t = Reader::new(body);
        match id {
            1 => {
                let _version = t.u8();
                let _charset = t.u8();
                while t.remaining() >= 5 {
                    let scid = t.u8()?;
                    let sw = t.selector()?;
                    if sw.has(0) {
                        t.bytes(3)?;
                    }
                    let _content = t.u8()?;
                    let aid = t.u16()?;
                    if sw.has(2) {
                        t.bytes(8)?;
                    }
                    if sw.has(3) {
                        t.u8()?;
                    }
                    info.comp(scid).aid = aid;
                }
            }
            14 => {
                let _version = t.u8();
                while t.remaining() >= 3 {
                    let scid = t.u8()?;
                    let major = t.u8()?;
                    let minor = t.u8()?;
                    let c = info.comp(scid);
                    c.major = major;
                    c.minor = minor;
                }
            }
            0 => {
                info.service_name = t.short_string().unwrap_or_default();
                info.description = t.short_string().unwrap_or_default();
            }
            3 => {
                let _version = t.u8();
                while t.remaining() >= 2 {
                    let scid = t.u8()?;
                    let name = t.short_string()?;
                    info.comp(scid).name = name;
                }
            }
            11 => {
                let _version = t.u8();
                let text = String::from_utf8_lossy(t.rest()).trim().to_string();
                info.provider = text.trim_matches(|c| c == '[' || c == ']').to_string();
            }
            _ => {}
        }
    }
    info.components.sort_by_key(|c| c.scid);
    Some(info)
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// SNI-Feld von "ARD TPEG" (11D, 17.09.2026), 152 Byte inkl. CRC.
    pub const ARD_SNI: &str = "05010011cf7d0000000000010001000502000200070e000acf00030201030202010000003008415244205450454726416b7475656c6c65205665726b65687273696e666f726d6174696f6e656e206465722041524403000bcf010354454302035446500b00302f5bc2a93230323620626d742054504547204f4e204149522d332e312d7769707c303030303233303430387c5744525dee9f";

    pub fn unhex(s: &str) -> Vec<u8> {
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
    }

    #[test]
    fn ard_sni() {
        let info = parse_sni(&unhex(ARD_SNI)).unwrap();
        assert_eq!(info.service_name, "ARD TPEG");
        assert_eq!(info.description, "Aktuelle Verkehrsinformationen der ARD");
        assert_eq!(info.provider, "©2026 bmt TPEG ON AIR-3.1-wip|0000230408|WDR");
        assert_eq!(info.components.len(), 3);
        assert_eq!(info.scid_for_aid(AID_TEC), Some(1));
        assert_eq!(info.scid_for_aid(AID_TFP), Some(2));
        let tec = &info.components[1];
        assert_eq!((tec.name.as_str(), tec.major, tec.minor), ("TEC", 3, 2));
        let tfp = &info.components[2];
        assert_eq!((tfp.name.as_str(), tfp.major, tfp.minor), ("TFP", 1, 0));
        assert_eq!(info.components[0].aid, AID_SNI);
        assert!(parse_sni(&[5, 1, 0]).is_none());
    }
}
