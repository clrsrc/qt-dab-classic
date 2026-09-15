#include "ews-location.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dabcore {
namespace {

constexpr double kEarthRadiusKm = 6371.0;
constexpr double kPi = 3.14159265358979323846;

double toRad(double deg) { return deg * kPi / 180.0; }

// Eine Hexziffer (gross oder klein) als Wert 0..15, sonst -1.
int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Ein FIG-0/15-Ortscode in seine Bestandteile zerlegt: Zone (dezimal),
// Ziffernfolge (bis zu 6 Hexziffern) und optionaler 16-Bit-Subcode (Annex
// D.2.3). std::nullopt bei jedem Formatfehler. Gemeinsame Grundlage fuer
// decodeEwsLocation() (Anzeige, Subcode wird verworfen) und
// ewsAlertRelevantForHome() (Geofencing, Subcode zaehlt mit).
struct ParsedCode {
    int zone = 0;
    std::vector<int> digits;         // 0..6 Werte 0..15, in Sendereihenfolge
    std::optional<int> subcode;      // 0..0xFFFF, gesetzt bei "+XXXX"
};

std::optional<ParsedCode> parseLocationCode(const std::string& code) {
    if (code.empty() || code[0] != 'Z') return std::nullopt;
    const std::size_t colon = code.find(':');
    if (colon == std::string::npos) return std::nullopt;

    // Zonennummer dezimal (0..41); alles andere ist kein gueltiger Code.
    const std::string zoneStr = code.substr(1, colon - 1);
    if (zoneStr.empty() || zoneStr.size() > 3) return std::nullopt;
    int zone = 0;
    for (char c : zoneStr) {
        if (c < '0' || c > '9') return std::nullopt;
        zone = zone * 10 + (c - '0');
    }
    if (zone > 41) return std::nullopt; // 0 = Nordpol, 1..40 = Baender, 41 = Suedpol

    std::string rest = code.substr(colon + 1);
    std::optional<int> subcode;
    const std::size_t plus = rest.find('+');
    std::string digitsStr = rest;
    if (plus != std::string::npos) {
        digitsStr = rest.substr(0, plus);
        const std::string subStr = rest.substr(plus + 1);
        if (subStr.size() != 4) return std::nullopt;
        int v = 0;
        for (char c : subStr) {
            const int d = hexDigit(c);
            if (d < 0) return std::nullopt;
            v = (v << 4) | d;
        }
        subcode = v;
    }
    if (digitsStr.empty()) return std::nullopt;
    std::vector<int> digits;
    digits.reserve(digitsStr.size());
    for (char c : digitsStr) {
        const int d = hexDigit(c);
        if (d < 0) return std::nullopt;
        digits.push_back(d);
    }
    if (digits.size() > 6) return std::nullopt; // mehr als maximale Aufloesung gibt es nicht
    return ParsedCode{zone, std::move(digits), subcode};
}

// Zone + Ziffernfolge (Annex F.3-F.5) in die Bounding-Box (Suedlicher Extent,
// Oestlicher Extent, je min/max in Grad) uebersetzen; std::nullopt bei
// unbekannter Zone oder ungueltiger erster Ziffer in einer Polzone.
struct Box { double seMin, seMax, eeMin, eeMax; };
std::optional<Box> boxForDigits(int zone, const std::vector<int>& digits) {
    double seMin = 0, seMax = 0, eeMin = 0, eeMax = 0;
    std::size_t startIdx = 0;
    if (zone >= 1 && zone <= 40) {
        // 40 Baender-Zonen, 36x36 Grad, 4 Reihen x 10 Spalten (Annex F.3).
        const double z = zone - 1;
        const double row = std::floor(z / 10.0);
        const double col = std::fmod(z, 10.0);
        seMin = 18.0 + 36.0 * row;
        seMax = seMin + 36.0;
        eeMin = 36.0 * col;
        eeMax = eeMin + 36.0;
    } else if (zone == 0 || zone == 41) {
        // Polzonen (Annex F.5): die erste Ziffer waehlt einen Sektor um den
        // Pol (aussen Ring 1-10, innen Kappe 11-15; am Suedpol an der
        // Zonenmitte gespiegelt), ab der zweiten Ziffer wieder die normale
        // 4x4-Kachelung.
        if (digits.empty()) return std::nullopt;
        const int d1 = digits.front();
        const double zoneMin = (zone == 0) ? 0.0 : 162.0;
        const double zoneMax = (zone == 0) ? 18.0 : 180.0;
        const double mid = (zone == 0) ? zoneMin + 9.0 : zoneMax - 9.0;
        if (d1 >= 1 && d1 <= 10) {
            const double sector = d1 - 1;
            eeMin = 36.0 * sector;
            eeMax = eeMin + 36.0;
            seMin = (zone == 0) ? mid : zoneMin;
            seMax = (zone == 0) ? zoneMax : mid;
        } else if (d1 >= 11 && d1 <= 15) {
            const double sector = d1 - 11;
            eeMin = 72.0 * sector;
            eeMax = eeMin + 72.0;
            seMin = (zone == 0) ? zoneMin : mid;
            seMax = (zone == 0) ? mid : zoneMax;
        } else {
            return std::nullopt;
        }
        startIdx = 1;
    } else {
        return std::nullopt;
    }

    // Weitere Ziffern (Annex F.4, Figure F.3): obere 2 Bit = Nord-Sued-Reihe
    // (0-3), untere 2 Bit = West-Ost-Spalte (0-3); jede Ziffer teilt beide
    // Ausdehnungen durch 4 (Aufloesung x4 je Ziffer).
    for (std::size_t i = startIdx; i < digits.size(); ++i) {
        const double row = digits[i] >> 2;
        const double col = digits[i] & 3;
        const double seStep = (seMax - seMin) / 4.0;
        const double eeStep = (eeMax - eeMin) / 4.0;
        seMax = seMin + (row + 1.0) * seStep;
        seMin += row * seStep;
        eeMax = eeMin + (col + 1.0) * eeStep;
        eeMin += col * eeStep;
    }
    return Box{seMin, seMax, eeMin, eeMax};
}

} // namespace

