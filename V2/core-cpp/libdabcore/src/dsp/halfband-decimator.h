// DAB Classic v3 – Halbband-FIR-Dezimator 2:1 fuer den HackRF-Pfad.
//
// Befund 18.09.2026 (aktive Antenne, NRW): die niederlaendischen Muxe 12C
// und 11C liegen direkt neben den starken deutschen 12D und 11D (Abstand
// 1,712 MHz). Der HackRF tastet mit 4,096 MS/s ab; die bisherige Boxcar-
// Mittelung 2:1 daempft bei 1,5 MHz nur ~8 dB, und alles oberhalb von
// 1,024 MHz faltet sich beim Dezimieren auf 2,048 MS/s in den Nutzkanal.
// Ein 20-30 dB staerkerer Nachbar deckt den schwachen Kanal damit zu
// (flatternder Schein-Sync, nie FIBs). Der Analogfilter des MAX2837
// (1,75 MHz) hilft dort kaum.
//
// Halbband-FIR, 47 Taps (Kaiser beta 7), nur 25 Taps ungleich null:
// Durchlass bis 0,80 MHz (< 0,01 dB Welligkeit), Sperrbereich ab 1,25 MHz
// >= 70 dB. Aliasse aus 1,25..2,048 MHz landen bei 0,80..0 MHz, also im
// Nutzband, sind aber >= 70 dB gedaempft; 1,024..1,25 MHz faltet auf
// 1,024..0,80 MHz, ausserhalb der DAB-Traeger (+-0,768 MHz).
// Festkomma: Eingang int8 (HackRF), Koeffizienten /32768, Ausgang int16
// mit 128-facher Skalierung des Eingangs (Verarbeitungsgewinn bleibt
// erhalten; 127 * 128 = 16256 passt in int16).
#pragma once

#include <complex>
#include <cstdint>
#include <vector>

namespace dabcore {

class HalfbandDecimator {
public:
    static constexpr int kTaps = 47;
    static constexpr int kHalf = (kTaps - 1) / 2;      // 23
    // Ausgang = Eingang * kOutScale bei Verstaerkung 1
    static constexpr int kOutScale = 128;

    HalfbandDecimator();
    void reset();

    // iq: verschachtelt I,Q (int8), n komplexe Eingangssamples (beliebig,
    // auch ungerade; die Dezimationsphase laeuft ueber Aufrufe hinweg).
    // out muss Platz fuer n/2 + 1 Samples haben. Rueckgabe: Anzahl Ausgaben.
    int process(const int8_t* iq, int n, std::complex<int16_t>* out);

private:
    std::vector<int16_t> workI_, workQ_;   // Historie (kTaps-1) + Eingang
    int phase_ = 0;                        // Paritaet des naechsten Eingangssamples
};

} // namespace dabcore
