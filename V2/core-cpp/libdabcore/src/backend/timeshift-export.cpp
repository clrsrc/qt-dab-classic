// DAB Classic v3 – Export eines Ringausschnitts, siehe timeshift-export.h.
#include "timeshift-export.h"

#include "aac-decoder.h"
#include "backend-callbacks.h"
#include "converter48k.h"
#include "mp4processor.h"
#include "timeshift-buffer.h"
#include "rec-format.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

namespace dabcore {

TimeshiftExportResult timeshiftExport(const TimeshiftExportJob& job, AacDecoderKind aacKind,
                                      const std::function<void(const char*, const std::string&)>& log) {
    TimeshiftExportResult res;
    res.path = job.path;
    if (job.frames == 0 || job.frameBits == 0 || job.bitRate <= 0) {
        res.error = "Timeshift-Export: kein Inhalt im gewaehlten Bereich";
        return res;
    }
    const uint32_t packedBytes = (job.frameBits + 7) / 8;
    if (job.packed.size() < static_cast<size_t>(job.frames) * packedBytes) {
        res.error = "Timeshift-Export: unvollstaendige Rahmendaten";
        return res;
    }

    std::string err;
    // Bei MP3 steht der ID3v2-Tag danach schon in der Datei, vor den Rahmen.
    std::unique_ptr<IPcmWriter> writer = makeRecWriter(job.format, job.path, 48000, 2, err);
    if (!writer) { res.error = err; return res; }

    const auto t0 = std::chrono::steady_clock::now();
    {
        converter_48000 conv;
        std::vector<float> out;
        std::vector<int16_t> pcm16;
        BackendCallbacks cb;
        cb.log = [&log](const char* level, const std::string& t) {
            if (log) log(level, "Timeshift-Export: " + t);
        };
        // PCM -> 48 kHz -> int16 (genau wie die AudioPipeline, nur ohne
        // Lautstaerke und ohne Ausgabe).
        cb.pcm = [&](const complex16* pcm, int nPairs, int rate, bool, bool, bool) {
            const int size = conv.convert(pcm, static_cast<int32_t>(nPairs), rate, out);
            if (size <= 0) return;
            pcm16.resize(static_cast<size_t>(size));
            for (int i = 0; i < size; ++i) {
                const float v = out[static_cast<size_t>(i)] * 32768.0f;
                pcm16[static_cast<size_t>(i)] = static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
            }
            writer->write(pcm16.data(), static_cast<uint32_t>(size / 2));
        };
        mp4Processor proc(job.sid, job.bitRate, &cb, aacKind);
        std::vector<uint8_t> frame(job.frameBits);
        for (uint64_t i = 0; i < job.frames; ++i) {
            TimeshiftBuffer::unpack(job.packed.data() + static_cast<size_t>(i) * packedBytes,
                                    job.frameBits, frame.data());
            proc.addtoFrame(frame);
        }
        proc.stop();
    }
    writer->close();
    res.bytes = writer->bytes();
    res.seconds = writer->seconds();
    res.ok = res.bytes > 0;
    if (!res.ok) {
        res.error = "Timeshift-Export: kein Ton dekodiert (zu kurzer Bereich?)";
        return res;
    }
    if (log) {
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double material = static_cast<double>(job.frames) * TimeshiftBuffer::kFrameSeconds;
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "%.1f s Material -> %.1f s %s in %.1f s (%.0fx Echtzeit): %s",
                      material, res.seconds, job.format.kind == "mp3" ? "MP3" : "WAV",
                      took, took > 0 ? material / took : 0.0, job.path.c_str());
        log("info", std::string("Timeshift-Export: ") + buf);
    }
    return res;
}

} // namespace dabcore