std::optional<std::tuple<double, double, double>> decodeEwsLocation(const std::string& code) {
    const auto parsed = parseLocationCode(code);
    if (!parsed) return std::nullopt;
    const auto box = boxForDigits(parsed->zone, parsed->digits);
    if (!box) return std::nullopt;

    const double seC = (box->seMin + box->seMax) / 2.0;
    const double eeC = (box->eeMin + box->eeMax) / 2.0;
    const double lat = 90.0 - seC;
    const double lon = (eeC > 180.0) ? eeC - 360.0 : eeC;

    // Naeherungsradius aus der Bounding-Box-Diagonale (1 Grad Breite ~ 111 km,
    // Laengengrad mit cos(Breite) gestaucht) - grob genug fuer "wie weit weg".
    const double seSpanKm = (box->seMax - box->seMin) * 111.0;
    const double eeSpanKm = (box->eeMax - box->eeMin) * 111.0 * std::max(std::fabs(std::cos(toRad(lat))), 0.05);
    const double radiusKm = std::sqrt(seSpanKm * seSpanKm + eeSpanKm * eeSpanKm) / 2.0;

    return std::make_tuple(lat, lon, radiusKm);
}

double haversineKm(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = toRad(lat1);
    const double p2 = toRad(lat2);
    const double dp = p2 - p1;
    const double dl = toRad(lon2 - lon1);
    const double s1 = std::sin(dp / 2.0);
    const double s2 = std::sin(dl / 2.0);
    const double a = s1 * s1 + std::cos(p1) * std::cos(p2) * s2 * s2;
    return 2.0 * kEarthRadiusKm * std::asin(std::min(1.0, std::sqrt(a)));
}

