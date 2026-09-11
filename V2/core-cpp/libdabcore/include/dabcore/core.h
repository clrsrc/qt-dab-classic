// DAB Classic – Kern-Fassade.
//
// DabCore nimmt Kommandos (JSON, siehe dab-api Command) entgegen und liefert
// Ereignisse ueber eine EventSink. Intern besitzt sie Quelle, OFDM-Thread
// (portierter Qt-DAB-Empfangspfad bis FIC), den mscHandler mit einem
// Backend-Thread je laufendem Dienst (Primary: Audio-Ausgabe + Aufnahme,
// Background: nur Backend + Aufnahme, beliebig viele – Entscheidung 24),
// je Audiodienst eine AudioPipeline (eigener Thread) und die Audio-Ausgabe
// (PortAudio oder Null-Sink). Stand M0: Datei-Quellen, Sync/OFDM/FIC, EWS,
// MSC/DAB+ Audio, PAD (DLS, DL+, MOT-Slides), MOT ueber Paketdienste,
// WAV-Aufnahme, Frame-Dump.
#pragma once

#include "dabcore/events.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class ISampleSource;
class FileSourceBase;
class ofdmHandler;
class mscHandler;
class Backend;
struct ReceiverCallbacks;
struct BackendCallbacks;
class processParams;
class IAudioSink;
enum class AacDecoderKind;

namespace dabcore {

class AudioPipeline;

struct CoreOptions {
    bool audio = true;          // PortAudio-Ausgabe (Entscheidung 3)
    std::string audioDevice;    // leer = Standardgeraet
    bool fastReplay = false;    // Dateien ohne Echtzeit-Pacing abspielen
    double replayDurationS = 0; // > 0: Datei-Wiedergabe nach S Sekunden Dateizeit beenden
    std::string aacDecoder = "auto";   // auto | faad2 | fdk
    // Headless: Dienste automatisch waehlen, sobald sie in der FIC auftauchen
    // (Name-Teilstring oder 0xSID). Erster Eintrag = Primary, weitere = Background.
    std::vector<std::string> autoServices;
    bool autoAllAudio = false;  // alle Audiodienste als Background (Spike 3)
    std::string autoWav;        // WAV-Dump des Primary-Dienstes ab Start
};

struct RunningService;

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
    void maybeAutoSelect(const ServiceInfo& s, bool seenBefore);
    std::string currentChannel() const;

    // Dienste (serviceM_ haelt der Aufrufer nicht; die Methoden sperren selbst)
    void selectService(uint32_t sid, uint8_t scids, Slot slot);
    void stopService(Slot slot, int64_t sid);      // sid < 0: alle im Slot
    void stopAllServicesLocked();
    void stopOneLocked(RunningService* rs);
    RunningService* findLocked(Slot slot, int64_t sid);
    void wireBackend(RunningService* rs);
    void updateServiceState();
    bool startRecording(Slot slot, int64_t sid, const std::string& path, const json& format);
    void stopRecording(Slot slot, int64_t sid);
    void startFrameDump(const std::string& path);
    void stopFrameDump();
    void emitAudioDevices();

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
    std::unique_ptr<mscHandler> msc_;
    std::unique_ptr<ISampleSource> source_;
    FileSourceBase* fileSource_ = nullptr;   // nicht besitzend
    std::unique_ptr<ofdmHandler> ofdm_;
    uint8_t cpuSupport_ = 0;

    // Dienste / Audio
    std::mutex serviceM_;
    bool closing_ = false;
    std::vector<std::unique_ptr<RunningService>> services_;
    std::unique_ptr<IAudioSink> audioSink_;
    AacDecoderKind aacKind_;
    std::mutex frameDumpM_;
    FILE* frameDump_ = nullptr;
    std::vector<std::string> autoPending_;   // noch nicht gefundene --service
    std::map<std::string, ServiceInfo> autoCandidates_;   // Teilstring-Treffer je --service
    bool autoWavStarted_ = false;

    // Drosselung latest-wins-Ereignisse
    std::chrono::steady_clock::time_point lastSnr_{}, lastFicQuality_{}, lastFreqOffset_{};
};

} // namespace dabcore
