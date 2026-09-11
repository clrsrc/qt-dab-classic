// DAB Classic v3 – Schnittstelle fuer den MSC-Pfad (Ersatz fuer die
// direkte Einbettung von mscHandler im ofdmHandler). Der ofdmHandler
// uebergibt die Soft-Bits der Bloecke 4..75 je Rahmen; Spike 2 haengt
// eine No-op-Senke an, der portierte mscHandler folgt in M0.
#pragma once

#include <cstdint>
#include <vector>

class IMscSink {
public:
    virtual ~IMscSink() = default;
    // Soft-Bits eines OFDM-Symbols (2 * carriers Werte), blkno 4 .. L-1
    virtual void processMscBlock(const std::vector<int16_t>& softbits, int blkno) = 0;
    // Kanalwechsel: Zustand verwerfen (v1 mscHandler::resetChannel)
    virtual void resetChannel() = 0;
    virtual void stop() = 0;
};

class NullMscSink : public IMscSink {
public:
    void processMscBlock(const std::vector<int16_t>&, int) override {}
    void resetChannel() override {}
    void stop() override {}
};
