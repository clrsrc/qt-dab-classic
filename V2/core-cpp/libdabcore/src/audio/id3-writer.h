// DAB Classic v3 – minimaler ID3v2.4-Schreiber (Plan M4b 1.3).
// Von Hand gebaut, damit keine weitere Abhaengigkeit noetig ist: Header
// "ID3" 04 00, Tag-Groesse als syncsafe Integer, danach die Textrahmen
// TIT2/TPE1/TALB/TDRC (Encoding-Byte 0x03 = UTF-8) und APIC (image/png,
// Picture-Type 0x03 "Cover (front)"). Leere Felder werden weggelassen.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Id3Tags {
    std::string title;        // TIT2
    std::string artist;       // TPE1
    std::string album;        // TALB, z. B. Sendername
    std::string date;         // TDRC, ISO yyyy-mm-dd
    std::string genre;        // TCON, z. B. Programmtyp (FIG 0/17)
    std::string coverPngB64;  // APIC, PNG als Base64 (auch als data:-URL)

    bool empty() const {
        return title.empty() && artist.empty() && album.empty() && date.empty() && genre.empty() && coverPngB64.empty();
    }
};

namespace Id3Writer {

// Fertiger Tag (Header + Rahmen); leer, wenn kein Feld gesetzt ist.
std::vector<uint8_t> build(const Id3Tags& tags);

// Base64 -> Bytes; toleriert Zeilenumbrueche und eine fuehrende data:-URL.
std::vector<uint8_t> base64Decode(const std::string& in);

// Tag vor eine bereits geschriebene Datei setzen (Datei wird umkopiert).
// Wird nicht gebraucht, solange Mp3Writer den Tag beim Oeffnen schreibt,
// bleibt aber fuer nachtraegliches Taggen vorhanden.
bool prependToFile(const std::string& path, const std::vector<uint8_t>& tag, std::string& error);

} // namespace Id3Writer
