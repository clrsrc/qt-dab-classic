// DAB Classic – Kern-Fassade.
//
// DabCore nimmt Kommandos (JSON, siehe dab-api Command) entgegen und liefert
// Ereignisse ueber eine EventSink. Intern besitzt sie Quelle, OFDM-Thread
// (portierter Qt-DAB-Empfangspfad bis FIC), den mscHandler mit einem
// Backend-Thread je laufendem Dienst (Primary: Audio-Ausgabe + Aufnahme,
// Background: nur Backend + Aufnahme, beliebig viele – Entscheidung 24),
// je Audiodienst eine AudioPipeline (eigener Thread) und die Audio-Ausgabe
// (PortAudio oder Null-Sink). Stand M1: Datei-, HackRF- und RTL-SDR-Quelle,
// Kanalwechsel, Gain-AGC (Akquisitions-Ramp + SNR-Bergsteiger, AgcController),
// Band-III-Scan mit derselben Ramp, IQ-Dump (.uff),
// Sync/OFDM/FIC, EWS, MSC/DAB+ Audio, PAD (DLS, DL+, MOT-Slides), MOT ueber
// Paketdienste, WAV-Aufnahme, Frame-Dump.
#pragma once

#include "dabcore/events.h"
#include "dabcore/gain.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
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
struct RecFormat;   // audio/rec-format.h, nur als Referenz gebraucht

namespace dabcore {

class AudioPipeline;
class ScanController;
class AgcController;
class TimeshiftController;
struct TimeshiftSnapshot;

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
    // autoEpg: vom Kern gestarteter SPI/EPG-Hintergrunddienst (set_epg false
    // beendet nur diese)
    void selectService(uint32_t sid, uint8_t scids, Slot slot, bool autoEpg = false);
    // Backend + Pipeline fuer die FIC-Komponente ficIndex anlegen und in
    // services_ eintragen (serviceM_ gehalten). false: nicht gestartet
    // (Log kam schon).
    bool startServiceLocked(int ficIndex, uint32_t sid, uint8_t scids, Slot slot, bool autoEpg);
    void stopService(Slot slot, int64_t sid);      // sid < 0: alle im Slot
    void stopAllServicesLocked();
    void stopOneLocked(RunningService* rs);
    // Review M1: nach einer Ensemble-Rekonfiguration (FIG 0/0) laufende
    // Dienste gegen die neue FIC pruefen: verschobene Subkanaele neu starten,
    // verschwundene Dienste stoppen. Laeuft im Aktionsthread.
    void reconcileServices();
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
    // pre_s > 0: Vorlauf aus dem Timeshift-Ring als <name>_vorlauf.<ext>
    // (Entscheidung 18, Plan M4 1.6)
    bool startRecording(Slot slot, int64_t sid, const std::string& path, const json& format, double preS);
    void stopRecording(Slot slot, int64_t sid);
    void startFrameDump(const std::string& path);
    void stopFrameDump();
    void emitAudioDevices();

    // --- Timeshift (M4) ---
    // Ring an den Primary-Audiodienst haengen bzw. loesen (serviceM_ gehalten)
    void attachTimeshiftLocked(RunningService* rs);
    void detachTimeshiftLocked(RunningService* rs);
    void onTimeshiftState(const TimeshiftSnapshot& s);
    // Zustand als [mode, buffered_s, offset_s, capacity_s] fuer state_snapshot
    json timeshiftJson() const;
    // Audio-Puffer des Primary-Slots verwerfen (timeshift_live)
    void flushPrimaryAudio();
    void setPrimaryStarved(bool starved);
    // Bitrate/SId des Primary-Audiodienstes fuer Export und Vorlauf
    bool primaryAudioParams(uint32_t& sid, int16_t& bitRate);
    void exportTimeshiftRange(double fromS, double toS, const std::string& path, const json& format);
    // Ergebnis eines Exports melden (recording_state, Plan 1.3)
    void startExportThread(double fromS, double toS, const std::string& path,
                           const RecFormat& format, bool reportRecordingState);
    void joinExportThread();
    // set_scopes auf den (neuen) ofdmHandler anwenden
    void applyScopes();
    // TII-Liste hoechstens 1x/s und nur bei Aenderung melden
    void onTii(const std::vector<std::tuple<uint8_t, uint8_t, float>>& tx);

