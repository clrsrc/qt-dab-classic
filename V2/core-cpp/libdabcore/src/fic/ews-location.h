// DAB Classic v3 – EWS-Ortscodes (ETSI TS 104 089 Annex F) -> WGS84.
//
// Die Codes der FIG 0/15 sind keine benannten Verwaltungsregionen, sondern ein
// Quadtree ueber der Erdkugel: 6-Bit-Zone (36x36-Grad-Kachel bzw. Polzone) und
// bis zu sechs weitere 4-Bit-Ziffern, die die Kachel je Stufe in 4x4 Felder
// teilen. Sie dienen dem automatischen Abgleich "betrifft mich das?"
// (Klausel 7.5/7.6), nicht der Anzeige.
//
// Eingabeformat ist genau das, was fibDecoder::readLocationCode erzeugt:
//   Z<Zone>:<Hexziffern>[+<16-Bit-Subcode-Hex>]   z. B. "Z1:5C+F300"
// Fuer decodeEwsLocation() (Anzeige) wird die Subcode-Bitmaske ignoriert: das
// Ergebnis ist die groebere Elternflaeche, nie feiner als der Code selbst.
// ewsAlertRelevantForHome() (Geofencing) wertet den Subcode dagegen aus
// (Annex D.2.3), siehe dort.
//
// Referenz und Ground Truth ist crates/dab-app/src/ews_location.rs (dort an
// den beiden Rechenbeispielen der Norm und an den Warntag-2026-Codes
// geprueft); diese Portierung liefert bit-gleiche Ergebnisse.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace dabcore {

// Einen Ortscode in (Breite, Laenge, Naeherungsradius in km) uebersetzen.
// Der Radius ist die halbe Diagonale der Bounding-Box der Kachel.
// std::nullopt bei unbekannter Zone, leerer Ziffernfolge oder Formatfehler.
std::optional<std::tuple<double, double, double>> decodeEwsLocation(const std::string& code);

// Grosskreis-Entfernung in km (Erdradius 6371 km), wie dab_app::tii.
double haversineKm(double lat1, double lon1, double lat2, double lon2);

// Eigener Standort bei maximaler Aufloesung (Annex F: Zone + 6 Hexziffern
// 0..15) aus WGS84-Grad - die Umkehrung von decodeEwsLocation(). Wird von
// ewsAlertRelevantForHome() fuer den Ziffernvergleich benutzt; hier nur zum
// direkten Testen (Rundreise decode->encode->decode) exportiert.
struct HomeLocation {
    int zone = 0;
    std::array<int, 6> digits{};
};
HomeLocation encodeEwsLocation(double lat, double lon);

// Geofencing nach ETSI TS 104 089 Klausel 7.5.4 (Location matching): betrifft
// ein Alarm mit diesen Ortscodes die Heimatposition? Kein Distanzvergleich,
// sondern der von der Norm vorgeschriebene Ziffernvergleich: der eigene
// Standort-Code (max. Aufloesung) wird links-buendig auf die Ziffernzahl des
// jeweiligen Alarmcodes gekuerzt; stimmen Zone und diese Ziffern exakt
// ueberein, ist der Code ein Treffer. Traegt der Alarmcode zusaetzlich einen
// Subcode (Annex D.2.3, "+XXXX"), muss danach noch die naechste eigene Ziffer
// im gesetzten Bit der 16-Bit-Maske vorkommen. true, sobald irgendein Code
// im Alarm-Satz trifft. Nicht dekodierbare Codes werden uebersprungen und
// zaehlen nicht gegen die Relevanz; bleibt gar kein brauchbarer Code uebrig
// (leere Liste, z. B. die End-Phase, oder lauter unlesbare Codes), gibt es
// keine Gebietseinschraenkung und der Alarm gilt als relevant - im Zweifel
// warnen, nicht verschlucken.
bool ewsAlertRelevantForHome(const std::vector<std::string>& codes, double homeLat, double homeLon);

} // namespace dabcore
