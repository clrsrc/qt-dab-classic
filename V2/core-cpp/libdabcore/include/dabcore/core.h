// DAB Classic – Kern-Fassade.
//
// DabCore nimmt Kommandos (JSON, siehe dab-api Command) entgegen und liefert
// Ereignisse ueber eine EventSink. Intern besitzt sie Quelle, OFDM-Thread
// (portierter Qt-DAB-Empfangspfad bis FIC), den mscHandler mit einem
// Backend-Thread je laufendem Dienst (Primary: Audio-Ausgabe + Aufnahme,
// Background: nur Backend + Aufnahme, beliebig viele – Entscheidung 24),
// je Audiodienst eine AudioPipeline (eigener Thread) und die Audio-Ausgabe
// (PortAudio oder Null-Sink). Stand M1: Datei-, HackRF- und RTL-SDR-Quelle,
// Kanalwechsel, Gain/SNR-AGC, Band-III-Scan mit AMP-Retry, IQ-Dump (.uff),
// Sync/OFDM/FIC, EWS, MSC/DAB+ Audio, PAD (DLS, DL+, MOT-Slides), MOT ueber
// Paketdienste, WAV-Aufnahme, Frame-Dump.
#pragma once

#include "dabcore/events.h"
#include "dabcore/gain.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
class ScanController;

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
    // Verweilzeit je Kanal im Scan (v1 switchDelay, Default 6 s)
    int scanDwellMs = 6000;
    // SPI/EPG-Paketdienst des Ensembles automatisch als Background-Slot
    // starten (Logos, EPG); Kommando set_epg{enabled} schaltet zur Laufzeit.
    bool epg = true;
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
    // Wird (aus einem Kernthread) aufgerufen, wenn ein Geraet keine Daten
    // mehr liefert (device_error wurde gesendet). Der Aufrufer soll danach
    // aus einem anderen Thread close_device ausfuehren.
    void setDeviceLostHandler(std::function<void()> h) { deviceLostHandler_ = std::move(h); }

    static std::string version();

private:
    void openDevice(const json& source);
    void closeDevice();
    void startSpike();   // Spike 1: synthetische Ereignisse (Quelle "spike")
    void stopSpike();
    void openFile(const std::string& path, bool loop, bool fast);
    void openHackRf(const std::string& serial);
    void openRtlSdr(int index);
    void attachDevice(std::unique_ptr<ISampleSource> src, const std::string& info);
    // Kanal einstellen (nur Geraete): Dienste stoppen, Quelle neu starten,
    // FIC zuruecksetzen, OFDM (im Scan-Modus nur FIC) neu starten.
    bool tuneChannel(const std::string& channel, bool scan);
    void onDeviceLost(const std::string& message);
    void wireCallbacks();
    void emitService(const std::string& name, uint32_t sid, int subChId, bool primary);
    void maybeAutoSelect(const ServiceInfo& s, bool seenBefore);
    std::string currentChannel() const;

    // Gain / AGC / Korrektur
    void setGain(const json& gain);
    void setAgc(bool enabled);
    void setPpm(int ppm);
    void emitGain();

    // Scan
    void startScan(const std::vector<std::string>& channels, const std::string& mode);
    void stopScan();
    void scanFinished();

    // IQ-Dump
    void startIqDump(const std::string& path);
    void stopIqDump();

    // Dienste (serviceM_ haelt der Aufrufer nicht; die Methoden sperren selbst)
    void selectService(uint32_t sid, uint8_t scids, Slot slot);
    void stopService(Slot slot, int64_t sid);      // sid < 0: alle im Slot
    void stopAllServicesLocked();
    void stopOneLocked(RunningService* rs);
    RunningService* findLocked(Slot slot, int64_t sid);
    void wireBackend(RunningService* rs);
    // MOT-Objekt eines Paketdienstes einordnen (v1 handle_motObject):
    // Bild -> mot_object (Logo), Application -> epg-compiler -> epg_object,
    // sonst mot_object. Laeuft im Backend-Thread des Dienstes.
    void onMotObject(RunningService* rs, const std::vector<uint8_t>& data, const std::string& name,
                     int contentType, uint32_t objSid);
    // SPI-Dienst (FIG 0/13 Appl-Type 7) automatisch als Background starten
    void maybeStartEpg(const ServiceInfo& s);
    void setEpg(bool enabled);
    uint16_t currentEid() const;
    // SId eines Ensemble-Dienstes aus dem Objektnamen (4/8 Hex-Zeichen vor '_'), sonst 0
    uint32_t sidFromLogoName(const std::string& name) const;
    // v1 radio.cpp extractName: Datum (8 Ziffern, Jahr 2000..2030) und
    // 4-stellige SId eines Ensemble-Dienstes aus dem MOT-Namen
    bool epgNameParts(const std::string& name, uint32_t& date, uint32_t& sid) const;
    bool ensembleHasSid(uint32_t sid) const;
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
    std::function<void()> deviceLostHandler_;
    std::atomic<bool> deviceLost_{false};

    // Empfangspfad
    std::unique_ptr<processParams> params_;
    std::unique_ptr<ReceiverCallbacks> callbacks_;
    std::unique_ptr<mscHandler> msc_;
    std::unique_ptr<ISampleSource> source_;
    FileSourceBase* fileSource_ = nullptr;   // nicht besitzend
    std::unique_ptr<ofdmHandler> ofdm_;
    uint8_t cpuSupport_ = 0;

    // Gain / AGC / Scan
    bool agc_ = true;
    int ppm_ = 0;
    std::optional<DeviceGain> pendingGain_;   // set_gain vor open_device
    std::chrono::steady_clock::time_point lastAgc_{};
    std::atomic<float> lastSnrDb_{0.0f};
    std::unique_ptr<ScanController> scan_;
    std::atomic<bool> scanning_{false};

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
    std::chrono::steady_clock::time_point autoCandidateSince_{};
    bool autoWavStarted_ = false;
    // EPG/SPI-Hintergrunddienst
    std::atomic<bool> epgEnabled_{true};
    std::atomic<int> lto_{0};                // FIG 0/9 LTO (Stunden), fuer den epg-compiler

    // Drosselung latest-wins-Ereignisse
    std::chrono::steady_clock::time_point lastSnr_{}, lastFicQuality_{}, lastFreqOffset_{};
};

} // namespace dabcore
