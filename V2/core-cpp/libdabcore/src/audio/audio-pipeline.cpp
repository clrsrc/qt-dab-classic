// DAB Classic v3 – Audio-Thread je Dienst-Slot, siehe audio-pipeline.h.
// Die Verarbeitung je Block entspricht v1 RadioInterface::newAudio
// (radio.cpp Z. 1353-1399): 48-k-Konvertierung, Gain = volume/100, Ausgabe,
// Peak-Pegel; der WAV-Dump entspricht converter_48000::dump (int16 nach
// der Konvertierung), liegt hier aber vor der Lautstaerke.
#include "audio-pipeline.h"

#include <algorithm>
#include <cmath>

namespace dabcore {

using namespace std::chrono_literals;

AudioPipeline::AudioPipeline(Slot slot, uint32_t sid, EventSink sink, IAudioSink* audioSink)
    : slot_(slot), sid_(sid), sink_(std::move(sink)), audio_(audioSink) {
    // Der Sink wird erst mit dem ersten PCM-Block gestartet, sonst meldet
    // der PortAudio-Callback bis dahin Underruns.
    thread_ = std::thread([this] { run(); });
}

AudioPipeline::~AudioPipeline() { stop(); }

void AudioPipeline::stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!running_) return;
        running_ = false;
    }
    cv_.notify_all();
    spaceCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    stopWav();
    if (audio_ && sinkStarted_) audio_->stop();
}

void AudioPipeline::push(const complex16* pcm, int nPairs, int rate, bool ps, bool sbr, bool stereo) {
    if (!running_) return;
    if (!started_ || rate != lastRate_ || ps != lastPs_ || sbr != lastSbr_ || stereo != lastStereo_) {
        bool first = !started_;
        started_ = true; lastRate_ = rate; lastPs_ = ps; lastSbr_ = sbr; lastStereo_ = stereo;
        rate_ = rate;
        if (formatCb_) formatCb_(rate, ps, sbr, stereo, first);
    }
    // v1: der Decoder schreibt in den Ring, bei Ueberlauf gehen Samples
    // verloren. v3: bei vollem Ring kurz warten (Backpressure, wichtig fuer
    // --fast mit WAV-Dump); der Ausgabe-Sink verwirft seinerseits, wenn die
    // Soundkarte nicht nachkommt.
    {
        std::unique_lock<std::mutex> lk(m_);
        spaceCv_.wait_for(lk, 500ms, [&] {
            return !running_ || ring_.GetRingBufferWriteAvailable() >= static_cast<uint32_t>(nPairs); });
        if (!running_) return;
    }
    ring_.putDataIntoBuffer(pcm, nPairs);
    cv_.notify_one();
}

void AudioPipeline::setVolume(int percent) { volume_ = std::clamp(percent, 0, 100); }
void AudioPipeline::setMute(bool muted) { muted_ = muted; }

// Timeshift: waehrend paused/playing liefert der Ring bewusst nichts; die
// dabei im Sink auflaufenden Fehlstellen sind kein Underrun. Beim Verlassen
// des Zustands den aufgelaufenen Zaehler einmal wegwerfen.
void AudioPipeline::setStarved(bool starved) {
    const bool was = starved_.exchange(starved);
    if (was && !starved && audio_) audio_->takeMissed();
}

bool AudioPipeline::startRec(const std::string& path, const RecFormat& format, std::string& error) {
    std::lock_guard<std::mutex> lk(wavM_);
    auto w = makeRecWriter(format, path, 48000, 2, error);
    if (!w) return false;
    rec_ = std::move(w);
    recording_ = true;
    lastRecState_ = std::chrono::steady_clock::now();
    sink_(events::recordingState(slot_, sid_, true, rec_->path(), 0, 0.0));
    return true;
}

void AudioPipeline::stopWav() {
    std::lock_guard<std::mutex> lk(wavM_);
    if (!rec_ || !rec_->isOpen()) return;
    std::string p = rec_->path();
    rec_->close();
    uint64_t b = rec_->bytes();
    double s = rec_->seconds();
    rec_.reset();
    recording_ = false;
    sink_(events::recordingState(slot_, sid_, false, p, b, s));
}

