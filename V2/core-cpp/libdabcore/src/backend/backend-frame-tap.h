// DAB Classic v3 – Anzapfpunkt zwischen Backend und backendDriver
// (Plan M4 1.1). Ohne Tap gibt Backend::processSegment jeden Hardbit-Rahmen
// direkt an den Driver; mit gesetztem Tap bekommt ihn stattdessen der Tap
// (Timeshift-Ring) und reicht ihn ueber Backend::deliverFrame zurueck –
// sofort (live) oder verzoegert (playing).
#pragma once

#include <cstdint>
#include <vector>

class IFrameTap {
public:
    virtual ~IFrameTap() = default;
    // Aufruf aus dem Backend-Thread des Dienstes.
    virtual void onBackendFrame(const std::vector<uint8_t>& hardBits) = 0;
};
