// ctest ews_location: Ortscodes der FIG 0/15 (ETSI TS 104 089 Annex F) als
// reine Rechnung ohne FIC. Die Erwartungswerte sind dieselben wie im
// Rust-Zwilling crates/dab-app/src/ews_location.rs (dort an den beiden
// Rechenbeispielen der Norm geprueft); diese Portierung muss sie treffen.
#include "ews-location.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using dabcore::decodeEwsLocation;
using dabcore::ewsAlertRelevantForHome;
using dabcore::haversineKm;

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FEHLER %s:%d: %s\n", __FILE__, __LINE__, msg); failures++; } } while (0)

// Heimatpositionen der Tests
static const double kDuesseldorfLat = 51.2180, kDuesseldorfLon = 6.7617;   // Sendestandort Warntag
static const double kEiffelLat = 48.8584, kEiffelLon = 2.2945;             // Eiffelturm
static const double kLissabonLat = 38.7223, kLissabonLon = -9.1393;        // klar ausserhalb

// Ortscodes des Warntag-2026-Mitschnitts (Bundesmux 5C), Trigger-Phase.
static const std::vector<std::string> kWarntagTrigger = {
    "Z1:5C+F300", "Z1:95+7FFF", "Z1:86+88CC", "Z1:92+7733", "Z1:9+0113"};

// Synthetischer Code fuer den "Eiffelturm"-Funktionstest: die Position des
// Eiffelturms nach Annex F.3/F.4 kodiert (Zone 1, Ziffern 8-9-4 = ~0,56 Grad
// Kachel um 48,66 N / 2,53 O). Ein echter Mitschnitt des Funktionstests liegt
// hier nicht vor; fuer den Geofencing-Test reicht der gerechnete Code.
static const std::vector<std::string> kEiffelTest = {"Z1:894"};

static void testStandardExamples() {
    // Annex F.4, Rechenbeispiel BBC Broadcasting House, London.
    auto bbc = decodeEwsLocation("Z10:B736BB");
    CHECK(bbc.has_value(), "Z10:B736BB sollte dekodierbar sein");
    if (bbc) {
        auto [lat, lon, r] = *bbc;
        CHECK(std::fabs(lat - 51.5187412) < 0.01, "BBC: Breite");
        CHECK(std::fabs(lon - (-0.1434571)) < 0.01, "BBC: Laenge");
        CHECK(r > 0.0 && r < 2.0, "BBC: 6 Ziffern -> Kachel unter 1 km Kantenlaenge");
    }
    // Annex F.5, Rechenbeispiel Svalbard Museum, Longyearbyen (Nordpol-Zone).
    auto sval = decodeEwsLocation("Z0:152FF1");
    CHECK(sval.has_value(), "Z0:152FF1 sollte dekodierbar sein");
    if (sval) {
        auto [lat, lon, r] = *sval;
        CHECK(std::fabs(lat - 78.222609) < 0.01, "Svalbard: Breite");
        CHECK(std::fabs(lon - 15.651605) < 0.02, "Svalbard: Laenge");
        CHECK(r > 0.0, "Svalbard: Radius positiv");
    }
    // Suedpol-Zone, aeusserer Ring: nur Plausibilitaet (kein Beispiel der Norm).
    auto south = decodeEwsLocation("Z41:5");
    CHECK(south.has_value(), "Z41:5 sollte dekodierbar sein");
    if (south) CHECK(std::get<0>(*south) < -70.0, "Z41:5 sollte nahe am Suedpol liegen");
    std::printf("Rechenbeispiele der Norm: ok\n");
}

