// DAB Classic v3 – gemeinsame Sicht auf die Aufnahme-Schreiber (WAV, MP3).
// AudioPipeline und Timeshift-Export kennen nur diese Schnittstelle; welches
// Format entsteht, entscheidet der Aufrufer beim Anlegen (makeRecWriter).
#pragma once

#include <cstdint>
#include <string>

class IPcmWriter {
public:
    virtual ~IPcmWriter() = default;
    // nFrames Rahmen, je `channels` interleavte int16-Werte
    virtual void write(const int16_t* samples, uint32_t nFrames) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    // Nutzdaten in der Datei (WAV: PCM ohne Header, MP3: Tag + Rahmen)
    virtual uint64_t bytes() const = 0;
    virtual double seconds() const = 0;
    virtual const std::string& path() const = 0;
};
