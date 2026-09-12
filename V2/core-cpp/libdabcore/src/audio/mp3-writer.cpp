// DAB Classic v3 – MP3-Schreiber (LAME), siehe mp3-writer.h.
#include "mp3-writer.h"

#ifdef DABCORE_WITH_MP3
#if __has_include(<lame/lame.h>)
#include <lame/lame.h>
#else
#include <lame.h>
#endif
#endif

#include <algorithm>

#ifdef DABCORE_WITH_MP3
namespace {
// LAME-Empfehlung fuer die Puffergroesse je Block (1,25 * Samples + 7200).
size_t encodeBufferSize(uint32_t nFrames) { return static_cast<size_t>(nFrames) * 5 / 4 + 7200; }
}
#endif

bool Mp3Writer::open(const std::string& path, uint32_t rate, uint16_t channels, uint16_t kbps,
                     const std::vector<uint8_t>& id3, std::string& error) {
    close();
#ifndef DABCORE_WITH_MP3
    (void)rate; (void)channels; (void)kbps; (void)id3;
    error = "MP3-Aufnahme: der Kern wurde ohne LAME uebersetzt";
    return false;
#else
    if (channels != 1 && channels != 2) { error = "MP3: nur Mono oder Stereo"; return false; }
    lame_global_flags* gf = lame_init();
    if (!gf) { error = "MP3: lame_init fehlgeschlagen"; return false; }
    lame_set_in_samplerate(gf, static_cast<int>(rate));
    lame_set_out_samplerate(gf, static_cast<int>(rate));
    lame_set_num_channels(gf, channels);
    lame_set_mode(gf, channels == 2 ? JOINT_STEREO : MONO);
    lame_set_VBR(gf, vbr_off);
    lame_set_brate(gf, std::clamp<int>(kbps, 32, 320));
    lame_set_quality(gf, 2);
    // Ohne Xing/Info-Rahmen: der muesste beim Schliessen per Rueckwaertssuche
    // nachgetragen werden, und die erste Rahmenkennung waere sonst kein Ton.
    lame_set_bWriteVbrTag(gf, 0);
    if (lame_init_params(gf) < 0) {
        lame_close(gf);
        error = "MP3: lame_init_params fehlgeschlagen (Bitrate/Rate nicht moeglich)";
        return false;
    }
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) { lame_close(gf); error = "kann " + path + " nicht schreiben"; return false; }
    lame_ = gf;
    path_ = path; rate_ = rate; channels_ = channels; bytes_ = 0; pcmFrames_ = 0;
    if (!id3.empty()) bytes_ += std::fwrite(id3.data(), 1, id3.size(), file_);
    return true;
#endif
}

void Mp3Writer::write(const int16_t* samples, uint32_t nFrames) {
#ifndef DABCORE_WITH_MP3
    (void)samples; (void)nFrames;
#else
    if (!file_ || !lame_ || nFrames == 0) return;
    auto* gf = static_cast<lame_global_flags*>(lame_);
    buf_.resize(encodeBufferSize(nFrames));
    int n;
    if (channels_ == 2) {
        n = lame_encode_buffer_interleaved(gf, const_cast<short*>(samples), static_cast<int>(nFrames),
                                           buf_.data(), static_cast<int>(buf_.size()));
    } else {
        n = lame_encode_buffer(gf, samples, samples, static_cast<int>(nFrames),
                               buf_.data(), static_cast<int>(buf_.size()));
    }
    if (n > 0) bytes_ += std::fwrite(buf_.data(), 1, static_cast<size_t>(n), file_);
    pcmFrames_ += nFrames;
#endif
}

void Mp3Writer::close() {
#ifdef DABCORE_WITH_MP3
    if (lame_) {
        auto* gf = static_cast<lame_global_flags*>(lame_);
        if (file_) {
            buf_.resize(7200);
            const int n = lame_encode_flush(gf, buf_.data(), static_cast<int>(buf_.size()));
            if (n > 0) bytes_ += std::fwrite(buf_.data(), 1, static_cast<size_t>(n), file_);
        }
        lame_close(gf);
        lame_ = nullptr;
    }
#endif
    if (file_) { std::fclose(file_); file_ = nullptr; }
}
