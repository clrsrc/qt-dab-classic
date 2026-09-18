// DAB Classic v3 – Halbband-FIR-Dezimator, siehe halfband-decimator.h.
#include "halfband-decimator.h"

#include <cstring>

namespace dabcore {

namespace {
// Koeffizienten * 32768: Mitte und die ungeraden Taps rechts der Mitte
// (j = 1, 3, ..., 23); links spiegelbildlich, gerade Taps sind 0.
// Summe = 16382 + 2 * 8193 = 32768 (Verstaerkung genau 1).
constexpr int32_t kCenter = 16382;
constexpr int32_t kOdd[12] = {10367, -3290, 1787, -1096, 693, -433, 261, -148, 77, -35, 13, -3};
constexpr int kHist = HalfbandDecimator::kTaps - 1;   // 46
}

HalfbandDecimator::HalfbandDecimator() { reset(); }

void HalfbandDecimator::reset() {
    workI_.assign(kHist, 0);
    workQ_.assign(kHist, 0);
    phase_ = 0;
}

int HalfbandDecimator::process(const int8_t* iq, int n, std::complex<int16_t>* out) {
    if (n <= 0) return 0;
    workI_.resize(kHist + n);
    workQ_.resize(kHist + n);
    int16_t* wi = workI_.data();
    int16_t* wq = workQ_.data();
    for (int i = 0; i < n; i++) {
        wi[kHist + i] = iq[2 * i];
        wq[kHist + i] = iq[2 * i + 1];
    }
    // Ausgabe fuer jedes Eingangssample mit gerader globaler Paritaet;
    // Referenz (Mitte des Filters) ist das Sample kHalf Positionen davor.
    int count = 0;
    int i = phase_;
    for (; i < n; i += 2) {
        const int16_t* ci = wi + kHist + i - kHalf;   // Mitte: ci[0]
        const int16_t* cq = wq + kHist + i - kHalf;
        int32_t accI = kCenter * ci[0];
        int32_t accQ = kCenter * cq[0];
        for (int k = 0; k < 12; k++) {
            const int j = 2 * k + 1;
            accI += kOdd[k] * static_cast<int32_t>(ci[-j] + ci[j]);
            accQ += kOdd[k] * static_cast<int32_t>(cq[-j] + cq[j]);
        }
        // /32768 * 128 = >> 8, mit Rundung
        out[count++] = std::complex<int16_t>(
            static_cast<int16_t>((accI + 128) >> 8),
            static_cast<int16_t>((accQ + 128) >> 8));
    }
    phase_ = i - n;                                  // 0 oder 1: naechstes Ausgabesample
    std::memmove(wi, wi + n, kHist * sizeof(int16_t));
    std::memmove(wq, wq + n, kHist * sizeof(int16_t));
    return count;
}

} // namespace dabcore