HomeLocation encodeEwsLocation(double lat, double lon) {
    double se = 90.0 - lat;
    double ee = (lon < 0.0) ? lon + 360.0 : lon;
    if (ee >= 360.0) ee -= 360.0;

    HomeLocation home;
    double seMin = 0, seMax = 0, eeMin = 0, eeMax = 0;
    std::size_t startIdx = 0;

    if (se < 18.0 || se >= 162.0) {
        const bool north = se < 18.0;
        const double zoneMin = north ? 0.0 : 162.0;
        const double zoneMax = north ? 18.0 : 180.0;
        const double mid = north ? 9.0 : 171.0;
        home.zone = north ? 0 : 41;
        // "Aussen" (Ring 1-10, 36 Grad) liegt fuer den Nordpol im Bereich
        // [mid, zoneMax), fuer den Suedpol gespiegelt in [zoneMin, mid).
        const bool outer = north ? (se >= mid) : (se < mid);
        int d1;
        if (outer) {
            const int sector = std::clamp(static_cast<int>(ee / 36.0), 0, 9);
            d1 = sector + 1;
            eeMin = 36.0 * sector; eeMax = eeMin + 36.0;
            seMin = north ? mid : zoneMin;
            seMax = north ? zoneMax : mid;
        } else {
            const int sector = std::clamp(static_cast<int>(ee / 72.0), 0, 4);
            d1 = sector + 11;
            eeMin = 72.0 * sector; eeMax = eeMin + 72.0;
            seMin = north ? zoneMin : mid;
            seMax = north ? mid : zoneMax;
        }
        home.digits[0] = d1;
        startIdx = 1;
    } else {
        const int row = std::clamp(static_cast<int>((se - 18.0) / 36.0), 0, 3);
        const int col = std::clamp(static_cast<int>(ee / 36.0), 0, 9);
        home.zone = row * 10 + col + 1;
        seMin = 18.0 + 36.0 * row; seMax = seMin + 36.0;
        eeMin = 36.0 * col; eeMax = eeMin + 36.0;
    }

    // Wie boxForDigits(), nur rueckwaerts: je Stufe die Kachel bestimmen, die
    // den Punkt enthaelt, statt eine gegebene Kachel aufzuteilen.
    for (std::size_t i = startIdx; i < 6; ++i) {
        const double seStep = (seMax - seMin) / 4.0;
        const double eeStep = (eeMax - eeMin) / 4.0;
        const int row = (seStep > 0.0) ? std::clamp(static_cast<int>((se - seMin) / seStep), 0, 3) : 0;
        const int col = (eeStep > 0.0) ? std::clamp(static_cast<int>((ee - eeMin) / eeStep), 0, 3) : 0;
        home.digits[i] = (row << 2) | col;
        const double newSeMin = seMin + row * seStep;
        const double newSeMax = seMin + (row + 1) * seStep;
        const double newEeMin = eeMin + col * eeStep;
        const double newEeMax = eeMin + (col + 1) * eeStep;
        seMin = newSeMin; seMax = newSeMax;
        eeMin = newEeMin; eeMax = newEeMax;
    }
    return home;
}

bool ewsAlertRelevantForHome(const std::vector<std::string>& codes, double homeLat, double homeLon) {
    const HomeLocation home = encodeEwsLocation(homeLat, homeLon);
    bool anyUsable = false;
    for (const auto& c : codes) {
        const auto parsed = parseLocationCode(c);
        if (!parsed) continue; // unlesbarer Code zaehlt weder dafuer noch dagegen
        anyUsable = true;
        if (parsed->zone != home.zone) continue;
        const auto& digits = parsed->digits;
        bool prefixMatches = true;
        for (std::size_t i = 0; i < digits.size(); ++i) {
            if (digits[i] != home.digits[i]) { prefixMatches = false; break; }
        }
        if (!prefixMatches) continue;
        if (!parsed->subcode) return true; // ETSI TS 104 089 Klausel 7.5.4: Ziffern-Praefix reicht
        // Annex D.2.3: Subcode-Bitmaske ueber die naechstfeinere (Enkel-)Ziffer.
        if (digits.size() >= 6) return true; // keine weitere Ziffer mehr moeglich -> voller Treffer
        const int childDigit = home.digits[digits.size()];
        if ((*parsed->subcode >> childDigit) & 1) return true;
        // sonst: Stem passt, aber die konkrete Kindzelle ist nicht gesetzt - kein Treffer fuer diesen Code
    }
    // Keine brauchbare Gebietsangabe -> keine Einschraenkung -> relevant.
    return !anyUsable;
}

} // namespace dabcore
