// DAB Classic v3 – gemeinsame Basis der Dateileser, siehe file-source.h.
#include "file-source.h"

#include <chrono>
#include <vector>

FileSourceBase::FileSourceBase(const std::string& path, uint32_t ringSize, FileSourceOptions options)
    : path_(path), opt_(options), ring_(ringSize) {}

FileSourceBase::~FileSourceBase() {
    FileSourceBase::stop();
    if (file_ != nullptr) std::fclose(file_);
}

std::string FileSourceBase::serial() const {
    auto p = path_.find_last_of("/\\");
    return p == std::string::npos ? path_ : path_.substr(p + 1);
}

bool FileSourceBase::restart(int32_t frequencyHz) {
    (void)frequencyHz;
    if (running_.load()) return true;
    if (file_ == nullptr) return false;
    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
}

void FileSourceBase::stop() {
    running_.store(false);
    spaceCv_.notify_all();
    dataCv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

int32_t FileSourceBase::getSamples(std::complex<float>* buffer, int32_t n) {
    int32_t got = ring_.getDataFromBuffer(buffer, n);
    spaceCv_.notify_one();
    return got;
}

int32_t FileSourceBase::samples() {
    return static_cast<int32_t>(ring_.GetRingBufferReadAvailable());
}

bool FileSourceBase::waitForSamples(int32_t n, int timeoutMs) {
    if (samples() >= n) return true;
    std::unique_lock<std::mutex> lk(m_);
    return dataCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                            [this, n] { return samples() >= n || !running_.load(); })
           && samples() >= n;
}

// Leseschleife: entspricht xml_Reader::run / rawReader::run aus v1.
// Je Block: auf Platz im Ring warten, lesen, einstellen, Fortschritt,
// Pacing (1 Block = chunkSamples_/sampleRate_ Sekunden Echtzeit).
void FileSourceBase::run() {
    std::vector<std::complex<float>> chunk(chunkSamples_);
    using clock = std::chrono::steady_clock;
    auto nextStop = clock::now();
    const auto chunkTime = std::chrono::nanoseconds(
        static_cast<int64_t>(1e9 * chunkSamples_ / sampleRate_));
    const uint64_t progressEvery = sampleRate_ / 2;   // 2x je Dateisekunde
    const uint64_t durationSamples =
        opt_.durationS > 0 ? static_cast<uint64_t>(opt_.durationS * sampleRate_) : 0;

    seekStart();
    uint64_t samplesRead = 0;       // im aktuellen Durchlauf
    uint64_t samplesTotal = 0;      // ueber alle Durchlaeufe (fuer --duration)
    uint64_t nextProgress = 0;
    if (progressCb_) progressCb_(0.0, lengthSeconds());

    while (running_.load()) {
        // Platz im Ring abwarten (v1 rawReader: usleep-Schleife; xml_Reader
        // schrieb ungeprueft). Ohne Pacing verhindert das den Ueberlauf.
        {
            std::unique_lock<std::mutex> lk(m_);
            spaceCv_.wait_for(lk, std::chrono::milliseconds(2), [this] {
                return ring_.WriteSpace() >= chunkSamples_ + 10 || !running_.load();
            });
        }
        if (!running_.load()) break;
        if (ring_.WriteSpace() < chunkSamples_ + 10) continue;

        int32_t n = 0;
        bool endOfPass = samplesRead >= totalSamples_;
        if (!endOfPass) {
            n = readChunk(chunk.data(), chunkSamples_);
            if (n <= 0) endOfPass = true;
        }
        if (endOfPass) {
            if (!opt_.loop) break;
            if (progressCb_) progressCb_(0.0, lengthSeconds());
            seekStart();
            samplesRead = 0;
            nextProgress = 0;
            continue;
        }

        ring_.putDataIntoBuffer(chunk.data(), n);
        dataCv_.notify_one();
        samplesRead += n;
        samplesTotal += n;

        if (samplesRead >= nextProgress) {
            if (progressCb_)
                progressCb_(static_cast<double>(samplesRead) / sampleRate_, lengthSeconds());
            nextProgress += progressEvery;
        }
        if (durationSamples > 0 && samplesTotal >= durationSamples) break;

        if (!opt_.fast) {
            nextStop += chunkTime;
            auto now = clock::now();
            if (nextStop > now) std::this_thread::sleep_until(nextStop);
        }
    }
    bool ended = running_.exchange(false);
    dataCv_.notify_all();
    if (ended && endedCb_) endedCb_();
}