void AudioPipeline::emitRecordingState(bool active) {
    std::lock_guard<std::mutex> lk(wavM_);
    if (!rec_ || !rec_->isOpen()) return;
    sink_(events::recordingState(slot_, sid_, active, rec_->path(), rec_->bytes(), rec_->seconds()));
}

void AudioPipeline::run() {
    std::vector<complex16> block;
    std::vector<float> out;
    std::vector<int16_t> wavBuf;
    while (true) {
        if (flushPending_.exchange(false)) {
            // eigener PCM-Ring (bis 1,4 s) und Ausgabepuffer des Sinks
            ring_.FlushRingBuffer();
            if (audio_) { audio_->flush(); audio_->takeMissed(); }
            quietUntil_ = std::chrono::steady_clock::now() + 1s;
            spaceCv_.notify_one();
        }
        int rate = rate_.load();
        uint32_t amount = static_cast<uint32_t>(rate / 10);   // v1: newAudio bei rate/10 Paaren
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, 50ms, [&] { return !running_ || ring_.GetRingBufferReadAvailable() >= amount; });
            if (!running_) break;
        }
        while (ring_.GetRingBufferReadAvailable() >= amount) {
            block.resize(amount);
            ring_.getDataFromBuffer(block.data(), amount);
            spaceCv_.notify_one();
            int size = converter_.convert(block.data(), static_cast<int32_t>(amount), rate, out);
            // WAV-Dump vor der Lautstaerke (int16 wie v1 converter dump)
            if (recording_) {
                wavBuf.resize(size);
                for (int i = 0; i < size; ++i) {
                    float v = out[i] * 32768.0f;
                    wavBuf[i] = static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
                }
                std::lock_guard<std::mutex> lk(wavM_);
                if (rec_) rec_->write(wavBuf.data(), static_cast<uint32_t>(size / 2));
            }
            if (audio_) {
                if (!sinkStarted_) {
                    sinkStarted_ = true;
                    audio_->start();
                    // Vorlauf: 200 ms Stille, damit der Ausgabepuffer im
                    // Betrieb nicht um Null pendelt (die Bloecke kommen
                    // superframe-weise in 120-ms-Schueben; ohne Vorlauf
                    // reisst jeder Jitter den PortAudio-Callback leer).
                    std::vector<float> silence(static_cast<size_t>(2 * 48000 / 5), 0.0f);
                    audio_->write(silence.data(), static_cast<uint32_t>(silence.size()));
                }
                int vol = volume_.load();
                if (muted_) {
                    std::fill(out.begin(), out.begin() + size, 0.0f);
                } else if (vol < 100) {
                    float gain = static_cast<float>(vol) / 100.0f;
                    for (int i = 0; i < size; ++i) out[i] *= gain;
                }
                audio_->write(out.data(), static_cast<uint32_t>(size));
                // Peak-Pegel 10 Hz (v1: alle 4 Bloecke)
                auto now = std::chrono::steady_clock::now();
                if (now - lastLevel_ >= 100ms) {
                    lastLevel_ = now;
                    float l = 0, r = 0;
                    for (int i = 0; i + 1 < size; i += 2) {
                        l = std::max(l, std::fabs(out[i]));
                        r = std::max(r, std::fabs(out[i + 1]));
                    }
                    sink_(events::audioLevel(l, r));
                    uint32_t missed = audio_->takeMissed();
                    if (missed > 0 && !starved_.load() && now >= quietUntil_)
                        sink_(events::audioUnderrun(missed));
                }
            }
            rate = rate_.load();
            amount = static_cast<uint32_t>(rate / 10);
        }
        if (recording_) {
            auto now = std::chrono::steady_clock::now();
            if (now - lastRecState_ >= 1s) {
                lastRecState_ = now;
                emitRecordingState(true);
            }
        }
    }
}

} // namespace dabcore
