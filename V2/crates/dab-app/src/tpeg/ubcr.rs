//! Binaerleser fuer TPEG2 (ISO 21219-3, "UML to binary conversion rules").
//!
//! Die Regeln sind nicht frei verfuegbar; die hier umgesetzte Form wurde am
//! Mitschnitt von "ARD TPEG" (11D, 17.09.2026) nachvollzogen und gegen die
//! Apache-2.0-Referenz `fenghlkevin/tpeg-item` (Java) abgeglichen:
//!
//! * Komponente = `id` (1 Byte) + Gesamtlaenge (IntUnLoMB) + Attributlaenge
//!   (IntUnLoMB) + Attribute + Unterkomponenten. Die Attributlaenge erlaubt,
//!   unbekannte Attribute am Ende zu ueberspringen und trotzdem die
//!   Unterkomponenten zu lesen.
//! * IntUnLoMB = vorzeichenlose Zahl, 7 Bit je Byte, MSB = "weiteres Byte".
//! * Optionale Attribute (und boolesche Pflichtattribute) stehen in einem
//!   Selektor: ein BitArray, ebenfalls MSB = "weiteres Byte", die Elemente
//!   belegen Bit 6..0 des ersten Bytes, dann Bit 6..0 des zweiten usw.
//!   Element 0 ist das erste optionale Attribut in Modellreihenfolge.
//! * DateTime = Sekunden seit 1970 UTC (4 Byte), ShortString = Laenge (1 Byte)
//!   + UTF-8, LocalisedShortString = Sprachcode (1 Byte) + ShortString.

/// Sequenzieller Leser ueber einen Byte-Ausschnitt; alle Lesefunktionen
/// liefern `None` statt zu panicken, wenn die Daten zu kurz sind.
#[derive(Clone, Copy, Debug)]
pub struct Reader<'a> {
    data: &'a [u8],
    pos: usize,
}

/// Selektor-Bits (siehe Modulkopf); `has(i)` = Element `i` vorhanden.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Selector {
    bits: u64,
    len: u8,
}

impl Selector {
    pub fn has(&self, idx: u8) -> bool {
        idx < self.len && (self.bits >> idx) & 1 == 1
    }
    /// Testhilfe: Selektor aus Element-Indizes bauen.
    #[cfg(test)]
    pub fn from_indices(idx: &[u8]) -> Self {
        let mut s = Selector { bits: 0, len: 63 };
        for &i in idx {
            s.bits |= 1 << i;
        }
        s
    }
}

/// Eine gelesene Komponente: Kennung, Attribut-Bytes, Unterkomponenten-Bytes.
#[derive(Clone, Copy, Debug)]
pub struct Component<'a> {
    pub id: u8,
    pub attrs: &'a [u8],
    pub children: &'a [u8],
}

impl<'a> Component<'a> {
    pub fn attrs(&self) -> Reader<'a> {
        Reader::new(self.attrs)
    }
    pub fn children(&self) -> Reader<'a> {
        Reader::new(self.children)
    }
}

