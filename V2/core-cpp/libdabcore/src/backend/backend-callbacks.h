// DAB Classic v3 – Callbacks des MSC-/Backend-Pfads (Ersatz fuer die
// Qt-Signale von mp4Processor, faadDecoder/fdkAAC, padHandler und motObject,
// die in v1 an RadioInterface gingen). Alle Aufrufe kommen aus dem
// Backend-Thread des jeweiligen Dienstes; DabCore setzt je laufendem Dienst
// eine Instanz (Slot und SId sind in den Closures gebunden).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "dab-constants.h"

struct BackendCallbacks {
    // Dekodierte PCM-Bloecke (Paare L/R als complex16, wie v1 audioBuffer);
    // rate/ps/sbr vom Decoder, stereo aus den Superframe-Parametern.
    std::function<void(const complex16* pcm, int nPairs, int rate, bool ps, bool sbr, bool stereo)> pcm;
    // Eine AAC-Zugriffseinheit als LOAS/LATM-Rahmen (v1 build_aacFile) –
    // Frame-Dump und spaeter AAC-Passthrough/Timeshift.
    std::function<void(const uint8_t* loas, int len)> aacFrame;
    // Fehlerzaehler seit dem letzten Aufruf, etwa 1 Hz (Wanduhr).
    std::function<void(int frameErrors, int rsErrors, int aacErrors, int rsCorrections)> stats;
    // padHandler: vollstaendiges Dynamic Label (UTF-8)
    std::function<void(const std::string& text)> dls;
    // padHandler: DL+-Kommando (TS 102 980) mit allen Tags (contentType, Text)
    std::function<void(bool itemToggle, bool itemRunning,
                       const std::vector<std::pair<uint8_t, std::string>>& tags)> dlPlus;
    // motObject (X-PAD-Slide oder Paketdienst): fertiges Objekt
    std::function<void(const std::vector<uint8_t>& data, const std::string& name,
                       int contentType, bool dirElement, uint32_t sid)> motObject;
    // Diagnose (v1: fprintf (stderr, ...))
    std::function<void(const char* level, const std::string& text)> log;
};

template <class F, class... A>
inline void emitBe(const F& f, A&&... a) {
    if (f) f(std::forward<A>(a)...);
}
