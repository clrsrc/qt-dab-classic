// ctest halfband_decimator: Verstaerkung im Durchlass, Sperrdaempfung des
// Nachbarkanal-Alias, Gleichheit bei beliebiger Stueckelung, DC-Skalierung.
#include "halfband-decimator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using dabcore::HalfbandDecimator;

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FEHLER %s:%d: %s\n", __FILE__, __LINE__, msg); failures++; } } while (0)

static const double kFs = 4.096e6;
static const double kPi = 3.14159265358979323846;

static std::vector<int8_t> tone(double hz, double amp, int n) {
    std::vector<int8_t> v(2 * n);
    for (int i = 0; i < n; i++) {
        double ph = 2.0 * kPi * hz * i / kFs;
        v[2 * i]     = static_cast<int8_t>(std::lround(amp * std::cos(ph)));
        v[2 * i + 1] = static_cast<int8_t>(std::lround(amp * std::sin(ph)));
    }
    return v;
}

// Ausgang in Stuecken der Groesse `chunk` (0 = alles auf einmal)
static std::vector<std::complex<int16_t>> run(const std::vector<int8_t>& in, int chunk) {
    HalfbandDecimator d;
    int n = static_cast<int>(in.size() / 2);
    std::vector<std::complex<int16_t>> out, tmp(n / 2 + 2);
    if (chunk <= 0) chunk = n;
    for (int pos = 0; pos < n; ) {
        int len = std::min(chunk, n - pos);
        int got = d.process(in.data() + 2 * pos, len, tmp.data());
        out.insert(out.end(), tmp.begin(), tmp.begin() + got);
        pos += len;
    }
    return out;
}

// Leistung des Tons bei fHz (Ausgangsrate 2,048 MS/s) im DFT-Bin mit
// Hann-Fenster, in dB re 1 (Skalierung kOutScale herausgerechnet). Das
// isoliert den Ton vom breitbandigen 8-Bit-Quantisierungsrauschen des
// Testsignals (Gesamtleistung wuerde bei ~50 dB deckeln).
static double toneDb(const std::vector<std::complex<int16_t>>& v, double fHz, size_t skip) {
    const double fsOut = kFs / 2.0;
    const size_t n = v.size() - skip;
    std::complex<double> acc(0, 0);
    double wsum = 0;
    for (size_t i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (n - 1));
        double ph = -2.0 * kPi * fHz * i / fsOut;
        acc += w * std::complex<double>(v[skip + i].real(), v[skip + i].imag()) * std::complex<double>(std::cos(ph), std::sin(ph));
        wsum += w;
    }
    double amp = std::abs(acc) / wsum / HalfbandDecimator::kOutScale;
    return 20.0 * std::log10(amp + 1e-12);
}

int main() {
    const int n = 40000;
    // Durchlass: Ton bei 0,5 MHz, Amplitude 100 -> Leistung 40 dB (re 1)
    auto pass = run(tone(0.5e6, 100.0, n), 0);
    CHECK(pass.size() == n / 2, "2:1 Dezimation");
    double pdb = toneDb(pass, 0.5e6, 100);
    CHECK(std::fabs(pdb - 40.0) < 0.1, ("Durchlass 0,5 MHz: " + std::to_string(pdb) + " dB, erwartet 40").c_str());
    // Nachbarkanal-Alias: Toene bei 1,3 / 1,712 / 2,0 MHz erscheinen nach der
    // Dezimation bei f - 2,048 MHz und muessen >= 65 dB unter dem Durchlass liegen
    for (double f : {1.3e6, 1.712e6, 2.0e6}) {
        auto stop = run(tone(f, 100.0, n), 0);
        double sdb = toneDb(stop, f - kFs / 2.0, 100);
        CHECK(pdb - sdb >= 65.0, ("Sperrbereich " + std::to_string(f / 1e6) + " MHz: " + std::to_string(pdb - sdb) + " dB").c_str());
        std::printf("Sperrbereich %.3f MHz: %.1f dB\n", f / 1e6, pdb - sdb);
    }
    // Stueckelung: gleiche Ausgabe bei 1, 7, 1000 Samples je Aufruf
    auto ref = run(tone(0.3e6, 90.0, 6000), 0);
    for (int chunk : {1, 7, 1000}) {
        auto v = run(tone(0.3e6, 90.0, 6000), chunk);
        CHECK(v == ref, ("Stueckelung " + std::to_string(chunk) + " liefert dieselbe Ausgabe").c_str());
    }
    // DC: konstant 100 -> 100 * 128
    std::vector<int8_t> dc(2 * 400, 100);
    auto o = run(dc, 0);
    CHECK(o.back().real() == 100 * HalfbandDecimator::kOutScale && o.back().imag() == 100 * HalfbandDecimator::kOutScale, "DC-Skalierung 128");
    // Vollaussteuerung passt in int16
    std::vector<int8_t> full(2 * 400, 127);
    auto of = run(full, 0);
    CHECK(of.back().real() == 127 * HalfbandDecimator::kOutScale, "Vollaussteuerung 127 -> 16256");
    if (failures) { std::printf("%d Fehler\n", failures); return 1; }
    std::printf("OK\n");
    return 0;
}
