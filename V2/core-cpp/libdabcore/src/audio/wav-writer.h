// DAB Classic v3 – einfacher RIFF/WAVE-Schreiber (PCM 16 Bit, Stereo),
// Ersatz fuer riffWriter aus Qt-DAB (QString/QFile). Groessenfelder werden
// beim Schliessen nachgetragen; bei > 4 GB bleibt der Header bei 0xFFFFFFFF.
#pragma once

#include "pcm-writer.h"

#include <cstdint>
#include <cstdio>
#include <string>

class WavWriter : public IPcmWriter {
public:
    WavWriter() = default;
    ~WavWriter() override { close(); }
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    bool open(const std::string& path, uint32_t rate, uint16_t channels, std::string& error);
    // nFrames Rahmen (channels Werte je Rahmen)
    void write(const int16_t* samples, uint32_t nFrames) override;
    void close() override;
    bool isOpen() const override { return file_ != nullptr; }
    uint64_t bytes() const override { return dataBytes_; }
    double seconds() const override { return rate_ ? static_cast<double>(dataBytes_) / (rate_ * channels_ * 2) : 0.0; }
    const std::string& path() const override { return path_; }

private:
    FILE* file_ = nullptr;
    std::string path_;
    uint32_t rate_ = 0;
    uint16_t channels_ = 0;
    uint64_t dataBytes_ = 0;
};
