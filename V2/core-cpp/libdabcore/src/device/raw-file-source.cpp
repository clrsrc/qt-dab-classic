// DAB Classic v3: portiert aus Qt-DAB devices/filereaders/rawfiles-new/
// (Jan van Katwijk, GPLv2+), Qt entfernt – siehe raw-file-source.h.
#include "raw-file-source.h"
#include "dab-constants.h"

#define INPUT_FRAMEBUFFERSIZE (32 * 32768)
#define RAW_BUFFERSIZE 32768

RawFileSource::RawFileSource(const std::string& path, FileSourceOptions options)
    : FileSourceBase(path, INPUT_FRAMEBUFFERSIZE, options), rawDataBuffer_(RAW_BUFFERSIZE) {
    for (int i = 0; i < 256; i++)
        // the offset 127.38f is due to the input data comes usually from
        // an SDR stick which has its DC offset a bit shifted from ideal (from old-dab)
        mapTable_[i] = ((float)i - 127.38) / 128.0;
}

bool RawFileSource::open(std::string& error) {
    file_ = std::fopen(path_.c_str(), "rb");
    if (file_ == nullptr) { error = "kann " + path_ + " nicht oeffnen"; return false; }
    fseek(file_, 0, SEEK_END);
    int64_t fileLength = ftell(file_);
    fseek(file_, 0, SEEK_SET);
    totalSamples_ = fileLength / 2;
    sampleRate_ = SAMPLERATE;
    chunkSamples_ = RAW_BUFFERSIZE / 2;
    return true;
}

void RawFileSource::seekStart() {
    fseek(file_, 0, SEEK_SET);
}

int32_t RawFileSource::readChunk(std::complex<float>* out, int32_t maxSamples) {
    (void)maxSamples;
    int n = fread(rawDataBuffer_.data(), sizeof(uint8_t), RAW_BUFFERSIZE, file_);
    if (n <= 0) return 0;
    // v1: am Dateiende wird der Rest mit Nullen aufgefuellt
    for (int i = n; i < RAW_BUFFERSIZE; i++) rawDataBuffer_[i] = 0;
    for (int i = 0; i < RAW_BUFFERSIZE / 2; i++)
        out[i] = std::complex<float>(4 * mapTable_[rawDataBuffer_[2 * i]],
                                     4 * mapTable_[rawDataBuffer_[2 * i + 1]]);
    return RAW_BUFFERSIZE / 2;
}
