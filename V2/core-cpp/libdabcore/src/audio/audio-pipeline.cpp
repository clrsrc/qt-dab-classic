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

namespace {
constexpr int SPEC_FFT = 1024;    // Crossmixer SpectrumAnalyzer fftOrder 10
constexpr int SPEC_BANDS = 48;
}

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
    std::lock_guard<std::mutex> lk(sinkM_);
    if (audio_ && sinkStarted_) audio_->stop();
    audio_ = nullptr;
    sinkStarted_ = false;
}

bool AudioPipeline::detachSink() {
    std::lock_guard<std::mutex> lk(sinkM_);
    const bool was = audio_ != nullptr && sinkStarted_;
    audio_ = nullptr;
    sinkStarted_ = false;
    handover_ = false;
    return was;
}

void AudioPipeline::attachSink(IAudioSink* sink, bool running) {
    std::lock_guard<std::mutex> lk(sinkM_);
    audio_ = sink;
    sinkStarted_ = sink != nullptr && running;
    handover_ = sinkStarted_;
    lastLevel_ = {};
}

bool AudioPipeline::hasSink() {
    std::lock_guard<std::mutex> lk(sinkM_);
    return audio_ != nullptr;
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
    if (was && !starved) {
        std::lock_guard<std::mutex> lk(sinkM_);
        if (audio_) audio_->takeMissed();
    }
}

bool AudioPipeline::startRec(const std::string& path, const RecFormat& format, std::string& error) {
    std::lock_guard<std::mutex> lk(wavM_);
    auto w = makeRecWriter(format, path, 48000, 2, error);
    if (!w) return false;
    rec_ = std::move(w);
    recording_ = true;
    lastRecState_ = std::chrono::steady_clock::now();
    sink_(events::recordingState(slot_.load(), sid_, true, rec_->path(), 0, 0.0));
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
    sink_(events::recordingState(slot_.load(), sid_, false, p, b, s));
}

void AudioPipeline::emitRecordingState(bool active) {
    std::lock_guard<std::mutex> lk(wavM_);
    if (!rec_ || !rec_->isOpen()) return;
    sink_(events::recordingState(slot_.load(), sid_, active, rec_->path(), rec_->bytes(), rec_->seconds()));
}

// Vorlauf: 200 ms Stille, damit der Ausgabepuffer im Betrieb nicht um Null
// pendelt (die Bloecke kommen superframe-weise in 120-ms-Schueben; ohne
// Vorlauf reisst jeder Jitter den PortAudio-Callback leer).
void AudioPipeline::writePrelude() {
    std::vector<float> silence(static_cast<size_t>(2 * 48000 / 5), 0.0f);
    audio_->write(silence.data(), static_cast<uint32_t>(silence.size()));
}

// Ein Abschnitt PCM (L/R verschachtelt, 48 kHz) -> 48 logarithmische Baender
// 40 Hz..16 kHz wie Crossmixer SpectrumAnalyzer::timerCallback: Mono-Summe,
// Hann, FFT 1024, je Band das Maximum, dB bezogen auf Vollaussteuerung
// (Sinus mit Amplitude 1 -> 0 dB), u8 in 0,5-dB-Stufen ab -90 dB.
void AudioPipeline::emitSpectrum(const float* pcm, int frames) {
    if (frames < SPEC_FFT) return;
    if (!fft_) {
        fft_ = std::make_unique<fftHandler>(SPEC_FFT, false);
        fftBuf_.resize(SPEC_FFT);
        hann_.resize(SPEC_FFT);
        for (int i = 0; i < SPEC_FFT; ++i)
            hann_[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * static_cast<float>(i) / (SPEC_FFT - 1));
    }
    const float* p = pcm + 2 * (frames - SPEC_FFT);   // die letzten 1024 Rahmen des Abschnitts
    for (int i = 0; i < SPEC_FFT; ++i)
        fftBuf_[i] = Complex(0.5f * (p[2 * i] + p[2 * i + 1]) * hann_[i], 0.0f);
    fft_->fft(fftBuf_);
    const float binHz = 48000.0f / SPEC_FFT;
    const float fMin = 40.0f, fMax = 16000.0f;
    const float logRange = std::log(fMax / fMin);
    std::vector<uint8_t> bands(SPEC_BANDS);
    for (int b = 0; b < SPEC_BANDS; ++b) {
        const float f0 = fMin * std::exp(logRange * static_cast<float>(b) / SPEC_BANDS);
        const float f1 = fMin * std::exp(logRange * static_cast<float>(b + 1) / SPEC_BANDS);
        const int k0 = std::max(1, static_cast<int>(std::floor(f0 / binHz)));
        const int k1 = std::min(SPEC_FFT / 2, std::max(k0 + 1, static_cast<int>(std::ceil(f1 / binHz))));
        float m = 0.0f;
        for (int k = k0; k < k1; ++k) m = std::max(m, std::abs(fftBuf_[k]));
        m /= SPEC_FFT / 4.0f;   // Hann halbiert die Amplitude: Vollaussteuerung -> 0 dB
        const float db = m > 1e-6f ? 20.0f * std::log10(m) : -120.0f;
        bands[b] = static_cast<uint8_t>(std::clamp(std::lround((db + 90.0f) * 2.0f), 0L, 255L));
    }
    sink_(events::audioSpectrum(bands));
}

void AudioPipeline::run() {
    std::vector<complex16> block;
    std::vector<float> out;
    std::vector<int16_t> wavBuf;
    while (true) {
        if (flushPending_.exchange(false)) {
            // eigener PCM-Ring (bis 1,4 s) und Ausgabepuffer des Sinks
            ring_.FlushRingBuffer();
            {
                std::lock_guard<std::mutex> lk(sinkM_);
                if (audio_) { audio_->flush(); audio_->takeMissed(); }
            }
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
            // Vordecodierter Hintergrunddienst ohne Aufnahme: nur den Ring
            // leeren, die 48-k-Konvertierung spart sich der Thread (das
            // Backend hat den Superframe schon dekodiert, mehr braucht die
            // Vorhaltung nicht).
            if (!recording_ && !hasSink()) {
                rate = rate_.load();
                amount = static_cast<uint32_t>(rate / 10);
                continue;
            }
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
            std::lock_guard<std::mutex> sl(sinkM_);
            if (audio_) {
                if (!sinkStarted_) {
                    sinkStarted_ = true;
                    handover_ = false;
                    audio_->start();
                    audio_->takeMissed();   // Luecke vor dem Start zaehlt nicht
                    writePrelude();
                } else if (handover_) {
                    // Ausgabe vom Vorgaenger uebernommen: dessen Rest im
                    // Ausgabepuffer (bis ~0,7 s) verwerfen, dann wie beim
                    // Start mit Vorlauf weiter.
                    handover_ = false;
                    audio_->flush();
                    audio_->takeMissed();
                    writePrelude();
                    quietUntil_ = std::chrono::steady_clock::now() + 1s;
                }
                // Equalizer-Anzeige vor Lautstaerke/Mute (zeigt das Programm,
                // nicht den Regler): zwei Abschnitte je Block -> ~20 Hz
                if (spectrumOn_ && spectrumOn_->load()) {
                    const int frames = size / 2;
                    emitSpectrum(out.data(), frames / 2);
                    emitSpectrum(out.data() + 2 * (frames / 2), frames - frames / 2);
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
