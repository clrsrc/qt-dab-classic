// DAB Classic v3 – Steuerung des Timeshift-Rings (Plan M4 1.1-1.3).
//
// Der Controller haengt zwischen Backend und backendDriver des **Primary**-
// Dienstes: Backend::processSegment gibt jeden Hardbit-Rahmen an
// IFrameTap::onBackendFrame, hier landet er im Ring und – im Zustand `live` –
// unveraendert beim Driver. In `paused` faellt der Driver aus (Stille), in
// `playing` bedient ihn ein eigener Takt-Thread (24 ms je Rahmen, Drift-
// Korrektur gegen steady_clock) aus dem Ring; erreicht der Lesezeiger den
// Schreibzeiger, geht es im selben Takt nahtlos zurueck auf `live`.
//
// Der Controller kennt weder Protokoll noch Kern: Meldungen und Log gehen
// ueber Callbacks, die DabCore setzt.
#pragma once

#include "backend-frame-tap.h"
#include "timeshift-buffer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dabcore {

// Zustand fuer das Ereignis timeshift_state bzw. state_snapshot.
struct TimeshiftSnapshot {
    TimeshiftMode mode = TimeshiftMode::Live;
    double bufferedS = 0.0;
    double offsetS = 0.0;
    double capacityS = 0.0;
    uint64_t frameIndex = 0;   // Schreibzeiger (Rahmen seit dem Leeren)
    int64_t liveUnix = 0;      // Ensemble-Uhrzeit am Schreibzeiger (0 = unbekannt)
    bool attached = false;     // ein Primary-Audiodienst haengt am Ring
};

class TimeshiftController final : public IFrameTap {
public:
    using FrameSink = std::function<void(const std::vector<uint8_t>&)>;
    using Notify = std::function<void(const TimeshiftSnapshot&)>;
    using Logger = std::function<void(const char* level, const std::string& text)>;
    using Action = std::function<void()>;
    using Clock = std::function<int64_t()>;   // Ensemble-Uhrzeit (unix_utc), 0 = unbekannt

    TimeshiftController();
    ~TimeshiftController() override;
    TimeshiftController(const TimeshiftController&) = delete;
    TimeshiftController& operator=(const TimeshiftController&) = delete;

    void setNotify(Notify n);
    void setLogger(Logger l);
    // timeshift_live: Audio-Puffer des Sinks verwerfen (sonst hoert man
    // beim Sprung noch ~0,7 s Altes).
    void setFlush(Action f);
    // Unterdrueckt die audio_underrun-Zaehlung, solange der Ring bewusst
    // nichts liefert (Plan 1.3): true = paused/playing.
    void setStarvedHandler(std::function<void(bool)> h);
    void setClock(Clock c);

    // capacity_s wird auf 60..14400 geklemmt; bei Aenderung neuer (leerer) Ring.
    void configure(uint32_t capacityS);
    uint32_t capacitySeconds() const { return buf_.capacitySeconds(); }

    // Primary-Audiodienst anhaengen: Rahmengroesse in Hardbits (24 * bitRate)
    // und die Senke zum Driver. Leert den Ring und startet den Takt-Thread.
    void attach(uint32_t frameBits, FrameSink sink);
    // Dienst weg: Takt-Thread anhalten, Senke loesen, Ring leeren. Nach der
    // Rueckkehr wird die Senke nicht mehr gerufen.
    void detach();
    bool attached() const { return attached_.load(); }

    // Backend-Thread (IFrameTap)
    void onBackendFrame(const std::vector<uint8_t>& hardBits) override;

    // Kommandos (Kommandothread)
    void pause();
    void play();
    void live();
    double seek(double offsetS);
    double skip(double deltaS);
    // Dienst-/Kanalwechsel, close_device, EWS: auf live und leeren.
    void dropToLive(const char* reason);

    TimeshiftSnapshot snapshot() const;
    TimeshiftBuffer& buffer() { return buf_; }
    const TimeshiftBuffer& buffer() const { return buf_; }

private:
    void startThread();
    void stopThread();
    void run();
    void publish();
    void setMode(TimeshiftMode m);
    void log(const char* level, const std::string& text) const;
    void updateStarved();

    TimeshiftBuffer buf_;
    std::atomic<bool> attached_{false};

    mutable std::mutex cbM_;
    Notify notify_;
    Logger logger_;
    Action flush_;
    std::function<void(bool)> starved_;
    Clock clock_;

    std::mutex sinkM_;
    FrameSink sink_;

    std::atomic<bool> threadRun_{false};
    std::thread thread_;
};

} // namespace dabcore
