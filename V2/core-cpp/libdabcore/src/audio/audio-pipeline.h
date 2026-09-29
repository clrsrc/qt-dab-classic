// DAB Classic v3 – Audio-Thread je Dienst-Slot (Entscheidung 3, Analyse 7.2).
// Ersatz fuer RadioInterface::newAudio aus Qt-DAB (dort lief Audio durch
// den GUI-Thread): PCM-Ring vom Decoder -> converter_48000 -> WAV-Dump
// (vor der Lautstaerke, also unabhaengig vom Regler) -> Lautstaerke/Mute ->
// Peak-Pegel (10 Hz) -> IAudioSink. Hintergrund-Slots haben keinen Sink
// (nur Aufnahme).
//
// Vordecodierung (28.09.2026): der Sink kann im Betrieb von einer Pipeline
// auf eine andere wandern (detachSink/attachSink). Alle Audiodienste des
// Ensembles laufen dann als Background mit, und ein Dienstwechsel im
// Ensemble haengt nur die Ausgabe um - ohne Backend-Neustart, ohne die
// ~0,5 s bis zum ersten dekodierten Superframe.
#pragma once

#include "dabcore/events.h"
#include "dab-constants.h"
#include "ringbuffer.h"
#include "converter48k.h"
#include "wav-writer.h"
#include "rec-format.h"
#include "audio-sink.h"
#include "fft-handler.h"

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

    // Grafik-Equalizer-Anzeige (set_audio_spectrum): Zeiger auf den Schalter
    // des Kerns; nur die Pipeline mit Ausgabe rechnet (zwei Hann-FFTs 1024
    // je 100-ms-Block, ~20 Hz audio_spectrum), vor Lautstaerke und Mute.
    void setSpectrumFlag(const std::atomic<bool>* on) { spectrumOn_ = on; }

    // Slot-Wechsel (Befoerderung Background -> Primary und zurueck): nur die
    // Kennung in den Aufnahme-Ereignissen; die Ausgabe wandert getrennt
    // ueber detachSink/attachSink.
    void setSlot(Slot slot) { slot_.store(slot); }
    Slot slot() const { return slot_.load(); }

    // Ausgabe abgeben: der eigene Thread schreibt danach nicht mehr in den
    // Sink; der Sink selbst laeuft weiter (die uebernehmende Pipeline fuehrt
    // ihn fort). Rueckgabe: ob der Sink gestartet war - das gibt die
    // uebernehmende Seite bei attachSink als `running` an. Blockiert, bis ein
    // gerade laufender Schreibvorgang fertig ist (Reihenfolge: erst abgeben,
    // dann uebernehmen, dann schreiben nie zwei Threads gleichzeitig).
    bool detachSink();
    // Ausgabe uebernehmen. running = true: der Sink spielt schon (vom
    // Vorgaenger); beim naechsten Block wird sein Ausgabepuffer verworfen
    // (kein Nachklang des alten Dienstes) und wie beim Start ein kurzer
    // Stille-Vorlauf geschrieben.
    void attachSink(IAudioSink* sink, bool running);
    bool hasSink();

    // Timeshift (Plan M4 1.3): beim Sprung auf live den PCM-Ring dieses
    // Threads und den Ausgabepuffer des Sinks verwerfen. Der eigene Thread
    // erledigt das beim naechsten Durchlauf (<= 50 ms), damit es keinen
    // Wettlauf mit dem Schreiben gibt.
    void requestFlush() { flushPending_.store(true); }
    // true, solange der Ring bewusst nichts liefert (paused/playing): dann
    // wird audio_underrun nicht gezaehlt.
    void setStarved(bool starved);

    // Aufnahme im gewuenschten Format (Plan M4b 1.4); startWav ist der
    // Sonderfall fuer den Headless-Schalter --wav.
    bool startRec(const std::string& path, const RecFormat& format, std::string& error);
    bool startWav(const std::string& path, std::string& error) { return startRec(path, RecFormat{}, error); }
    void stopWav();
    bool recording() const { return recording_.load(); }
    // Stand der laufenden Aufnahme (fuer state_snapshot)
    std::string recordingPath() { std::lock_guard<std::mutex> lk(wavM_); return rec_ ? rec_->path() : std::string(); }
    uint64_t recordingBytes() { std::lock_guard<std::mutex> lk(wavM_); return rec_ ? rec_->bytes() : 0; }
    double recordingSeconds() { std::lock_guard<std::mutex> lk(wavM_); return rec_ ? rec_->seconds() : 0.0; }

    void stop();

private:
    void run();
    void emitRecordingState(bool active);
    void writePrelude();   // sinkM_ gehalten, audio_ != nullptr
    void emitSpectrum(const float* interleaved, int frames);   // Pipeline-Thread

    const std::atomic<bool>* spectrumOn_ = nullptr;
    std::unique_ptr<fftHandler> fft_;
    std::vector<Complex> fftBuf_;
    std::vector<float> hann_;

    std::atomic<Slot> slot_;
    uint32_t sid_;
    EventSink sink_;
    // Ausgabe: audio_, sinkStarted_ und handover_ nur unter sinkM_ (der
    // eigene Thread haelt sie waehrend jedes Schreibblocks, detach/attach
    // aus dem Aktionsthread warten darauf).
    std::mutex sinkM_;
    IAudioSink* audio_;
    bool sinkStarted_ = false;
    bool handover_ = false;
    std::function<void(int, bool, bool, bool, bool)> formatCb_;

    RingBuffer<complex16> ring_{1 << 16};   // 65 536 Paare (~1,4 s bei 48 k)
    std::mutex m_;
    std::condition_variable cv_;       // Erzeuger -> Thread: Daten da
    std::condition_variable spaceCv_;  // Thread -> Erzeuger: Platz frei
    std::atomic<bool> running_{true};
    std::atomic<int> rate_{48000};
    bool started_ = false;
    int lastRate_ = 0; bool lastPs_ = false, lastSbr_ = false, lastStereo_ = false;

    std::atomic<int> volume_{70};
    std::atomic<bool> muted_{false};
    std::atomic<bool> flushPending_{false};
    std::atomic<bool> starved_{false};
    // nach einem Flush braucht der Decoder bis zu einem Superframe (120 ms),
    // bis wieder PCM kommt; solange ist eine Luecke kein Underrun
    std::chrono::steady_clock::time_point quietUntil_{};

    converter_48000 converter_;
    std::mutex wavM_;
    std::unique_ptr<IPcmWriter> rec_;
    std::atomic<bool> recording_{false};
    std::chrono::steady_clock::time_point lastRecState_{};
    std::chrono::steady_clock::time_point lastLevel_{};

    std::thread thread_;
};

} // namespace dabcore
