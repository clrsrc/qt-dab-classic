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

} // namespace

std::optional<std::tuple<double, double, double>> decodeEwsLocation(const std::string& code) {
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
    if (zone > 255) return std::nullopt;

    // Ziffernfolge bis zum optionalen "+<Subcode>"; der Subcode wird ignoriert.
    std::string rest = code.substr(colon + 1);
    const std::size_t plus = rest.find('+');
    if (plus != std::string::npos) rest = rest.substr(0, plus);
    if (rest.empty()) return std::nullopt;
    std::vector<int> digits;
    digits.reserve(rest.size());
    for (char c : rest) {
        const int d = hexDigit(c);
        if (d < 0) return std::nullopt;
        digits.push_back(d);
    }

    // Suedlicher Extent (SE = 90 - Breite; Nordpol 0, Aequator 90, Suedpol 180)
    // und Oestlicher Extent (EE = Laenge, negative Werte + 360), je min/max in Grad.
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
        startIdx = 0;
    } else if (zone == 0 || zone == 41) {
        // Polzonen (Annex F.5): die erste Ziffer waehlt einen Sektor um den Pol
        // (aussen Ring 1-10, innen Kappe 11-15; am Suedpol an der Zonenmitte
        // gespiegelt), ab der zweiten Ziffer wieder die normale 4x4-Kachelung.
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

    const double seC = (seMin + seMax) / 2.0;
    const double eeC = (eeMin + eeMax) / 2.0;
    const double lat = 90.0 - seC;
    const double lon = (eeC > 180.0) ? eeC - 360.0 : eeC;

    // Naeherungsradius aus der Bounding-Box-Diagonale (1 Grad Breite ~ 111 km,
    // Laengengrad mit cos(Breite) gestaucht) - grob genug fuer "wie weit weg".
    const double seSpanKm = (seMax - seMin) * 111.0;
    const double eeSpanKm = (eeMax - eeMin) * 111.0 * std::max(std::fabs(std::cos(toRad(lat))), 0.05);
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

bool ewsAlertRelevantForHome(const std::vector<std::string>& codes, double homeLat, double homeLon) {
    bool anyUsable = false;
    for (const auto& c : codes) {
        const auto decoded = decodeEwsLocation(c);
        if (!decoded) continue;   // unlesbarer Code zaehlt weder dafuer noch dagegen
        anyUsable = true;
        const auto [lat, lon, radiusKm] = *decoded;
        if (haversineKm(homeLat, homeLon, lat, lon) <= radiusKm) return true;
    }
    // Keine brauchbare Gebietsangabe -> keine Einschraenkung -> relevant.
    return !anyUsable;
}

} // namespace dabcore