impl<'a> Reader<'a> {
    pub fn new(data: &'a [u8]) -> Self {
        Reader { data, pos: 0 }
    }
    pub fn remaining(&self) -> usize {
        self.data.len().saturating_sub(self.pos)
    }
    pub fn is_empty(&self) -> bool {
        self.remaining() == 0
    }
    pub fn rest(&self) -> &'a [u8] {
        &self.data[self.pos.min(self.data.len())..]
    }
    pub fn peek_u8(&self) -> Option<u8> {
        self.data.get(self.pos).copied()
    }
    pub fn bytes(&mut self, n: usize) -> Option<&'a [u8]> {
        if self.remaining() < n {
            return None;
        }
        let s = &self.data[self.pos..self.pos + n];
        self.pos += n;
        Some(s)
    }
    pub fn u8(&mut self) -> Option<u8> {
        self.bytes(1).map(|b| b[0])
    }
    pub fn u16(&mut self) -> Option<u16> {
        self.bytes(2).map(|b| u16::from_be_bytes([b[0], b[1]]))
    }
    pub fn i16(&mut self) -> Option<i16> {
        self.u16().map(|v| v as i16)
    }
    pub fn u32(&mut self) -> Option<u32> {
        self.bytes(4).map(|b| u32::from_be_bytes([b[0], b[1], b[2], b[3]]))
    }
    /// Vorzeichenbehaftete 24-Bit-Zahl (OpenLR-Koordinaten).
    pub fn i24(&mut self) -> Option<i32> {
        self.bytes(3).map(|b| {
            let v = ((b[0] as u32) << 16) | ((b[1] as u32) << 8) | b[2] as u32;
            if v & 0x80_0000 != 0 {
                (v | 0xFF00_0000) as i32
            } else {
                v as i32
            }
        })
    }
    /// IntUnLoMB: 7 Bit je Byte, MSB = Fortsetzung; hoechstens 5 Byte.
    pub fn int_unlomb(&mut self) -> Option<u32> {
        let mut v: u32 = 0;
        for _ in 0..5 {
            let b = self.u8()?;
            v = (v << 7) | (b & 0x7F) as u32;
            if b & 0x80 == 0 {
                return Some(v);
            }
        }
        None
    }
    /// Selektor-BitArray (siehe Modulkopf), hoechstens 8 Byte.
    pub fn selector(&mut self) -> Option<Selector> {
        let mut bits: u64 = 0;
        let mut len: u8 = 0;
        for _ in 0..8 {
            let b = self.u8()?;
            for k in 0..7u8 {
                if (b >> (6 - k)) & 1 == 1 {
                    bits |= 1 << (len + k);
                }
            }
            len += 7;
            if b & 0x80 == 0 {
                return Some(Selector { bits, len });
            }
        }
        None
    }
    /// ShortString: Laenge (1 Byte) + UTF-8 (tolerant).
    pub fn short_string(&mut self) -> Option<String> {
        let n = self.u8()? as usize;
        let b = self.bytes(n)?;
        Some(String::from_utf8_lossy(b).trim_end_matches('\0').to_string())
    }
    /// LocalisedShortString: Sprachcode (1 Byte) + ShortString.
    pub fn localised_short_string(&mut self) -> Option<(u8, String)> {
        let lang = self.u8()?;
        let s = self.short_string()?;
        Some((lang, s))
    }
    /// Komponente: id, Gesamtlaenge, Attributlaenge, Attribute, Unterkomponenten.
    pub fn component(&mut self) -> Option<Component<'a>> {
        let id = self.u8()?;
        let len = self.int_unlomb()? as usize;
        let body = self.bytes(len)?;
        let mut r = Reader::new(body);
        let attr_len = r.int_unlomb()? as usize;
        let attrs = r.bytes(attr_len)?;
        Some(Component { id, attrs, children: r.rest() })
    }
    /// Alle Komponenten bis zum Ende (defekte Reste werden verworfen).
    pub fn components(mut self) -> Vec<Component<'a>> {
        let mut out = Vec::new();
        while !self.is_empty() {
            match self.component() {
                Some(c) => out.push(c),
                None => break,
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn int_unlomb_und_selector() {
        let mut r = Reader::new(&[0x61, 0x81, 0x1d, 0x84, 0xe6, 0x52, 0x60, 0x10, 0xcf, 0x7d]);
        assert_eq!(r.int_unlomb(), Some(97));
        assert_eq!(r.int_unlomb(), Some(157));
        assert_eq!(r.int_unlomb(), Some(78674));
        // 0x60: Elemente 0 und 1 (Start- und Endzeit im TEC-Ereignis)
        let s = r.selector().unwrap();
        assert!(s.has(0) && s.has(1) && !s.has(2));
        // 0x10: Element 2 (Prioritaet im MMC)
        let s = r.selector().unwrap();
        assert!(!s.has(0) && !s.has(1) && s.has(2) && !s.has(3));
        // 0xcf 0x7d: zwei Byte (MSB des ersten gesetzt)
        let s = r.selector().unwrap();
        assert_eq!(s.len, 14);
        assert!(s.has(0) && !s.has(1) && !s.has(2) && s.has(3) && s.has(6) && s.has(7));
        assert!(r.is_empty());
    }

    #[test]
    fn komponente_mit_attributen_und_kindern() {
        // id 3, len 7, attrLen 2, attrs [1,2], Kind: id 4, len 2, attrLen 1, attr [9]; 0xff bleibt uebrig
        let raw = [0x03, 0x07, 0x02, 0x01, 0x02, 0x04, 0x02, 0x01, 0x09, 0xff];
        let mut r = Reader::new(&raw);
        let c = r.component().unwrap();
        assert_eq!(c.id, 3);
        assert_eq!(c.attrs, &[1, 2]);
        let kids = c.children().components();
        assert_eq!(kids.len(), 1);
        assert_eq!(kids[0].id, 4);
        assert_eq!(kids[0].attrs, &[9]);
        assert_eq!(r.remaining(), 1);
        assert!(Reader::new(&[0x03, 0x07, 0x02]).component().is_none());
    }

    #[test]
    fn i24_und_strings() {
        let mut r = Reader::new(&[0x24, 0x8c, 0xeb, 0xff, 0xff, 0xff, 0x03, b'T', b'E', b'C', 0x09, 0x02, b'o', b'k']);
        assert_eq!(r.i24(), Some(0x248ceb));
        assert_eq!(r.i24(), Some(-1));
        assert_eq!(r.short_string().as_deref(), Some("TEC"));
        assert_eq!(r.localised_short_string(), Some((9, "ok".into())));
    }
}
