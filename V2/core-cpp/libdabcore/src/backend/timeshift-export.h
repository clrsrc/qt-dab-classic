// DAB Classic v3 – Export eines Ringausschnitts als WAV (Plan M4 1.3/1.6).
//
// Die kopierten (gepackten) Hardbit-Rahmen laufen durch eine ZWEITE
// mp4Processor-Instanz – Firecode, Reed-Solomon, AAC wie im Empfang, aber
// ohne Audio-Ausgabe und so schnell, wie die CPU mag. Senke ist der
// 48-kHz-Konverter und der WAV-Schreiber.
//
// Der Aufrufer ruft dies in einem eigenen Thread auf (die Funktion blockiert).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

enum class AacDecoderKind;

namespace dabcore {

struct TimeshiftExportJob {
    std::vector<uint8_t> packed;   // Rahmen hintereinander, je (frameBits+7)/8 Byte
    uint32_t frameBits = 0;        // 24 * bitRate
    uint64_t frames = 0;
    uint32_t sid = 0;
    int16_t bitRate = 0;
    std::string path;
};

struct TimeshiftExportResult {
    bool ok = false;
    std::string error;
    std::string path;
    uint64_t bytes = 0;
    double seconds = 0.0;
};

TimeshiftExportResult timeshiftExportWav(const TimeshiftExportJob& job, AacDecoderKind aacKind,
                                         const std::function<void(const char*, const std::string&)>& log);

} // namespace dabcore
