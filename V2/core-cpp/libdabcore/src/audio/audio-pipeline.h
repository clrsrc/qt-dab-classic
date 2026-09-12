// DAB Classic v3 – Audio-Thread je Dienst-Slot (Entscheidung 3, Analyse 7.2).
// Ersatz fuer RadioInterface::newAudio aus Qt-DAB (dort lief Audio durch
// den GUI-Thread): PCM-Ring vom Decoder -> converter_48000 -> WAV-Dump
// (vor der Lautstaerke, also unabhaengig vom Regler) -> Lautstaerke/Mute ->
// Peak-Pegel (10 Hz) -> IAudioSink. Hintergrund-Slots haben keinen Sink
// (nur Aufnahme).
#pragma once

#include "dabcore/events.h"
#include "dab-constants.h"
#include "ringbuffer.h"
#include "converter48k.h"
#include "wav-writer.h"
#include "audio-sink.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dabcore {

class AudioPipeline {
public:
    // sink darf nullptr sein (Hintergrund / --no-audio ohne Sink).
    AudioPipeline(Slot slot, uint32_t sid, EventSink sink, IAudioSink* audioSink);
    ~AudioPipeline();

    // Aus dem Backend-Thread (Decoder-Callback).
    void push(const complex16* pcm, int nPairs, int rate, bool ps, bool sbr, bool stereo);
    // Wird beim ersten Block und bei Formatwechsel aufgerufen (Backend-Thread).
    void setFormatHandler(std::function<void(int rate, bool ps, bool sbr, bool stereo, bool first)> h) { formatCb_ = std::move(h); }

    void setVolume(int percent);
    void setMute(bool muted);

    bool startWav(const std::string& path, std::string& error);
    void stopWav();
    bool recording() const { return recording_.load(); }
    // Stand der laufenden Aufnahme (fuer state_snapshot)
    std::string recordingPath() { std::lock_guard<std::mutex> lk(wavM_); return wav_.path(); }
    uint64_t recordingBytes() { std::lock_guard<std::mutex> lk(wavM_); return wav_.bytes(); }
    double recordingSeconds() { std::lock_guard<std::mutex> lk(wavM_); return wav_.seconds(); }

    void stop();

private:
    void run();
    void emitRecordingState(bool active);

    Slot slot_;
    uint32_t sid_;
    EventSink sink_;
    IAudioSink* audio_;
    std::function<void(int, bool, bool, bool, bool)> formatCb_;

    RingBuffer<complex16> ring_{1 << 16};   // 65 536 Paare (~1,4 s bei 48 k)
    std::mutex m_;
    std::condition_variable cv_;       // Erzeuger -> Thread: Daten da
    std::condition_variable spaceCv_;  // Thread -> Erzeuger: Platz frei
    std::atomic<bool> running_{true};
    bool sinkStarted_ = false;
    std::atomic<int> rate_{48000};
    bool started_ = false;
    int lastRate_ = 0; bool lastPs_ = false, lastSbr_ = false, lastStereo_ = false;

    std::atomic<int> volume_{70};
    std::atomic<bool> muted_{false};

    converter_48000 converter_;
    std::mutex wavM_;
    WavWriter wav_;
    std::atomic<bool> recording_{false};
    std::chrono::steady_clock::time_point lastRecState_{};
    std::chrono::steady_clock::time_point lastLevel_{};

    std::thread thread_;
};

} // namespace dabcore
