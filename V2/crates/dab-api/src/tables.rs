//! Namenstabellen aus ETSI TS 101 756 (Registrierte Tabellen fuer DAB):
//! Programmtyp (Tabelle 12, FIG 0/17) und Sprache (Tabellen 9/10, FIG 0/5).
//! Englische Bezeichner wie in der Norm; die Oberflaeche uebersetzt selbst
//! (i18n), die CLI und der ID3-Genre-Rahmen (TCON) nutzen diese Texte direkt.

/// Programmtyp 0..31 (TS 101 756 Tabelle 12, internationale Tabelle 1).
/// 0 = "kein Programmtyp" -> `None`; nicht belegte Codes 30/31 -> `None`.
pub fn pty_name(pty: u8) -> Option<&'static str> {
    const NAMES: [&str; 30] = [
        "",
        "News",
        "Current Affairs",
        "Information",
        "Sport",
        "Education",
        "Drama",
        "Culture",
        "Science",
        "Varied",
        "Pop Music",
        "Rock Music",
        "Easy Listening Music",
        "Light Classical",
        "Serious Classical",
        "Other Music",
        "Weather/meteorology",
        "Finance/Business",
        "Children's programmes",
        "Social Affairs",
        "Religion",
        "Phone In",
        "Travel",
        "Leisure",
        "Jazz Music",
        "Country Music",
        "National Music",
        "Oldies Music",
        "Folk Music",
        "Documentary",
    ];
    match pty {
        1..=29 => Some(NAMES[pty as usize]),
        _ => None,
    }
}

/// Programmtypen 10..15 und 24..28 sind Musikprogramme (fuer die Musik-Trennung).
pub fn pty_is_music(pty: u8) -> bool {
    matches!(pty, 10..=15 | 24..=28)
}

/// Sprache (TS 101 756 Tabelle 9 europaeisch 0x01..0x2B, Tabelle 10 weitere
/// 0x45..0x7F). 0 und unbekannte Codes -> `None`.
pub fn language_name(code: u8) -> Option<&'static str> {
    Some(match code {
        0x01 => "Albanian",
        0x02 => "Breton",
        0x03 => "Catalan",
        0x04 => "Croatian",
        0x05 => "Welsh",
        0x06 => "Czech",
        0x07 => "Danish",
        0x08 => "German",
        0x09 => "English",
        0x0A => "Spanish",
        0x0B => "Esperanto",
        0x0C => "Estonian",
        0x0D => "Basque",
        0x0E => "Faroese",
        0x0F => "French",
        0x10 => "Frisian",
        0x11 => "Irish",
        0x12 => "Gaelic",
        0x13 => "Galician",
        0x14 => "Icelandic",
        0x15 => "Italian",
        0x16 => "Sami",
        0x17 => "Latin",
        0x18 => "Latvian",
        0x19 => "Luxembourgian",
        0x1A => "Lithuanian",
        0x1B => "Hungarian",
        0x1C => "Maltese",
        0x1D => "Dutch",
        0x1E => "Norwegian",
        0x1F => "Occitan",
        0x20 => "Polish",
        0x21 => "Portuguese",
        0x22 => "Romanian",
        0x23 => "Romansh",
        0x24 => "Serbian",
        0x25 => "Slovak",
        0x26 => "Slovene",
        0x27 => "Finnish",
        0x28 => "Swedish",
        0x29 => "Turkish",
        0x2A => "Flemish",
        0x2B => "Walloon",
        0x40 => "Background sound/clean feed",
        0x45 => "Zulu",
        0x46 => "Vietnamese",
        0x47 => "Uzbek",
        0x48 => "Urdu",
        0x49 => "Ukrainian",
        0x4A => "Thai",
        0x4B => "Telugu",
        0x4C => "Tatar",
        0x4D => "Tamil",
        0x4E => "Tadzhik",
        0x4F => "Swahili",
        0x50 => "Sranan Tongo",
        0x51 => "Somali",
        0x52 => "Sinhalese",
        0x53 => "Shona",
        0x54 => "Serbo-Croat",
        0x55 => "Rusyn",
        0x56 => "Russian",
        0x57 => "Quechua",
        0x58 => "Pushtu",
        0x59 => "Punjabi",
        0x5A => "Persian",
        0x5B => "Papiamento",
        0x5C => "Oriya",
        0x5D => "Nepali",
        0x5E => "Ndebele",
        0x5F => "Marathi",
        0x60 => "Moldavian",
        0x61 => "Malaysian",
        0x62 => "Malagasay",
        0x63 => "Macedonian",
        0x64 => "Laotian",
        0x65 => "Korean",
        0x66 => "Khmer",
        0x67 => "Kazakh",
        0x68 => "Kannada",
        0x69 => "Japanese",
        0x6A => "Indonesian",
        0x6B => "Hindi",
        0x6C => "Hebrew",
        0x6D => "Hausa",
        0x6E => "Gurani",
        0x6F => "Gujurati",
        0x70 => "Greek",
        0x71 => "Georgian",
        0x72 => "Fulani",
        0x73 => "Dari",
        0x74 => "Chuvash",
        0x75 => "Chinese",
        0x76 => "Burmese",
        0x77 => "Bulgarian",
        0x78 => "Bengali",
        0x79 => "Belorussian",
        0x7A => "Bambora",
        0x7B => "Azerbaijani",
        0x7C => "Assamese",
        0x7D => "Armenian",
        0x7E => "Arabic",
        0x7F => "Amharic",
        _ => return None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pty_table() {
        assert_eq!(pty_name(0), None);
        assert_eq!(pty_name(1), Some("News"));
        assert_eq!(pty_name(10), Some("Pop Music"));
        assert_eq!(pty_name(29), Some("Documentary"));
        assert_eq!(pty_name(30), None);
        assert_eq!(pty_name(31), None);
        assert!(pty_is_music(11) && pty_is_music(24) && !pty_is_music(1) && !pty_is_music(29));
    }

    #[test]
    fn language_table() {
        assert_eq!(language_name(0), None);
        assert_eq!(language_name(0x08), Some("German"));
        assert_eq!(language_name(0x09), Some("English"));
        assert_eq!(language_name(0x29), Some("Turkish"));
        assert_eq!(language_name(0x2C), None);
        assert_eq!(language_name(0x7E), Some("Arabic"));
        assert_eq!(language_name(0x80), None);
    }
}