static void testWarntagCodes() {
    for (const auto& code : {"Z1:5C+F300", "Z1:95+7FFF", "Z1:86+88CC", "Z1:9+0113", "Z1:8"}) {
        auto d = decodeEwsLocation(code);
        CHECK(d.has_value(), code);
        if (!d) continue;
        auto [lat, lon, r] = *d;
        CHECK(lat >= 35.0 && lat < 72.0, "Warntag-Code ausserhalb Zone 1 (Breite)");
        CHECK(lon >= 0.0 && lon < 36.0, "Warntag-Code ausserhalb Zone 1 (Laenge)");
        CHECK(r > 0.0, "Warntag-Code: Radius positiv");
    }
    // Der Subcode (+XXXX) verfeinert nur und darf das Ergebnis nicht aendern.
    auto with = decodeEwsLocation("Z1:5C+F300");
    auto without = decodeEwsLocation("Z1:5C");
    CHECK(with.has_value() && without.has_value() && *with == *without, "Subcode wird ignoriert");
    std::printf("Warntag-2026-Codes: ok\n");
}

static void testMalformed() {
    CHECK(!decodeEwsLocation("nonsense").has_value(), "Text ohne Z/: -> kein Ergebnis");
    CHECK(!decodeEwsLocation("Z1:").has_value(), "keine Ziffern -> kein Ergebnis");
    CHECK(!decodeEwsLocation("Z99:5C").has_value(), "Zone ausserhalb 0..41 -> kein Ergebnis");
    CHECK(!decodeEwsLocation("Z1:GG").has_value(), "kein Hex -> kein Ergebnis");
    CHECK(!decodeEwsLocation("").has_value(), "leer -> kein Ergebnis");
    CHECK(!decodeEwsLocation("Z0:0").has_value(), "Polzone, Sektor 0 gibt es nicht");
    std::printf("Fehlerhafte Codes: ok\n");
}

static void testHaversine() {
    // Duesseldorf -> Langenberg, wie im Rust-Test von dab_app::tii.
    double d = haversineKm(51.217964, 6.761675, 51.356256, 7.134128);
    CHECK(d > 29.0 && d < 31.5, "Haversine Duesseldorf-Langenberg ~30 km");
    CHECK(haversineKm(51.0, 7.0, 51.0, 7.0) < 1e-9, "gleiche Position -> 0 km");
    std::printf("Haversine: ok\n");
}

static void testGeofencing() {
    // Der echte Warntag-Alarm deckt Deutschland ab.
    CHECK(ewsAlertRelevantForHome(kWarntagTrigger, kDuesseldorfLat, kDuesseldorfLon),
          "Warntag-Alarm muss fuer Duesseldorf relevant sein");
    CHECK(!ewsAlertRelevantForHome(kWarntagTrigger, kLissabonLat, kLissabonLon),
          "Warntag-Alarm darf fuer Lissabon nicht relevant sein");
    // Der Funktionstest um den Eiffelturm: nur dort relevant.
    CHECK(ewsAlertRelevantForHome(kEiffelTest, kEiffelLat, kEiffelLon),
          "Eiffelturm-Test muss am Eiffelturm relevant sein");
    CHECK(!ewsAlertRelevantForHome(kEiffelTest, kDuesseldorfLat, kDuesseldorfLon),
          "Eiffelturm-Test darf in Duesseldorf nicht relevant sein");
    // Ohne brauchbare Gebietsangabe gibt es keine Einschraenkung: relevant.
    CHECK(ewsAlertRelevantForHome({}, kDuesseldorfLat, kDuesseldorfLon),
          "leere Codeliste (z. B. End-Phase) -> relevant");
    CHECK(ewsAlertRelevantForHome({"quatsch", "Z99:1"}, kDuesseldorfLat, kDuesseldorfLon),
          "nur unlesbare Codes -> relevant");
    // Ein unlesbarer Code neben einem lesbaren zaehlt nicht gegen die Relevanz.
    CHECK(!ewsAlertRelevantForHome({"quatsch", "Z1:894"}, kDuesseldorfLat, kDuesseldorfLon),
          "unlesbarer Code macht einen fremden Alarm nicht relevant");
    std::printf("Geofencing: ok\n");
}

int main() {
    testStandardExamples();
    testWarntagCodes();
    testMalformed();
    testHaversine();
    testGeofencing();
    if (failures) { std::printf("%d Fehler\n", failures); return 1; }
    std::printf("OK\n");
    return 0;
}