    // --- EWS Auto-Umschaltung (Entscheidung 5) ---
    // Bei Trigger/Sustain auf den Dienst des gemeldeten Unterkanals wechseln
    // (v1 radio.cpp ewsStart), bei End zurueck auf den vorherigen Primary-
    // Dienst. Keine Umschaltung bei Testalarm, ausgeschaltetem Autoswitch
    // oder wenn schon umgeschaltet ist; eine gesperrte Umschaltung wegen
    // laufender Aufnahme (selectService) bleibt beim Warndienst-Ton stumm.
    // relevant = Geofencing-Ergebnis (siehe ewsRelevance); false verhindert
    // die Umschaltung, der Alarm bleibt nur informativ.
    // Laeuft im Aktionsthread (Review K1), nicht mehr im FIC-Callback.
    // phase 3 (End) schaltet immer zurueck, auch wenn set_ews inzwischen
    // enabled/autoswitch abgeschaltet hat (sonst bliebe der Warndienst stehen).
    void handleEwsAutoswitch(int phase, uint32_t subChId, bool isTest, bool relevant);
    // set_ews.enabled (Review G11): false unterdrueckt alle EWS-Ereignisse
    // (ews_alert/ews_alive/ews_present/ewf_alarm) und die Umschaltung.
    std::atomic<bool> ewsEnabled_{true};
    bool     ewsAutoActive_ = false;
    uint32_t ewsAlertSid_ = 0;
    uint32_t ewsSavedSid_ = 0;
    uint8_t  ewsSavedScids_ = 0;
    bool     ewsHasSaved_ = false;
    // Geofencing (ASA DE / TS 104 089 Klausel 7.5): Ortscodes des Alarms gegen
    // die Heimatposition. std::nullopt = keine Heimatposition gesetzt, dann
    // gilt jeder Alarm als relevant (Ausgangsverhalten). Ohne den Abgleich
    // wuerde z. B. der 5-Minuten-Funktionstest des Bundesmux ("Eiffelturm",
    // Ortscodes um Paris) auch in Deutschland die Umschaltung ausloesen.
    std::optional<bool> ewsRelevance(const std::vector<std::string>& locations) const;
    std::optional<double> homeLat_;   // set_home_location
    std::optional<double> homeLon_;

    EventSink sink_;
    CoreOptions opt_;
    mutable std::mutex stateM_;
    json state_;
    std::atomic<bool> spikeRunning_{false};
    // set_scopes: Spektrum und Konstellation getrennt, gemeinsame Rate 1..10 Hz
    std::atomic<bool> spectrumOn_{false};
    std::atomic<bool> iqOn_{false};
    std::atomic<int> scopeRateHz_{5};
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
    // set_agc (Nutzerwunsch); atomar, weil emitGain auch aus dem AGC-Apply-
    // Callback im OFDM-Thread liest (Review G7)
    std::atomic<bool> agc_{true};
    int ppm_ = 0;
    std::optional<DeviceGain> pendingGain_;   // set_gain vor open_device
    // AgcController (device/agc-controller.h) je geoeffnetem Geraet; agcM_
    // schuetzt ihn zwischen OFDM-Thread (no_signal/snr/synced), Kommando-
    // Thread (set_gain/set_agc) und Scan-Thread (tuneChannel). Nie halten,
    // waehrend der OFDM-Thread gejoint wird.
    std::mutex agcM_;
    std::unique_ptr<AgcController> agcCtl_;
    static int64_t agcNowMs();
    // Scan: die Ramp laeuft auch bei AGC aus; danach wird der Gain-Satz
    // von vor dem Scan wiederhergestellt.
    bool scanForcedAgc_ = false;
    DeviceGain preScanGain_;
    std::atomic<float> lastSnrDb_{0.0f};
    std::unique_ptr<ScanController> scan_;
    std::atomic<bool> scanning_{false};

