// DAB Classic v3: portiert aus Qt-DAB devices/filereaders/rawfiles-new/
// (rawfiles, raw-reader; Jan van Katwijk, GPLv2+), Qt entfernt: QThread
// durch FileSourceBase ersetzt, kein Widget. Rohe 8-Bit-IQ-Dateien
// (osmocom/rtl_sdr-Format), Mapping 4 * (i - 127.38) / 128 wie v1.
#pragma once

#include "file-source.h"

class RawFileSource : public FileSourceBase {
public:
    RawFileSource(const std::string& path, FileSourceOptions options);
    ~RawFileSource() override = default;

    bool open(std::string& error);
    std::string name() const override { return "raw-file"; }
    int16_t bitDepth() const override { return 8; }

protected:
    void seekStart() override;
    int32_t readChunk(std::complex<float>* out, int32_t maxSamples) override;

private:
    float mapTable_[256];
    std::vector<uint8_t> rawDataBuffer_;
};
