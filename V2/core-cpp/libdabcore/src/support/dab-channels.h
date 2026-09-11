// DAB Classic v3 – Kanaltabelle Band III (v1 support/scan-handler.cpp
// frequencies_1; EN 300 401 / TR 101 496). L-Band entfaellt (Streichliste).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct DabChannel {
    const char* name;
    int32_t     kHz;
};

// Alle 38 Band-III-Kanaele 5A .. 13F in Scan-Reihenfolge.
const std::vector<DabChannel>& bandIIIChannels();

// Mittenfrequenz in Hz; 0, wenn der Kanal unbekannt ist (Vergleich ohne
// Beachtung von Gross-/Kleinschreibung).
int32_t channelFrequencyHz(const std::string& channel);

// Kanalname zu einer Frequenz (Toleranz +-100 kHz); leer, wenn keiner passt.
std::string channelForFrequency(int32_t hz);