    // Dienste / Audio
    //
    // Sperrordnung (Review 16.09.2026 K1, Deadlock fibLocker <-> serviceM_):
    //   serviceM_  ->  fibLocker (ofdm_->fic())  ->  mscHandler::locker
    //   serviceM_  ->  stateM_  ->  (keine weitere)
    //   serviceM_  ->  primaryAudioM_
    // Der OFDM-Thread haelt in den FIC-Callbacks (addToEnsemble, ewsAlert,
    // changeInConfiguration, ...) den fibLocker des fibDecoders und darf
    // deshalb serviceM_ NIE nehmen. Alles, was aus einem FIC-Callback einen
    // Dienst starten oder stoppen will (Headless-Autoauswahl, SPI/EPG-
    // Hintergrunddienst, EWS-Umschaltung, Rekonfiguration), wird als Action
    // eingereiht und vom Aktionsthread (runActions) ausserhalb des fibLocker
    // ausgefuehrt. Kommandothread, Scan-Thread und Aktionsthread nehmen
    // serviceM_ und duerfen darunter die FIC abfragen.
    std::mutex serviceM_;
    bool closing_ = false;
    // Review M2: waehrend tuneChannel (zwischen stopAllServicesLocked und
    // ofdm_->start(), das per resetChannel alle Backends loescht) darf kein
    // Dienst angelegt werden - sonst zeigt RunningService::backend ins Leere.
    bool retuning_ = false;
    std::vector<std::unique_ptr<RunningService>> services_;

    // --- Aktionsthread (Review K1) ---
    struct Action {
        enum Kind { Select, Ews, Reconfigure };
        Kind kind = Select;
        uint64_t generation = 0;      // Kanalwechsel/close_device verwerfen aeltere
        uint32_t sid = 0;             // Select
        uint8_t scids = 0;
        Slot slot = Slot::Primary;
        bool autoEpg = false;
        int phase = 0;                // Ews
        uint32_t subChId = 0;
        bool isTest = false;
        bool relevant = true;
    };
    void enqueueAction(Action a);
    void invalidateActions();         // Warteschlange leeren, Generation erhoehen
    void runActions();                // Schleife des Aktionsthreads
    void stopActionThread();
    std::mutex actionM_;
    std::condition_variable actionCv_;
    std::deque<Action> actions_;
    uint64_t actionGeneration_ = 0;
    bool actionStop_ = false;
    std::thread actionThread_;
    std::unique_ptr<IAudioSink> audioSink_;
    AacDecoderKind aacKind_;
    std::mutex frameDumpM_;
    FILE* frameDump_ = nullptr;
    // Timeshift-Ring des Primary-Slots (Entscheidung 4); der Controller lebt
    // so lange wie der Kern, der Ring nur zwischen attach und detach.
    std::unique_ptr<TimeshiftController> timeshift_;
    // Pipeline des Primary-Slots fuer Flush/Underrun-Unterdrueckung; eigene
    // Sperre, weil der Timeshift-Controller sie auch aus attach/detach ruft
    // (serviceM_ ist dann schon gehalten).
    mutable std::mutex primaryAudioM_;
    AudioPipeline* primaryAudio_ = nullptr;
    std::thread exportThread_;
    std::atomic<bool> exportBusy_{false};
    // Letzte Ensemble-Uhrzeit als Zeitstempel der Ringrahmen
    std::atomic<int64_t> clockUnix_{0};
    std::atomic<int64_t> clockAtMs_{0};
    int64_t frameUnixNow() const;
    std::vector<std::string> autoPending_;   // noch nicht gefundene --service
    std::map<std::string, ServiceInfo> autoCandidates_;   // Teilstring-Treffer je --service
    std::chrono::steady_clock::time_point autoCandidateSince_{};
    std::atomic<bool> autoWavStarted_{false};   // Aktions- und Kommandothread
    // EPG/SPI-Hintergrunddienst
    std::atomic<bool> epgEnabled_{true};
    std::atomic<int> lto_{0};                // FIG 0/9 LTO (Stunden), fuer den epg-compiler

    // Drosselung latest-wins-Ereignisse
    std::chrono::steady_clock::time_point lastSnr_{}, lastFicQuality_{}, lastFreqOffset_{};
    // TII: zuletzt gemeldete Liste (mainId, subId, Staerke auf 0,01 gerundet)
    std::mutex tiiM_;
    std::vector<std::tuple<uint8_t, uint8_t, int>> lastTii_;
    std::chrono::steady_clock::time_point lastTiiTime_{};
};

} // namespace dabcore
