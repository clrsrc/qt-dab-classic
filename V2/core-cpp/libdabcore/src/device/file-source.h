// DAB Classic v3 – gemeinsame Basis der Dateileser (Ersatz fuer die
// QThread-Leser xml_Reader / rawReader aus Qt-DAB, Jan van Katwijk, GPLv2+).
// Ein eigener Thread liest Bloecke aus der Datei in einen RingBuffer;
// Pacing in Echtzeit wie v1 (1 ms je Block) oder ohne Pacing ("fast").
// Der Verbraucher wartet per Condition-Variable statt usleep-Polling.
#pragma once

#include "isample-source.h"
#include "ringbuffer.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// 64-Bit-Dateipositionen (Mitschnitte > 2 GB; fseek/ftell mit long reichen
// unter Windows nicht).
#ifdef _WIN32
inline int     fileSeek(FILE* f, int64_t off, int whence) { return _fseeki64(f, off, whence); }
inline int64_t fileTell(FILE* f) { return _ftelli64(f); }
#else
inline int     fileSeek(FILE* f, int64_t off, int whence) { return fseeko(f, off, whence); }
inline int64_t fileTell(FILE* f) { return ftello(f); }
#endif

struct FileSourceOptions {
    bool   loop = false;      // am Dateiende von vorn beginnen
    bool   fast = false;      // kein Echtzeit-Pacing
    double durationS = 0;     // > 0: nach so vielen Sekunden Dateizeit enden
};

class FileSourceBase : public ISampleSource {
public:
    FileSourceBase(const std::string& path, uint32_t ringSize, FileSourceOptions options);
    ~FileSourceBase() override;

    bool restart(int32_t frequencyHz) override;
    void stop() override;
    int32_t getSamples(std::complex<float>* buffer, int32_t n) override;
    int32_t samples() override;
    bool waitForSamples(int32_t n, int timeoutMs) override;
    bool isFileInput() const override { return true; }
    std::string serial() const override;

    // Fortschritt (Sekunden Dateizeit: Position, Laenge), etwa 2x je Dateisekunde.
    void setProgressCallback(std::function<void(double, double)> cb) { progressCb_ = std::move(cb); }
    // Dateiende erreicht (ohne loop) bzw. Dauer abgelaufen.
    void setEndedCallback(std::function<void()> cb) { endedCb_ = std::move(cb); }

    bool running() const { return running_.load(); }
    double lengthSeconds() const { return static_cast<double>(totalSamples_) / sampleRate_; }

protected:
    // Von der Unterklasse: Datei an den Datenanfang setzen.
    virtual void seekStart() = 0;
    // Einen Block lesen; liefert die Anzahl der Samples (0 = Dateiende).
    virtual int32_t readChunk(std::complex<float>* out, int32_t maxSamples) = 0;

    std::string path_;
    FILE*       file_ = nullptr;
    uint64_t    totalSamples_ = 0;    // je Durchlauf
    int32_t     sampleRate_ = 2048000;
    int32_t     chunkSamples_ = 2048; // Samples je Leseblock

private:
    void run();

    FileSourceOptions opt_;
    RingBuffer<std::complex<float>> ring_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex m_;
    std::condition_variable dataCv_;   // Leser -> Verbraucher: neue Daten
    std::condition_variable spaceCv_;  // Verbraucher -> Leser: Platz frei
    std::function<void(double, double)> progressCb_;
    std::function<void()> endedCb_;
};
