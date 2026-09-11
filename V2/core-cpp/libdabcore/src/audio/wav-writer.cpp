// DAB Classic v3 – RIFF/WAVE-Schreiber, siehe wav-writer.h.
#include "wav-writer.h"

#include <cstring>

namespace {
void put32(uint8_t* p, uint32_t v) { p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (v >> 24) & 255; }
void put16(uint8_t* p, uint16_t v) { p[0] = v & 255; p[1] = (v >> 8) & 255; }
}

bool WavWriter::open(const std::string& path, uint32_t rate, uint16_t channels, std::string& error) {
    close();
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) { error = "kann " + path + " nicht schreiben"; return false; }
    path_ = path; rate_ = rate; channels_ = channels; dataBytes_ = 0;
    uint8_t h[44];
    std::memcpy(h, "RIFF", 4); put32(h + 4, 0xFFFFFFFFu); std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4); put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, channels);
    put32(h + 24, rate); put32(h + 28, rate * channels * 2); put16(h + 32, channels * 2); put16(h + 34, 16);
    std::memcpy(h + 36, "data", 4); put32(h + 40, 0xFFFFFFFFu);
    std::fwrite(h, 1, 44, file_);
    return true;
}

void WavWriter::write(const int16_t* samples, uint32_t nFrames) {
    if (!file_ || nFrames == 0) return;
    size_t n = std::fwrite(samples, 2 * channels_, nFrames, file_);
    dataBytes_ += static_cast<uint64_t>(n) * channels_ * 2;
}

void WavWriter::close() {
    if (!file_) return;
    uint8_t v[4];
    if (dataBytes_ + 36 < 0xFFFFFFFFu) {
        std::fseek(file_, 4, SEEK_SET);  put32(v, static_cast<uint32_t>(dataBytes_ + 36)); std::fwrite(v, 1, 4, file_);
        std::fseek(file_, 40, SEEK_SET); put32(v, static_cast<uint32_t>(dataBytes_));      std::fwrite(v, 1, 4, file_);
    }
    std::fclose(file_);
    file_ = nullptr;
}
