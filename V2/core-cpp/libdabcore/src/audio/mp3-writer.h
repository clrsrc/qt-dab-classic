// DAB Classic v3 – MP3-Schreiber (LAME), Gegenstueck zu WavWriter.
// Eingang wie im ganzen Audiopfad: 48 kHz, Stereo, int16 interleaved;
// Bitrate konfigurierbar (Standard 192 kbit/s, Entscheidung 6). Ein fertiger
// ID3v2-Tag kann beim Oeffnen vor die MP3-Rahmen gesetzt werden.
#pragma once

#include "pcm-writer.h"

#include <cstdio>
#include <vector>

class Mp3Writer : public IPcmWriter {
public:
    Mp3Writer() = default;
    ~Mp3Writer() override { close(); }
    Mp3Writer(const Mp3Writer&) = delete;
    Mp3Writer& operator=(const Mp3Writer&) = delete;

    bool open(const std::string& path, uint32_t rate, uint16_t channels, uint16_t kbps,
              const std::vector<uint8_t>& id3, std::string& error);

    void write(const int16_t* samples, uint32_t nFrames) override;
    void close() override;
    bool isOpen() const override { return file_ != nullptr; }
    uint64_t bytes() const override { return bytes_; }
    // Dauer aus den eingespeisten PCM-Rahmen (die Dateigroesse haengt am Encoder)
    double seconds() const override { return rate_ ? static_cast<double>(pcmFrames_) / rate_ : 0.0; }
    const std::string& path() const override { return path_; }

private:
    void* lame_ = nullptr;   // lame_global_flags*, Typ nur in der .cpp
    FILE* file_ = nullptr;
    std::string path_;
    uint32_t rate_ = 0;
    uint16_t channels_ = 0;
    uint64_t bytes_ = 0;
    uint64_t pcmFrames_ = 0;
    std::vector<uint8_t> buf_;
};
