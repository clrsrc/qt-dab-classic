// DAB Classic – Kern-Fassade.
//
// DabCore nimmt Kommandos (JSON, siehe dab-api Command) entgegen und liefert
// Ereignisse ueber eine EventSink. Intern besitzt sie Quelle, OFDM-Thread
// (portierter Qt-DAB-Empfangspfad bis FIC), spaeter Backends, Audio und
// Timeshift. Stand Spike 2: Datei-Quellen (.uff, .iq), Sync/OFDM/FIC,
// Ensemble-/Dienstliste, EWS; MSC ist eine No-op-Senke.
#pragma once

#include "dabcore/events.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class ISampleSource;
class FileSourceBase;
class ofdmHandler;
class IMscSink;
struct ReceiverCallbacks;
class processParams;

namespace dabcore {

struct CoreOptions {
    bool audio = true;          // PortAudio-Ausgabe (Entscheidung 3)
    std::string audioDevice;    // leer = Standardgeraet
    bool fastReplay = false;    // Dateien ohne Echtzeit-Pacing abspielen
    double replayDurationS = 0; // > 0: Datei-Wiedergabe nach S Sekunden Dateizeit beenden
};

class DabCore {
public:
    DabCore(EventSink sink, CoreOptions options);
    ~DabCore();
    DabCore(const DabCore&) = delete;
    DabCore& operator=(const DabCore&) = delete;

    // Verarbeitet ein Kommando; wird vom Kommandothread aufgerufen.
    // Gibt false zurueck, wenn der Kern beendet werden soll (shutdown).
    bool handle(const json& command);

    // Vollstaendigen Zustand als state_snapshot-Ereignis senden.
    void emitState();

    // Wird (aus einem Kernthread) aufgerufen, wenn eine Datei-Quelle ohne
    // loop zu Ende ist; der Aufrufer darf daraus keine Kernfunktionen rufen.
    void setFileEndedHandler(std::function<void()> h) { fileEndedHandler_ = std::move(h); }

    static std::string version();

private:
    void openDevice(const json& source);
    void closeDevice();
    void startSpike();   // Spike 1: synthetische Ereignisse (Quelle "spike")
    void stopSpike();
    void openFile(const std::string& path, bool loop, bool fast);
    void wireCallbacks();
    void emitService(const std::string& name, uint32_t sid, int subChId, bool primary);
    std::string currentChannel() const;

    EventSink sink_;
    CoreOptions opt_;
    mutable std::mutex stateM_;
    json state_;
    std::atomic<bool> spikeRunning_{false};
    std::atomic<bool> spectrumOn_{false};
    std::thread spikeThread_;
    std::function<void()> fileEndedHandler_;

    // Empfangspfad
    std::unique_ptr<processParams> params_;
    std::unique_ptr<ReceiverCallbacks> callbacks_;
    std::unique_ptr<IMscSink> mscSink_;
    std::unique_ptr<ISampleSource> source_;
    FileSourceBase* fileSource_ = nullptr;   // nicht besitzend
    std::unique_ptr<ofdmHandler> ofdm_;
    uint8_t cpuSupport_ = 0;
    // Drosselung latest-wins-Ereignisse
    std::chrono::steady_clock::time_point lastSnr_{}, lastFicQuality_{}, lastFreqOffset_{};
};

} // namespace dabcore
