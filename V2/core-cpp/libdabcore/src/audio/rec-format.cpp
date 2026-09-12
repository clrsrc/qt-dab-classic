// DAB Classic v3 – Auswahl des Aufnahme-Schreibers, siehe rec-format.h.
#include "rec-format.h"

#include "mp3-writer.h"
#include "wav-writer.h"

std::unique_ptr<IPcmWriter> makeRecWriter(const RecFormat& fmt, const std::string& path,
                                          uint32_t rate, uint16_t channels, std::string& error) {
    if (fmt.kind == "mp3") {
        auto w = std::make_unique<Mp3Writer>();
        if (!w->open(path, rate, channels, fmt.kbps, Id3Writer::build(fmt.id3), error)) return nullptr;
        return w;
    }
    if (fmt.kind != "wav") {
        error = "Aufnahmeformat " + fmt.kind + " wird nicht unterstuetzt (wav, mp3)";
        return nullptr;
    }
    auto w = std::make_unique<WavWriter>();
    if (!w->open(path, rate, channels, error)) return nullptr;
    return w;
}
