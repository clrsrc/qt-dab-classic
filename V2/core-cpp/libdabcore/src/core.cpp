#include "dabcore/core.h"

#include "device/xml-file-source.h"
#include "device/raw-file-source.h"
#include "device/hackrf-source.h"
#include "device/rtlsdr-source.h"
#include "device/agc-controller.h"
#include "scan/scan-controller.h"
#include "support/dab-channels.h"
#include "frontend/ofdm-handler.h"
#include "frontend/receiver-callbacks.h"
#include "backend/msc-handler.h"
#include "backend/backend.h"
#include "backend/backend-callbacks.h"
#include "backend/timeshift-controller.h"
#include "backend/timeshift-export.h"
#include "backend/audio/aac-decoder.h"
#include "pad/mot-object.h"
#include "backend/data/epg/epg-compiler.h"
#include "audio/audio-sink.h"
#include "audio/portaudio-sink.h"
#include "audio/audio-pipeline.h"
#include "support/process-params.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace dabcore {

using namespace std::chrono_literals;

// Ein laufender Dienst: Backend (eigener Thread im mscHandler), seine
// Callbacks (muessen das Backend ueberleben) und bei Audiodiensten die
// AudioPipeline. Primary hat den Audio-Sink, Background keinen.
struct RunningService {
    Slot slot = Slot::Primary;
    uint32_t sid = 0;
    uint8_t scids = 0;
    uint8_t subCh = 0;
    bool isAudio = true;
    std::string name;
    std::unique_ptr<BackendCallbacks> cb;
    std::unique_ptr<AudioPipeline> audio;
    std::unique_ptr<descriptorType> descriptor;   // audiodata / packetdata
    Backend* backend = nullptr;                    // gehoert dem mscHandler
    bool started = false;
    // Letzter Anzeigezustand fuer state_snapshot (geschrieben im Backend-
    // bzw. Audio-Thread, gelesen im Kommandothread -> padM).
    std::mutex padM;
    std::string lastDls;
    json lastDlPlus;                               // {item_toggle, item_running, tags} oder null
    json lastSlide;                                // {mime, name, data_b64} oder null
    json codec;                                    // wie service_started.codec oder null
    bool stereo = false;
    std::unique_ptr<epgCompiler> epg;              // Paketdienste: Binaer-EPG -> XML
    bool autoEpg = false;                          // vom Kern gestarteter SPI-Dienst
    // Das SPI-Karussell liefert jedes Objekt mit jeder neuen Verzeichnis-
    // Version erneut (Bundesmux: alle ~25 min); unveraenderte Objekte
    // (Name + FNV-1a-Hash des Inhalts) werden nicht noch einmal gemeldet.
    std::map<std::string, uint64_t> motSeen;
};

static uint64_t fnv1a(const std::vector<uint8_t>& d) {
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t b : d) { h ^= b; h *= 1099511628211ULL; }
    return h;
}

std::string DabCore::version() {
#ifdef DABCORE_VERSION
    return DABCORE_VERSION;
#else
    return "0.0.0";
#endif
}

static std::string trimRight(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

static bool endsWithNoCase(const std::string& s, const char* suffix) {
    std::string a = s, b = suffix;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    return a.size() >= b.size() && a.compare(a.size() - b.size(), b.size(), b) == 0;
}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// --service NAME|0xSID: 2 = exakter Name oder SId, 1 = Name-Teilstring, 0 = nein
static int serviceMatch(const ServiceInfo& s, const std::string& wanted) {
    if (wanted.size() > 2 && (wanted[0] == '0') && (wanted[1] == 'x' || wanted[1] == 'X')) {
        char* end = nullptr;
        unsigned long v = std::strtoul(wanted.c_str() + 2, &end, 16);
        return (end && *end == '\0' && v == s.sid) ? 2 : 0;
    }
    std::string a = lower(trimRight(s.name)), b = lower(trimRight(wanted));
    if (a == b) return 2;
    return a.find(b) != std::string::npos ? 1 : 0;
}

static Slot slotFromJson(const json& c, Slot dflt = Slot::Primary) {
    std::string s = c.value("slot", "");
    if (s == "background") return Slot::Background;
    if (s == "primary") return Slot::Primary;
    return dflt;
}

DabCore::DabCore(EventSink sink, CoreOptions options)
    : sink_(std::move(sink)), opt_(options), aacKind_(aacDecoderKindFromName(options.aacDecoder)) {
    state_ = {
        {"source", nullptr}, {"channel", nullptr},
        // Defaults HackRF (v1 hackrf-handler): LNA 40, VGA 24, AMP aus
        {"gain", {{"lna", 40}, {"vga", 24}, {"amp", false}}}, {"agc", true},
        {"synced", false}, {"ensemble", nullptr}, {"services", json::array()},
        {"primary", nullptr}, {"background", nullptr},
        {"volume_percent", 70}, {"muted", false}, {"timeshift", nullptr},
        {"recording", false}, {"ews_enabled", true}, {"ews_autoswitch", true},
        {"epg_enabled", options.epg},
        {"clock_time", nullptr}, {"ppm", 0},
    };
    epgEnabled_.store(options.epg);
#ifdef __ARCH_X86__
    __builtin_cpu_init();
    int has_avx2 = __builtin_cpu_supports("avx2") != 0 ? AVX_SUPPORT : 0;
    int has_sse4 = __builtin_cpu_supports("sse4.1") != 0 ? SSE_SUPPORT : 0;
    cpuSupport_ = static_cast<uint8_t>(has_avx2 + has_sse4);
#endif
    // Timeshift-Ring des Primary-Slots (Entscheidung 4, Plan M4):
    // Meldungen, Log, Audio-Flush und die Ensemble-Uhrzeit kommen von hier.
    timeshift_ = std::make_unique<TimeshiftController>();
    timeshift_->setNotify([this](const TimeshiftSnapshot& s) { onTimeshiftState(s); });
    timeshift_->setLogger([this](const char* level, const std::string& t) { sink_(events::log(level, t)); });
    timeshift_->setFlush([this] { flushPrimaryAudio(); });
    timeshift_->setStarvedHandler([this](bool starved) { setPrimaryStarved(starved); });
    timeshift_->setClock([this] { return frameUnixNow(); });

    params_ = std::make_unique<processParams>();
    callbacks_ = std::make_unique<ReceiverCallbacks>();
    msc_ = std::make_unique<mscHandler>(params_->dabMode, cpuSupport_);
    wireCallbacks();
    autoPending_ = opt_.autoServices;

    sink_(events::ready(version(), 1, availableAacDecoders()));
    sink_(events::log("info", std::string("Viterbi: ") +
                              (cpuSupport_ & AVX_SUPPORT ? "avx2" : cpuSupport_ & SSE_SUPPORT ? "sse4.1" : "scalar")));

#ifdef DABCORE_AUDIO_PORTAUDIO
    if (opt_.audio) {
        auto pa = std::make_unique<PortAudioSink>(1);
        if (pa->ok()) {
            audioSink_ = std::move(pa);
        } else {
            sink_(events::log("warn", "PortAudio nicht verfuegbar, Audio-Ausgabe aus"));
        }
    }
#endif
    if (!audioSink_) audioSink_ = std::make_unique<NullAudioSink>();
    if (opt_.audio && !opt_.audioDevice.empty()) {
        auto names = audioSink_->devices();
        for (size_t i = 0; i < names.size(); ++i)
            if (lower(names[i]).find(lower(opt_.audioDevice)) != std::string::npos) {
                audioSink_->selectDevice(static_cast<int>(i));
                break;
            }
    }
    emitAudioDevices();
}

DabCore::~DabCore() {
    stopSpike();
    closeDevice();
    stopFrameDump();
    joinExportThread();
}

// audio_devices kommt nach ready, auf get_state und nach set_audio_device;
// ohne Audio-Ausgabe (--no-audio) mit leerer Liste und current = null.
void DabCore::emitAudioDevices() {
    sink_(events::audioDevices(audioSink_->devices(), audioSink_->currentDevice()));
}

void DabCore::applyScopes() {
    if (ofdm_) ofdm_->setScopes(spectrumOn_.load(), iqOn_.load(), scopeRateHz_.load());
}

// v1 meldete jede TII-Auswertung (alle 3 TII-Nullsymbole, ~1,7/s). Hier
// hoechstens 1x/s und nur, wenn sich die Liste (IDs oder Staerke auf 0,01
// gerundet) geaendert hat; eine leer gewordene Liste wird einmal gemeldet.
void DabCore::onTii(const std::vector<std::tuple<uint8_t, uint8_t, float>>& tx) {
    std::vector<std::tuple<uint8_t, uint8_t, int>> key;
    for (auto& [m, s, st] : tx) key.emplace_back(m, s, static_cast<int>(std::lround(st * 100.0f)));
    {
        std::lock_guard<std::mutex> lk(tiiM_);
        auto now = std::chrono::steady_clock::now();
        if (key == lastTii_) return;
        if (now - lastTiiTime_ < 1000ms) return;
        lastTii_ = key;
        lastTiiTime_ = now;
    }
    sink_(events::tii(tx));
}

std::string DabCore::currentChannel() const {
    std::lock_guard<std::mutex> lk(stateM_);
    return state_["channel"].is_string() ? state_["channel"].get<std::string>() : "";
}

// Entscheidung 5 / v1 radio.cpp ewsStart: bei Trigger/Sustain auf den
// Warndienst (den Audiodienst auf dem gemeldeten Unterkanal) wechseln,
// bei End zurueck auf den Dienst, der vorher lief. Laeuft im OFDM-Thread
// (FIC-Callback), wie die anderen ews*-Callbacks auch.
void DabCore::handleEwsAutoswitch(int phase, uint32_t subChId, bool isTest) {
    bool autoswitch;
    {
        std::lock_guard<std::mutex> lk(stateM_);
        autoswitch = state_.value("ews_autoswitch", true);
    }
    if (!autoswitch || isTest) return;
    if (phase == 1 || phase == 2) {   // Trigger, Sustain
        if (ewsAutoActive_) return;   // schon auf dem Warndienst
        uint32_t targetSid = 0;
        uint8_t targetScids = 0;
        uint32_t curSid = 0;
        uint8_t curScids = 0;
        bool haveCur = false;
        {
            std::lock_guard<std::mutex> lk(stateM_);
            for (auto& e : state_["services"]) {
                if (e.value("is_audio", false) && e.value("sub_ch", -1) == static_cast<int>(subChId)) {
                    targetSid = e.at("sid").get<uint32_t>();
                    targetScids = e.value("scids", static_cast<uint8_t>(0));
                    break;
                }
            }
            if (state_["primary"].is_array() && state_["primary"].size() == 2) {
                curSid = state_["primary"][0].get<uint32_t>();
                curScids = state_["primary"][1].get<uint8_t>();
                haveCur = true;
            }
        }
        // Nur umschalten, wenn schon ein Primary-Dienst lief (Entscheidung 5:
        // "...schaltet auf den Warndienst; nach Alarmende vorheriger Sender
        // live weiter" setzt einen gehoerten Dienst voraus). Ohne Hoerer
        // (z. B. reiner EPG-Empfang, Headless-Tests ohne --service) bleibt
        // der Alarm rein informativ (ews_alert/ews_present/ews_alive).
        if (targetSid == 0 || !haveCur || targetSid == curSid) return;
        ewsSavedSid_ = haveCur ? curSid : 0;
        ewsSavedScids_ = haveCur ? curScids : 0;
        ewsHasSaved_ = haveCur;
        ewsAlertSid_ = targetSid;
        // selectService lehnt bei laufender Aufnahme ab (loggt "warn") – der
        // Alarm bleibt dann beim gehoerten Dienst, ews_switched bleibt aus.
        selectService(targetSid, targetScids, Slot::Primary);
        uint32_t nowPrimary = 0;
        {
            std::lock_guard<std::mutex> lk(stateM_);
            if (state_["primary"].is_array() && state_["primary"].size() == 2)
                nowPrimary = state_["primary"][0].get<uint32_t>();
        }
        if (nowPrimary != targetSid) return;   // Umschalten wurde abgelehnt
        ewsAutoActive_ = true;
        sink_(events::ewsSwitched(targetSid, haveCur ? static_cast<int64_t>(curSid) : -1));
    } else if (phase == 3) {   // End
        if (!ewsAutoActive_) return;
        uint32_t backSid = ewsSavedSid_;
        uint8_t backScids = ewsSavedScids_;
        bool hadSaved = ewsHasSaved_;
        uint32_t alertSid = ewsAlertSid_;
        ewsAutoActive_ = false;
        ewsHasSaved_ = false;
        if (hadSaved && backSid != 0) {
            selectService(backSid, backScids, Slot::Primary);
            sink_(events::ewsSwitched(backSid, static_cast<int64_t>(alertSid)));
        }
    }
}

// Verbindet die Callbacks des Empfangspfads (OFDM-Thread) mit der EventSink.
void DabCore::wireCallbacks() {
    auto& cb = *callbacks_;
    cb.synced = [this](bool s) {
        { std::lock_guard<std::mutex> lk(stateM_); state_["synced"] = s; }
        sink_(events::synced(s));
        std::lock_guard<std::mutex> lk(agcM_);
        if (agcCtl_) agcCtl_->onSynced(s, agcNowMs());
    };
    cb.noSignal = [this] {
        sink_(events::noSignal(currentChannel()));
        // Akquisitions-Ramp: jedes no_signal hebt den Gain eine Stufe; der
        // Scan wartet, solange noch eine Stufe zu bewerten ist.
        bool moreToTry = false;
        {
            std::lock_guard<std::mutex> lk(agcM_);
            if (agcCtl_) moreToTry = agcCtl_->onNoSignal(agcNowMs());
        }
        if (scanning_.load() && scan_) scan_->onNoSignal(moreToTry);
    };
    cb.syncLost = [this] { sink_(events::log("debug", "Synchronisation verloren")); };
    cb.snr = [this](float db) {
        lastSnrDb_.store(db);
        auto now = std::chrono::steady_clock::now();
        // Tracking-AGC (Bergsteiger auf dem SNR, AgcController); der
        // ofdmHandler liefert den Wert als EMA etwa alle 0,58 s.
        {
            std::lock_guard<std::mutex> lk(agcM_);
            if (agcCtl_) agcCtl_->onSnr(db, agcNowMs());
        }
        if (now - lastSnr_ < 100ms) return;      // 10 Hz
        lastSnr_ = now;
        sink_(events::snr(db));
    };
    cb.clockError = [this](int ppm) { (void)ppm; };
    cb.corrector = [this](int coarse, float fine) {
        auto now = std::chrono::steady_clock::now();
        if (now - lastFreqOffset_ < 100ms) return;
        lastFreqOffset_ = now;
        sink_(events::frequencyOffset(coarse + static_cast<int32_t>(fine)));
    };
    cb.tii = [this](const std::vector<tiiData>& v) {
        std::vector<std::tuple<uint8_t, uint8_t, float>> tx;
        for (auto& t : v) tx.emplace_back(t.mainId, t.subId, t.strength);
        onTii(tx);
    };
    cb.spectrum = [this](const std::vector<uint8_t>& bins) { sink_(events::spectrum(bins)); };
    cb.iqSamples = [this](const std::vector<int8_t>& iq) { sink_(events::iqSamples(iq)); };
    cb.ficQuality = [this](int ok, int scaler) {
        {
            // Schein-Sync-Erkennung der AGC (Sync ohne dekodierte FIBs)
            std::lock_guard<std::mutex> lk(agcM_);
            if (agcCtl_) agcCtl_->onFicQuality(ok, agcNowMs());
        }
        auto now = std::chrono::steady_clock::now();
        if (now - lastFicQuality_ < 1000ms) return;   // 1 Hz
        lastFicQuality_ = now;
        // v1: Anzeige ok * scaler (Prozent); hier ok von total = 100 / scaler
        sink_(events::ficQuality(static_cast<uint16_t>(ok), static_cast<uint16_t>(100 / scaler)));
    };
    cb.ficBer = [](float) {};
    cb.ensembleName = [this](uint16_t eid, const std::string& name) {
        std::string n = trimRight(name);
        { std::lock_guard<std::mutex> lk(stateM_); state_["ensemble"] = {{"eid", eid}, {"name", n}}; }
        sink_(events::ensembleFound(eid, n, currentChannel()));
    };
    cb.addToEnsemble = [this](const std::string& name, uint32_t sid, int subChId, bool primary) {
        (void)subChId;
        emitService(name, sid, subChId, primary);
    };
    cb.programType = [this](int sid, int pty) {
        // Dienst erneut melden, jetzt mit Programmtyp (siehe protocol.md)
        std::lock_guard<std::mutex> lk(stateM_);
        for (auto& s : state_["services"]) {
            if (s["sid"].get<uint32_t>() == static_cast<uint32_t>(sid)) {
                s["pty"] = pty;
                sink_(json{{"type", "service_added"}, {"service", s}});
            }
        }
    };
    cb.changeInConfiguration = [this] {
        { std::lock_guard<std::mutex> lk(stateM_); state_["services"] = json::array(); }
        sink_(events::ensembleReconfigured());
    };
    cb.announcement = [this](int sid, int flags) {
        uint8_t subCh = 0;
        if (ofdm_) {
            int idx = ofdm_->fic().getServiceComp(static_cast<uint32_t>(sid), 0);
            if (idx >= 0) {
                audiodata ad;
                ofdm_->fic().audioData(idx, ad);
                if (ad.defined) subCh = static_cast<uint8_t>(ad.subchId);
            }
        }
        sink_(events::announcement(static_cast<uint16_t>(flags), subCh, flags != 0));
    };
    cb.nrServices = [](int) {};
    cb.ltoEcc = [this](int lto, int) { lto_.store(lto); };
    cb.freqListChanged = [] {};
    cb.clockTime = [this](uint32_t mjd, int h, int m, int s, int ltoMinutes,
                          int, int, int, int, int) {
        int64_t unix = (static_cast<int64_t>(mjd) - 40587) * 86400 + h * 3600 + m * 60 + s;
        // Zeitstempel der Timeshift-Rahmen (Plan 1.2): letzter bekannter
        // Wert, zwischen zwei Meldungen mit der steady_clock fortgeschrieben.
        clockUnix_.store(unix);
        clockAtMs_.store(agcNowMs());
        {
            std::lock_guard<std::mutex> lk(stateM_);
            state_["clock_time"] = {{"unix_utc", unix}, {"lto_minutes", ltoMinutes}};
        }
        sink_(events::clockTime(unix, static_cast<int16_t>(ltoMinutes)));
    };
    cb.alarmFlag = [this](bool active) {
        sink_(events::log("info", std::string("FIG 0/0 Alarm-Flag ") + (active ? "gesetzt" : "geloescht")));
    };
    cb.ewfAlarm = [this](bool active, int subChId) {
        sink_(events::ewfAlarm(active, static_cast<uint8_t>(subChId < 0 ? 0 : subChId)));
    };
    cb.ewsAlert = [this](int phase, int subChId, int stage, int stageRaw, int iid, const std::vector<std::string>& loc) {
        EwsPhase p = phase == 0 ? EwsPhase::PreTrigger : phase == 1 ? EwsPhase::Trigger
                   : phase == 2 ? EwsPhase::Sustain : EwsPhase::End;
        // v1 radio.cpp: Stufe 7 ("Test") ist eine Testwarnung, keine echte.
        bool isTest = (stage & 7) == 7;
        // Entscheidung 5: der Alarm verlaesst den Zeitversatz immer – der
        // Hoerer muss die Warnung live bekommen, nicht aus dem Puffer.
        if (p == EwsPhase::Trigger && timeshift_ && timeshift_->attached())
            timeshift_->dropToLive("Notfallwarnung");
        // Erst die Meldung selbst, dann ihre Folgen (Umschalten): die App
        // soll den Alarm sehen, bevor ews_switched eintrifft.
        sink_(events::ewsAlert(p, static_cast<uint8_t>(subChId), static_cast<uint8_t>(stage),
                               static_cast<uint8_t>(stageRaw), static_cast<uint16_t>(iid), loc, isTest));
        if (phase != 0) handleEwsAutoswitch(phase, static_cast<uint32_t>(subChId), isTest);
    };
    cb.ewsAlive = [this](int subChId) { sink_(events::ewsAlive(subChId)); };
    cb.ewsPresent = [this] { sink_(events::ewsPresent()); };
    cb.log = [this](const char* level, const std::string& text) { sink_(events::log(level, text)); };
}

// service_added aus dem FIC-Zustand zusammenstellen (v1: addToEnsemble-Signal,
// die GUI holte sich audioData/packetData nach).
void DabCore::emitService(const std::string& rawName, uint32_t sid, int subChId, bool primary) {
    if (!ofdm_) return;
    ServiceInfo s;
    s.sid = sid;
    s.name = trimRight(rawName);
    s.isPrimary = primary;
    auto& fic = ofdm_->fic();
    int index = primary ? fic.getServiceComp(sid, 0) : fic.getServiceComp(s.name);
    if (index >= 0) {
        uint8_t tmid = fic.serviceType(index);
        if (tmid == 0) {
            audiodata ad;
            fic.audioData(index, ad);
            if (ad.defined) {
                s.isAudio = true;
                s.scids = static_cast<uint8_t>(ad.SCIds);
                s.subCh = static_cast<uint8_t>(ad.subchId);
                s.bitrateKbps = static_cast<uint16_t>(ad.bitRate);
                s.pty = static_cast<uint8_t>(ad.programType);
            }
        } else {
            packetdata pd;
            fic.packetData(index, pd);
            s.isAudio = false;
            if (pd.defined) {
                s.scids = static_cast<uint8_t>(pd.SCIds);
                s.subCh = static_cast<uint8_t>(pd.subchId);
                s.bitrateKbps = static_cast<uint16_t>(pd.bitRate);
            }
        }
    } else if (subChId >= 0) {
        s.subCh = static_cast<uint8_t>(subChId);
    }
    bool replaced = false;
    {
        std::lock_guard<std::mutex> lk(stateM_);
        for (auto& e : state_["services"]) {
            if (e["sid"].get<uint32_t>() == sid && e["scids"].get<uint8_t>() == s.scids) {
                e = s.toJson(); replaced = true;
            }
        }
        if (!replaced) state_["services"].push_back(s.toJson());
    }
    sink_(events::serviceAdded(s));
    maybeAutoSelect(s, replaced);
    if (!s.isAudio) maybeStartEpg(s);
}

// v1 radio.cpp addToEnsemble: ein Paketdienst mit Appl-Type 7 (SPI, FIG 0/13)
// laeuft immer im Hintergrund (Logos, EPG) – auch ohne gehoerten Dienst.
void DabCore::maybeStartEpg(const ServiceInfo& s) {
    if (!epgEnabled_.load() || scanning_.load() || !ofdm_) return;
    if (!ofdm_->fic().is_SPI(s.sid)) return;
    if (msc_->serviceRuns(s.sid, s.subCh)) return;
    sink_(events::log("info", "SPI/EPG-Dienst erkannt: " + s.name + " (SId " + std::to_string(s.sid) + ")"));
    selectService(s.sid, s.scids, Slot::Background);
    std::lock_guard<std::mutex> lk(serviceM_);
    if (auto* rs = findLocked(Slot::Background, s.sid)) rs->autoEpg = true;
}

void DabCore::setEpg(bool enabled) {
    epgEnabled_.store(enabled);
    { std::lock_guard<std::mutex> lk(stateM_); state_["epg_enabled"] = enabled; }
    if (!enabled) {
        // nur die vom Kern gestarteten SPI-Dienste beenden
        std::lock_guard<std::mutex> lk(serviceM_);
        std::vector<RunningService*> victims;
        for (auto& rs : services_) if (rs->autoEpg) victims.push_back(rs.get());
        for (auto* v : victims) stopOneLocked(v);
        updateServiceState();
        return;
    }
    std::vector<ServiceInfo> data;
    {
        std::lock_guard<std::mutex> lk(stateM_);
        for (auto& e : state_["services"]) {
            if (e.value("is_audio", true)) continue;
            ServiceInfo si;
            si.sid = e.value("sid", 0u); si.scids = e.value("scids", 0); si.name = e.value("name", "");
            si.isAudio = false; si.subCh = e.value("sub_ch", 0);
            data.push_back(si);
        }
    }
    for (auto& si : data) maybeStartEpg(si);
}

uint16_t DabCore::currentEid() const {
    std::lock_guard<std::mutex> lk(stateM_);
    return state_["ensemble"].is_object() ? static_cast<uint16_t>(state_["ensemble"].value("eid", 0)) : 0;
}

bool DabCore::ensembleHasSid(uint32_t sid) const {
    std::lock_guard<std::mutex> lk(stateM_);
    for (auto& e : state_["services"])
        if (e.value("sid", 0u) == sid) return true;
    return false;
}

static std::string baseName(const std::string& name) {
    size_t p = name.find_last_of("/\\");
    return p == std::string::npos ? name : name.substr(p + 1);
}

static bool parseHex(const std::string& s, uint32_t& v) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    v = static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 16));
    return true;
}

// "d210_Dlf_320x240.png", "10c4_ASA DE_320x240.png": Hex-SId (4 oder 8
// Zeichen) bis zum ersten '_'. Der SPI-Dienst des Bundesmux traegt auch
// Logos fremder Ensembles (Antenne DE, Bearer e0:11f7:...), deshalb ohne
// Abgleich mit der Dienstliste.
uint32_t DabCore::sidFromLogoName(const std::string& name) const {
    std::string b = baseName(name);
    size_t us = b.find('_');
    if (us == std::string::npos || (us != 4 && us != 8)) return 0;
    uint32_t sid = 0;
    if (!parseHex(b.substr(0, us), sid)) return 0;
    return sid;
}

// v1 extractName: Datum als 8 Ziffern (Jahr 2000..2030) an Position 0..3,
// danach die erste 4-stellige Hex-Zahl, die eine SId des Ensembles ist
// ("w20260914dd230c0.EHB" -> 20260914, 0xD230).
bool DabCore::epgNameParts(const std::string& name, uint32_t& date, uint32_t& sid) const {
    std::string real = baseName(name);
    size_t dotat = real.rfind('.');
    if (dotat == std::string::npos) return false;
    size_t eos = 0;
    for (size_t i = 0; i < 4 && i + 8 <= real.size(); ++i) {
        std::string y4 = real.substr(i, 4);
        if (!std::all_of(y4.begin(), y4.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) continue;
        int y = std::atoi(y4.c_str());
        if (y < 2000 || y > 2030) continue;
        std::string d8 = real.substr(i, 8);
        if (!std::all_of(d8.begin(), d8.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) return false;
        date = static_cast<uint32_t>(std::atol(d8.c_str()));
        eos = i + 8;
        break;
    }
    if (eos == 0) return false;
    for (size_t i = eos; dotat >= 3 && i < dotat - 3; ++i) {
        uint32_t cand = 0;
        if (!parseHex(real.substr(i, 4), cand)) continue;
        if (ensembleHasSid(cand)) { sid = cand; return true; }
    }
    return false;
}

// v1 handle_motObject / process_epgData / showMOTlabel (SPI-Zweig)
void DabCore::onMotObject(RunningService* rs, const std::vector<uint8_t>& data, const std::string& name,
                          int contentType, uint32_t objSid) {
    (void)objSid;
    const uint16_t eid = currentEid();
    const int base = (contentType >> 8) & 0x3F;
    {
        const uint64_t h = fnv1a(data) ^ (static_cast<uint64_t>(contentType) << 56);
        auto it = rs->motSeen.find(name);
        if (it != rs->motSeen.end() && it->second == h) return;   // unveraendert
        rs->motSeen[name] = h;
    }
    if (base == MOTBaseTypeApplication) {           // EPG-Binaerobjekt (0x0701)
        if (scanning_.load()) return;
        if (!rs->epg) rs->epg = std::make_unique<epgCompiler>();
        std::string xml;
        int docType = rs->epg->process_epg(xml, data, lto_.load());
        if (docType == noType) return;
        if (docType == serviceInformationType) {
            sink_(events::epgObject(eid, 0, 0, name, xml));
            return;
        }
        uint32_t date = 0, sid = 0;
        if (!epgNameParts(name, date, sid)) {
            sink_(events::log("debug", "EPG-Objekt ohne Datum/SId im Namen: " + name));
            return;
        }
        sink_(events::epgObject(eid, sid, date, name, xml));
        return;
    }
    if (base == MOTBaseTypeImage || base == MOTBaseTypeText ||
        base == MOTBaseTypeGeneralData || base == MOTBaseTypeTransport) {
        if (name.empty()) return;
        sink_(events::motObject(eid, sidFromLogoName(name), static_cast<uint16_t>(contentType), name, data));
    }
}

// Headless-Auswahl (--service / --all-audio): laeuft im OFDM-Thread.
// Exakter Name/SId gewinnt sofort; ein Teilstring-Treffer ("Dlf" passt auch
// auf "Dlf Kultur") wird erst genommen, wenn die FIC einen Dienst zum zweiten
// Mal meldet und der Kandidat mindestens 1,5 s alt ist (die FIC meldet die
// Dienste in beliebiger Reihenfolge; live kommt "Dlf Kultur" oft vor "Dlf").
void DabCore::maybeAutoSelect(const ServiceInfo& s, bool seenBefore) {
    if (scanning_.load()) return;
    if (autoPending_.empty() && !opt_.autoAllAudio) return;
    auto slotFor = [this](const std::string& wanted) {
        return (!opt_.autoServices.empty() && wanted == opt_.autoServices[0]) ? Slot::Primary : Slot::Background;
    };
    for (size_t i = 0; i < autoPending_.size(); ++i) {
        int m = serviceMatch(s, autoPending_[i]);
        if (m == 2) {
            std::string wanted = autoPending_[i];
            autoPending_.erase(autoPending_.begin() + static_cast<long>(i));
            autoCandidates_.erase(wanted);
            selectService(s.sid, s.scids, slotFor(wanted));
            return;
        }
        if (m == 1 && !autoCandidates_.count(autoPending_[i])) {
            autoCandidates_[autoPending_[i]] = s;
            if (autoCandidates_.size() == 1) autoCandidateSince_ = std::chrono::steady_clock::now();
        }
    }
    const bool graceOver = std::chrono::steady_clock::now() - autoCandidateSince_ >= 1500ms;
    if (seenBefore && graceOver && !autoCandidates_.empty()) {
        for (size_t i = 0; i < autoPending_.size();) {
            auto it = autoCandidates_.find(autoPending_[i]);
            if (it == autoCandidates_.end()) { ++i; continue; }
            std::string wanted = autoPending_[i];
            ServiceInfo cand = it->second;
            autoPending_.erase(autoPending_.begin() + static_cast<long>(i));
            autoCandidates_.erase(it);
            selectService(cand.sid, cand.scids, slotFor(wanted));
        }
    }
    if (opt_.autoAllAudio && s.isAudio) {
        if (msc_->serviceRuns(s.sid, s.subCh)) return;
        selectService(s.sid, s.scids, Slot::Background);
    }
}

bool DabCore::handle(const json& c) {
    const std::string type = c.value("type", "");

    if (type == "shutdown") {
        stopSpike();
        closeDevice();
        stopFrameDump();
        sink_(events::exiting("shutdown"));
        return false;
    }
    if (type == "get_state") { emitState(); emitAudioDevices(); return true; }
    if (type == "open_device") { openDevice(c.value("source", json::object())); return true; }
    if (type == "close_device") { closeDevice(); return true; }
    if (type == "set_channel") {
        stopScan();
        const std::string ch = c.value("channel", "");
        if (source_ && ofdm_ && !source_->isFileInput()) {
            tuneChannel(ch, false);
        } else {
            // Ohne Geraet (oder bei Datei-Quellen) nur merken; ein spaeteres
            // open_device stellt den gemerkten Kanal ein.
            std::lock_guard<std::mutex> lk(stateM_);
            state_["channel"] = ch;
        }
        return true;
    }
    if (type == "set_gain") { setGain(c.value("gain", json::object())); return true; }
    if (type == "set_agc") { setAgc(c.value("enabled", true)); return true; }
    if (type == "set_ppm") { setPpm(c.value("ppm", 0)); return true; }
    if (type == "start_scan") {
        std::vector<std::string> channels;
        if (c.contains("channels") && c["channels"].is_array())
            for (auto& x : c["channels"]) if (x.is_string()) channels.push_back(x.get<std::string>());
        startScan(channels, c.value("mode", "single"));
        return true;
    }
    if (type == "stop_scan") { stopScan(); return true; }
    if (type == "start_iq_dump") { startIqDump(c.value("path", "")); return true; }
    if (type == "stop_iq_dump") { stopIqDump(); return true; }
    if (type == "select_service") {
        if (scanning_.load()) { sink_(events::log("warn", "select_service waehrend des Scans abgelehnt")); return true; }
        selectService(c.value("sid", 0u), static_cast<uint8_t>(c.value("scids", 0)), slotFromJson(c));
        return true;
    }
    if (type == "stop_service") {
        stopService(slotFromJson(c), c.contains("sid") && c["sid"].is_number() ? c["sid"].get<int64_t>() : -1);
        return true;
    }
    if (type == "set_volume") {
        int v = std::clamp(c.value("percent", 70), 0, 100);
        { std::lock_guard<std::mutex> lk(stateM_); state_["volume_percent"] = v; }
        std::lock_guard<std::mutex> lk(serviceM_);
        for (auto& rs : services_) if (rs->audio) rs->audio->setVolume(v);
        return true;
    }
    if (type == "set_mute") {
        bool m = c.value("muted", false);
        { std::lock_guard<std::mutex> lk(stateM_); state_["muted"] = m; }
        std::lock_guard<std::mutex> lk(serviceM_);
        for (auto& rs : services_) if (rs->audio) rs->audio->setMute(m);
        return true;
    }
    if (type == "set_audio_device") {
        if (c.contains("index") && c["index"].is_number()) {
            if (!audioSink_->selectDevice(c["index"].get<int>()))
                sink_(events::log("warn", "Audiogeraet nicht waehlbar"));
        }
        emitAudioDevices();
        return true;
    }
    if (type == "start_recording") {
        startRecording(slotFromJson(c), c.contains("sid") && c["sid"].is_number() ? c["sid"].get<int64_t>() : -1,
                       c.value("path", ""), c.value("format", json::object()),
                       c.contains("pre_s") && c["pre_s"].is_number() ? c["pre_s"].get<double>() : 0.0);
        return true;
    }
    if (type == "stop_recording") {
        stopRecording(slotFromJson(c), c.contains("sid") && c["sid"].is_number() ? c["sid"].get<int64_t>() : -1);
        return true;
    }
    if (type == "start_frame_dump") { startFrameDump(c.value("path", "")); return true; }
    if (type == "stop_frame_dump") { stopFrameDump(); return true; }
    if (type == "set_epg") { setEpg(c.value("enabled", true)); return true; }
    if (type == "set_ews") {
        std::lock_guard<std::mutex> lk(stateM_);
        state_["ews_enabled"] = c.value("enabled", true);
        state_["ews_autoswitch"] = c.value("autoswitch", true);
        return true;
    }
    if (type == "set_scopes") {
        spectrumOn_ = c.value("spectrum", false);
        iqOn_ = c.value("iq", false);
        scopeRateHz_ = std::clamp(c.value("rate_hz", 5), 1, 10);
        applyScopes();
        return true;
    }
    // --- Timeshift (Plan M4 1.3) ---
    if (type == "timeshift_configure") {
        const std::string backing = c.contains("backing") && c["backing"].is_object()
                                        ? c["backing"].value("backing", "ram") : "ram";
        if (backing != "ram")
            sink_(events::log("warn", "timeshift_configure: backing \"" + backing +
                                      "\" wird vorerst wie ram behandelt (Entscheidung 4)"));
        timeshift_->configure(static_cast<uint32_t>(std::max(0, c.value("capacity_s", 3600))));
        return true;
    }
    if (type == "timeshift_pause") { timeshift_->pause(); return true; }
    if (type == "timeshift_play") { timeshift_->play(); return true; }
    if (type == "timeshift_live") { timeshift_->live(); return true; }
    if (type == "timeshift_seek") { timeshift_->seek(c.value("offset_s", 0.0)); return true; }
    if (type == "timeshift_skip") { timeshift_->skip(c.value("delta_s", 0.0)); return true; }
    if (type == "export_timeshift_range") {
        exportTimeshiftRange(c.value("from_s", 0.0), c.value("to_s", 0.0), c.value("path", ""),
                             c.value("format", json::object()));
        return true;
    }
    if (type == "set_tii") {
        params_->tiiEnabled = c.value("enabled", true);
        params_->tiiThreshold = static_cast<int16_t>(c.value("threshold", 6));
        // dx_mode: in v1 ein Anzeigemodus (mehr Sender, Abstandsberechnung);
        // im Kern heute ohne Wirkung, wird nur gemerkt (state.tii_dx_mode).
        params_->dxMode = c.value("dx_mode", false);
        if (ofdm_) { ofdm_->setTIIThreshold(params_->tiiThreshold); ofdm_->setDXMode(params_->dxMode); }
        return true;
    }

    // Alles Weitere (Timeshift, MP3/AAC-Aufnahme, ...) folgt in M3/M4.
    sink_(events::log("debug", "Kommando noch ohne Wirkung: " + type));
    return true;
}

// state_snapshot: der gemerkte Zustand plus alles, was eine neu verbundene
// App zum Wiederaufbau der Anzeige braucht (je laufendem Dienst der letzte
// DLS/DL+/Slide-Stand, Codec, Aufnahme; Scopes, TII, SNR, Uhrzeit).
// Sperrreihenfolge wie ueberall: serviceM_ vor stateM_.
void DabCore::emitState() {
    std::lock_guard<std::mutex> sl(serviceM_);
    json running = json::array();
    for (auto& rs : services_) {
        json e = {{"slot", slotName(rs->slot)}, {"sid", rs->sid}, {"scids", rs->scids},
                  {"name", rs->name}, {"is_audio", rs->isAudio}};
        {
            std::lock_guard<std::mutex> pl(rs->padM);
            e["codec"] = rs->codec.is_null() ? json(nullptr) : rs->codec;
            e["stereo"] = rs->stereo;
            e["dls"] = rs->lastDls.empty() ? json(nullptr) : json(rs->lastDls);
            e["dl_plus"] = rs->lastDlPlus.is_null() ? json(nullptr) : rs->lastDlPlus;
            e["slide"] = rs->lastSlide.is_null() ? json(nullptr) : rs->lastSlide;
        }
        json rec = {{"active", false}, {"path", nullptr}, {"bytes", 0}, {"seconds", 0.0}};
        if (rs->audio && rs->audio->recording()) {
            rec["active"] = true;
            rec["path"] = rs->audio->recordingPath();
            rec["bytes"] = rs->audio->recordingBytes();
            rec["seconds"] = rs->audio->recordingSeconds();
        }
        e["recording"] = rec;
        running.push_back(e);
    }
    std::lock_guard<std::mutex> lk(stateM_);
    json st = state_;
    // dab-api CoreState.ensemble ist Option<(u16, String)> -> [eid, name]
    if (state_["ensemble"].is_object())
        st["ensemble"] = json::array({state_["ensemble"]["eid"], state_["ensemble"]["name"]});
    st["running"] = running;
    st["tii_enabled"] = params_->tiiEnabled;
    st["tii_threshold"] = params_->tiiThreshold;
    st["tii_dx_mode"] = params_->dxMode;
    st["scopes"] = {{"spectrum", spectrumOn_.load()}, {"iq", iqOn_.load()}, {"rate_hz", scopeRateHz_.load()}};
    st["timeshift"] = timeshiftJson();
    st["snr"] = lastSnrDb_.load();
    st["scanning"] = scanning_.load();
    st["ppm"] = ppm_;
    sink_(json{{"type", "state_snapshot"}, {"state", st}});
}

// --- Dienste ----------------------------------------------------------------

RunningService* DabCore::findLocked(Slot slot, int64_t sid) {
    for (auto& rs : services_)
        if (rs->slot == slot && (sid < 0 || rs->sid == static_cast<uint32_t>(sid))) return rs.get();
    return nullptr;
}

void DabCore::updateServiceState() {
    std::lock_guard<std::mutex> lk(stateM_);
    state_["primary"] = nullptr;
    state_["background"] = nullptr;
    bool rec = false;
    for (auto& rs : services_) {
        json e = json::array({rs->sid, rs->scids});
        if (rs->slot == Slot::Primary) state_["primary"] = e;
        else if (state_["background"].is_null()) state_["background"] = e;
        if (rs->audio && rs->audio->recording()) rec = true;
    }
    state_["recording"] = rec;
}

// Callbacks des Backends an Ereignisse binden (laufen im Backend-Thread).
void DabCore::wireBackend(RunningService* rs) {
    auto& cb = *rs->cb;
    const Slot slot = rs->slot;
    const uint32_t sid = rs->sid;
    const uint8_t scids = rs->scids;
    cb.log = [this](const char* level, const std::string& t) { sink_(events::log(level, t)); };
    cb.stats = [this, slot, sid](int fe, int rse, int aac, int rsc) {
        sink_(events::serviceStats(slot, sid, static_cast<uint16_t>(std::min(fe, 65535)),
                                   static_cast<uint16_t>(std::min(rse, 65535)),
                                   static_cast<uint16_t>(std::min(aac, 65535)),
                                   static_cast<uint16_t>(std::min(rsc, 65535))));
    };
    // dls nur bei Aenderung (v1: dl-cache in der GUI); DL+ kommt je Kommando
    cb.dls = [this, slot, sid, rs](const std::string& t) {
        {
            std::lock_guard<std::mutex> pl(rs->padM);
            if (t == rs->lastDls) return;
            rs->lastDls = t;
        }
        sink_(events::dls(slot, sid, t));
    };
    cb.dlPlus = [this, slot, sid, rs](bool it, bool ir, const std::vector<std::pair<uint8_t, std::string>>& tags) {
        json ev = events::dlPlus(slot, sid, it, ir, tags);
        {
            std::lock_guard<std::mutex> pl(rs->padM);
            rs->lastDlPlus = {{"item_toggle", ev["item_toggle"]}, {"item_running", ev["item_running"]}, {"tags", ev["tags"]}};
        }
        sink_(std::move(ev));
    };
    cb.motObject = [this, slot, rs](const std::vector<uint8_t>& data, const std::string& name,
                                    int contentType, bool dirElement, uint32_t objSid) {
        (void)dirElement;
        // X-PAD-Slides eines Audiodienstes -> mot_slide; alles aus
        // Paketdiensten (SPI: Logos, EPG) -> onMotObject.
        if (rs->isAudio) {
            if (((contentType >> 8) & 0x3F) == MOTBaseTypeImage) {
                json ev = events::motSlide(slot, rs->sid, motMimeType(contentType), name, data);
                {
                    std::lock_guard<std::mutex> pl(rs->padM);
                    rs->lastSlide = {{"mime", ev["mime"]}, {"name", ev["name"]}, {"data_b64", ev["data_b64"]}};
                }
                sink_(std::move(ev));
            }
            return;
        }
        onMotObject(rs, data, name, contentType, objSid);
    };
    if (rs->isAudio) {
        cb.pcm = [rs](const complex16* pcm, int n, int rate, bool ps, bool sbr, bool stereo) {
            if (rs->audio) rs->audio->push(pcm, n, rate, ps, sbr, stereo);
        };
        cb.aacFrame = [this, slot](const uint8_t* loas, int len) {
            if (slot != Slot::Primary) return;
            std::lock_guard<std::mutex> lk(frameDumpM_);
            if (frameDump_) std::fwrite(loas, 1, static_cast<size_t>(len), frameDump_);
        };
        rs->audio->setFormatHandler([this, rs, slot, sid, scids](int rate, bool ps, bool sbr, bool stereo, bool first) {
            json ev = events::serviceStarted(slot, sid, scids, true, sbr, ps, static_cast<uint32_t>(rate), stereo);
            {
                std::lock_guard<std::mutex> pl(rs->padM);
                rs->codec = ev["codec"];
                rs->stereo = stereo;
            }
            if (first) {
                rs->started = true;
                sink_(std::move(ev));
            }
            sink_(events::audioFormat(static_cast<uint32_t>(rate), 2));
        });
    }
}

void DabCore::selectService(uint32_t sid, uint8_t scids, Slot slot) {
    std::lock_guard<std::mutex> lk(serviceM_);
    if (closing_ || !ofdm_) {
        sink_(events::log("warn", "select_service ohne geoeffnete Quelle"));
        return;
    }
    auto& fic = ofdm_->fic();
    int index = fic.getServiceComp_SCIds(sid, scids);
    if (index < 0) index = fic.getServiceComp(sid, 0);
    if (index < 0) {
        sink_(events::log("warn", "Dienst nicht in der FIC: SId " + std::to_string(sid)));
        return;
    }
    // Laeuft der Dienst im Slot schon?
    if (auto* ex = findLocked(slot, sid)) {
        if (ex->scids == scids) return;
    }
    if (slot == Slot::Primary) {
        if (auto* p = findLocked(Slot::Primary, -1)) {
            // Regel wie v1 localSelect_SS: kein Umschalten bei laufender Aufnahme
            if (p->audio && p->audio->recording()) {
                sink_(events::log("warn", "Dienstwechsel blockiert: Aufnahme laeuft"));
                return;
            }
            stopOneLocked(p);
        }
    }

    auto rs = std::make_unique<RunningService>();
    rs->slot = slot; rs->sid = sid; rs->scids = scids;
    rs->cb = std::make_unique<BackendCallbacks>();
    uint8_t tmid = fic.serviceType(index);
    if (tmid == 0) {
        auto ad = std::make_unique<audiodata>();
        fic.audioData(index, *ad);
        if (!ad->defined) { sink_(events::log("warn", "Audiodienst noch nicht vollstaendig in der FIC")); return; }
        if (ad->ASCTy != DAB_PLUS) {
            sink_(events::log("error", "MP2-Dienst (DAB alt) wird nicht unterstuetzt: " + trimRight(ad->serviceName)));
            return;
        }
        rs->isAudio = true;
        rs->subCh = static_cast<uint8_t>(ad->subchId);
        rs->name = trimRight(ad->serviceName);
        rs->descriptor = std::move(ad);
        int vol; bool mute;
        { std::lock_guard<std::mutex> sl(stateM_); vol = state_["volume_percent"].get<int>(); mute = state_["muted"].get<bool>(); }
        rs->audio = std::make_unique<AudioPipeline>(slot, sid, sink_, slot == Slot::Primary ? audioSink_.get() : nullptr);
        rs->audio->setVolume(vol);
        rs->audio->setMute(mute);
    } else {
        auto pd = std::make_unique<packetdata>();
        fic.packetData(index, *pd);
        if (!pd->defined) { sink_(events::log("warn", "Paketdienst noch nicht vollstaendig in der FIC")); return; }
        rs->isAudio = false;
        rs->subCh = static_cast<uint8_t>(pd->subchId);
        rs->name = trimRight(pd->serviceName);
        rs->descriptor = std::move(pd);
    }
    wireBackend(rs.get());
    rs->backend = msc_->startBackend(*rs->descriptor, rs->cb.get(),
                                     slot == Slot::Primary ? FORE_GROUND : BACK_GROUND, aacKind_);
    {
        const descriptorType& d = *rs->descriptor;
        sink_(events::log("info", std::string(slot == Slot::Primary ? "Primary" : "Background") + ": " + rs->name +
                                  " (SId " + std::to_string(sid) + ", SubCh " + std::to_string(rs->subCh) +
                                  ", CU " + std::to_string(d.startAddr) + "+" + std::to_string(d.length) +
                                  ", " + std::to_string(d.bitRate) + " kbit/s, " +
                                  (d.shortForm ? "UEP " : "EEP ") + std::to_string(d.protLevel) +
                                  (rs->isAudio ? "" : ", DSCTy " + std::to_string(static_cast<const packetdata&>(d).DSCTy) +
                                                      ", Appl-Type " + std::to_string(static_cast<const packetdata&>(d).appType)) + ")"));
    }
    if (!rs->isAudio) {
        rs->started = true;
        rs->codec = {{"codec", "data"}};
        sink_(events::serviceStartedData(slot, sid, scids));
    }
    RunningService* raw = rs.get();
    // Timeshift: nur der Primary-Audiodienst haengt am Ring (Entscheidung 4).
    // Der Ring beginnt mit dem Dienst neu.
    attachTimeshiftLocked(raw);
    services_.push_back(std::move(rs));
    // Headless --wav: Dump des Primary-Dienstes ab dem ersten PCM-Block
    if (slot == Slot::Primary && raw->audio && !opt_.autoWav.empty() && !autoWavStarted_) {
        std::string err;
        if (raw->audio->startWav(opt_.autoWav, err)) autoWavStarted_ = true;
        else sink_(events::log("error", err));
    }
    updateServiceState();
}

void DabCore::stopOneLocked(RunningService* rs) {
    // Reihenfolge: Timeshift loesen (der Takt-Thread darf danach nicht mehr
    // in den Driver schreiben), dann Backend (danach keine Callbacks mehr),
    // dann Pipeline
    detachTimeshiftLocked(rs);
    if (rs->backend) { msc_->stopBackend(rs->backend); rs->backend = nullptr; }
    if (rs->audio) rs->audio->stop();
    sink_(events::serviceStopped(rs->slot, rs->sid));
    for (size_t i = 0; i < services_.size(); ++i)
        if (services_[i].get() == rs) { services_.erase(services_.begin() + static_cast<long>(i)); break; }
}

void DabCore::stopService(Slot slot, int64_t sid) {
    std::lock_guard<std::mutex> lk(serviceM_);
    std::vector<RunningService*> victims;
    for (auto& rs : services_)
        if (rs->slot == slot && (sid < 0 || rs->sid == static_cast<uint32_t>(sid))) victims.push_back(rs.get());
    for (auto* v : victims) stopOneLocked(v);
    updateServiceState();
}

void DabCore::stopAllServicesLocked() {
    while (!services_.empty()) stopOneLocked(services_.front().get());
    updateServiceState();
}

bool DabCore::startRecording(Slot slot, int64_t sid, const std::string& path, const json& format, double preS) {
    std::string fmt = format.value("format", "wav");
    if (fmt != "wav") {
        sink_(events::log("error", "Aufnahmeformat " + fmt + " folgt spaeter (nur wav)"));
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(serviceM_);
        auto* rs = findLocked(slot, sid);
        if (!rs || !rs->audio) {
            sink_(events::log("warn", "start_recording: kein Audiodienst im Slot"));
            return false;
        }
        std::string err;
        if (!rs->audio->startWav(path, err)) { sink_(events::log("error", err)); return false; }
        updateServiceState();
    }
    // Vorlauf aus dem Ring (Entscheidung 18, Plan M4 1.6): der WAV-Schreiber
    // kann nicht anhaengen, deshalb als eigene Datei <name>_vorlauf.wav.
    if (preS > 0.0 && slot == Slot::Primary && timeshift_ && timeshift_->attached()) {
        const double have = timeshift_->buffer().bufferedSeconds();
        const double pre = std::min(preS, have);
        if (pre < 1.0) {
            sink_(events::log("info", "Aufnahme-Vorlauf: Ring hat erst " +
                                      std::to_string(static_cast<int>(have)) + " s, kein Vorlauf"));
        } else {
            std::string pv = path;
            const size_t dot = pv.find_last_of('.');
            const size_t slash = pv.find_last_of("/\\");
            if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
                pv.insert(dot, "_vorlauf");
            else
                pv += "_vorlauf.wav";
            sink_(events::log("info", "Aufnahme-Vorlauf: " + std::to_string(static_cast<int>(pre)) +
                                      " s aus dem Timeshift-Ring nach " + pv));
            startExportThread(pre, 0.0, pv, false);
        }
    }
    return true;
}

void DabCore::stopRecording(Slot slot, int64_t sid) {
    std::lock_guard<std::mutex> lk(serviceM_);
    for (auto& rs : services_)
        if (rs->slot == slot && (sid < 0 || rs->sid == static_cast<uint32_t>(sid)) && rs->audio)
            rs->audio->stopWav();
    updateServiceState();
}

void DabCore::startFrameDump(const std::string& path) {
    std::lock_guard<std::mutex> lk(frameDumpM_);
    if (frameDump_) std::fclose(frameDump_);
    frameDump_ = std::fopen(path.c_str(), "wb");
    if (!frameDump_) sink_(events::log("error", "kann " + path + " nicht schreiben"));
    else sink_(events::log("info", "Frame-Dump (LOAS/AAC) nach " + path));
}

void DabCore::stopFrameDump() {
    std::lock_guard<std::mutex> lk(frameDumpM_);
    if (frameDump_) { std::fclose(frameDump_); frameDump_ = nullptr; }
}

// --- Timeshift (M4) ------------------------------------------------------------

// Zeitstempel fuer einen Ringrahmen: letzte Ensemble-Uhrzeit (FIG 0/10),
// zwischen zwei Meldungen mit der steady_clock fortgeschrieben; 0, solange
// keine Uhrzeit kam (Plan 1.2).
int64_t DabCore::frameUnixNow() const {
    const int64_t base = clockUnix_.load();
    if (base == 0) return 0;
    const int64_t at = clockAtMs_.load();
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    return base + (now - at) / 1000;
}

// Primary-Audiodienst an den Ring haengen (serviceM_ gehalten). Der Backend-
// Tap laeuft danach ueber den Controller; der Ring ist leer und live.
void DabCore::attachTimeshiftLocked(RunningService* rs) {
    if (!timeshift_ || rs->slot != Slot::Primary || !rs->isAudio || !rs->backend || !rs->descriptor) return;
    {
        std::lock_guard<std::mutex> lk(primaryAudioM_);
        primaryAudio_ = rs->audio.get();
    }
    Backend* be = rs->backend;
    const uint32_t frameBits = static_cast<uint32_t>(rs->descriptor->bitRate) * 24;
    timeshift_->attach(frameBits, [be](const std::vector<uint8_t>& f) { be->deliverFrame(f); });
    be->setFrameTap(timeshift_.get());
}

void DabCore::detachTimeshiftLocked(RunningService* rs) {
    if (!timeshift_ || rs->slot != Slot::Primary || !rs->isAudio) return;
    if (rs->backend) rs->backend->setFrameTap(nullptr);
    {
        std::lock_guard<std::mutex> lk(primaryAudioM_);
        primaryAudio_ = nullptr;
    }
    timeshift_->detach();
}

void DabCore::onTimeshiftState(const TimeshiftSnapshot& s) {
    sink_(events::timeshiftState(timeshiftModeName(s.mode), s.bufferedS, s.offsetS, s.capacityS,
                                 s.frameIndex, s.liveUnix));
}

// state_snapshot.state.timeshift: [mode, buffered_s, offset_s, capacity_s]
// (dab-api CoreState.timeshift ist ein Tupel), null ohne Primary-Dienst.
json DabCore::timeshiftJson() const {
    if (!timeshift_ || !timeshift_->attached()) return json(nullptr);
    const TimeshiftSnapshot s = timeshift_->snapshot();
    return json::array({timeshiftModeName(s.mode), s.bufferedS, s.offsetS, s.capacityS});
}

// Der Controller ruft diese beiden aus attach/detach (serviceM_ ist dann
// gehalten) und aus seinem Takt-Thread; deshalb eine eigene Sperre nur um
// den Zeiger auf die Pipeline des Primary-Slots.
// Sperrreihenfolge: serviceM_ vor primaryAudioM_.
void DabCore::flushPrimaryAudio() {
    std::lock_guard<std::mutex> lk(primaryAudioM_);
    if (primaryAudio_) primaryAudio_->requestFlush();
}

void DabCore::setPrimaryStarved(bool starved) {
    std::lock_guard<std::mutex> lk(primaryAudioM_);
    if (primaryAudio_) primaryAudio_->setStarved(starved);
}

bool DabCore::primaryAudioParams(uint32_t& sid, int16_t& bitRate) {
    std::lock_guard<std::mutex> lk(serviceM_);
    for (auto& rs : services_)
        if (rs->slot == Slot::Primary && rs->isAudio && rs->descriptor) {
            sid = rs->sid;
            bitRate = rs->descriptor->bitRate;
            return true;
        }
    return false;
}

void DabCore::joinExportThread() {
    if (exportThread_.joinable()) exportThread_.join();
}

// export_timeshift_range: from_s/to_s sind Sekunden hinter live (from_s > to_s).
void DabCore::exportTimeshiftRange(double fromS, double toS, const std::string& path, const json& format) {
    const std::string fmt = format.is_object() ? format.value("format", "wav") : "wav";
    if (fmt != "wav")
        sink_(events::log("warn", "export_timeshift_range: Format " + fmt +
                                  " folgt in M4b, es wird WAV geschrieben"));
    if (path.empty()) { sink_(events::log("error", "export_timeshift_range ohne Pfad")); return; }
    if (!(fromS > toS) || toS < 0.0) {
        sink_(events::log("error", "export_timeshift_range: ungueltiger Bereich (from_s > to_s >= 0)"));
        return;
    }
    startExportThread(fromS, toS, path, true);
}

// Rahmen kopieren (unter der Ringsperre) und in einem eigenen Thread
// dekodieren. reportRecordingState: Ende als recording_state melden
// (export_timeshift_range); der Aufnahme-Vorlauf meldet nur ins Log, damit
// die laufende Aufnahme in der App nicht als beendet erscheint.
void DabCore::startExportThread(double fromS, double toS, const std::string& path, bool reportRecordingState) {
    if (!timeshift_ || !timeshift_->attached()) {
        sink_(events::log("warn", "Timeshift-Export: kein Primary-Dienst"));
        return;
    }
    if (exportBusy_.load()) {
        sink_(events::log("warn", "Timeshift-Export laeuft bereits"));
        return;
    }
    joinExportThread();
    uint32_t sid = 0;
    int16_t bitRate = 0;
    if (!primaryAudioParams(sid, bitRate)) {
        sink_(events::log("warn", "Timeshift-Export: kein Primary-Audiodienst"));
        return;
    }
    TimeshiftExportJob job;
    job.frameBits = timeshift_->buffer().frameBits();
    job.frames = timeshift_->buffer().copyRange(fromS, toS, job.packed);
    job.sid = sid;
    job.bitRate = bitRate;
    job.path = path;
    if (job.frames == 0) {
        sink_(events::log("warn", "Timeshift-Export: im Bereich " + std::to_string(fromS) + ".." +
                                  std::to_string(toS) + " s liegen keine Rahmen"));
        if (reportRecordingState) sink_(events::recordingState(Slot::Primary, sid, false, path, 0, 0.0));
        return;
    }
    sink_(events::log("info", "Timeshift-Export: " + std::to_string(job.frames) + " Rahmen (" +
                              std::to_string(static_cast<int>(job.frames * 24 / 1000)) + " s) nach " + path));
    exportBusy_.store(true);
    exportThread_ = std::thread([this, job = std::move(job), sid, reportRecordingState] {
        auto res = timeshiftExportWav(job, aacKind_,
                                      [this](const char* level, const std::string& t) { sink_(events::log(level, t)); });
        if (!res.ok) sink_(events::log("error", res.error));
        if (reportRecordingState)
            sink_(events::recordingState(Slot::Primary, sid, false, res.path, res.bytes, res.seconds));
        exportBusy_.store(false);
    });
}

// --- Quelle -------------------------------------------------------------------

void DabCore::openDevice(const json& source) {
    closeDevice();
    { std::lock_guard<std::mutex> lk(stateM_); state_["source"] = source; }
    const std::string kind = source.value("kind", "");
    if (kind == "file") {
        const std::string path = source.value("path", "");
        if (path == "spike") {
            sink_(events::deviceOpened("spike", "synthetisch", 8));
            startSpike();
            return;
        }
        openFile(path, source.value("loop", false), source.value("fast", opt_.fastReplay));
        return;
    }
    if (kind == "hack_rf") {
        // dab-api: HackRf { serial: Option<String> } -> "serial": null
        std::string serial;
        if (source.contains("serial") && source["serial"].is_string()) serial = source["serial"].get<std::string>();
        openHackRf(serial);
        return;
    }
    if (kind == "rtl_sdr") {
        int index = source.contains("index") && source["index"].is_number() ? source["index"].get<int>() : 0;
        openRtlSdr(index);
        return;
    }
    sink_(events::deviceError("unbekannte Quelle: " + kind));
}

// --- Geraete (M1) -------------------------------------------------------------

void DabCore::openHackRf(const std::string& serial) {
    auto src = std::make_unique<HackRfSource>();
    std::string error;
    if (!src->open(serial, error)) {
        { std::lock_guard<std::mutex> lk(stateM_); state_["source"] = nullptr; }
        sink_(events::deviceError(error));
        return;
    }
    std::string info = "HackRF " + src->boardInfo() + ", libhackrf " + src->libraryVersion() +
                       ", 4,096 MS/s -> 2,048 MS/s (Mittelung 2:1), Bandbreite 1536 kHz";
    attachDevice(std::move(src), info);
}

void DabCore::openRtlSdr(int index) {
    auto src = std::make_unique<RtlSdrSource>();
    std::string error;
    if (!src->open(index, error)) {
        { std::lock_guard<std::mutex> lk(stateM_); state_["source"] = nullptr; }
        sink_(events::deviceError(error));
        return;
    }
    std::string info = "RTL-SDR " + src->model() + ", Tuner " + src->tunerType() + ", " +
                       std::to_string(src->gainTable().size()) + " Gain-Stufen, 2,048 MS/s";
    attachDevice(std::move(src), info);
}

// Geraet uebernehmen: Ereignisse melden, OFDM anlegen; der Empfang beginnt
// mit set_channel bzw. sofort, wenn ein Kanal schon gemerkt ist.
void DabCore::attachDevice(std::unique_ptr<ISampleSource> src, const std::string& info) {
    deviceLost_.store(false);
    source_ = std::move(src);
    fileSource_ = nullptr;
    source_->setErrorCallback([this](const std::string& msg) { onDeviceLost(msg); });
    if (ppm_ != 0) source_->setPpm(ppm_);
    // Gain-Satz aus einem set_gain vor open_device anwenden, sonst die
    // Geraete-Defaults (HackRF: LNA 40 / VGA 24 / AMP aus) uebernehmen.
    if (pendingGain_) {
        source_->setGain(*pendingGain_);
        pendingGain_.reset();
    }
    sink_(events::deviceOpened(source_->name(), source_->serial(), static_cast<uint8_t>(source_->bitDepth())));
    sink_(events::log("info", info));
    emitGain();
    // AGC auf den Gain-Stufen des Geraets (HackRF VGA/2 + AMP, RTL-SDR
    // Tabellenindex); Startpunkt = der jetzt gesetzte Gain.
    if (source_->gainStepCount() > 0) {
        AgcConfig cfg;
        cfg.maxStep = source_->gainStepCount() - 1;
        cfg.acqIncrement = source_->gainAcqIncrement();
        cfg.hasAmp = source_->hasAmp();
        cfg.ampTrialStep = cfg.fallbackStep = source_->gainDefaultStep();
        cfg.trackStep = source_->gainTrackStep();
        auto ctl = std::make_unique<AgcController>(cfg, [this](int step, bool amp) {
            source_->setGainStep(step, amp);
            emitGain();
        });
        ctl->setStart(source_->gainStep(), source_->gain().amp, agcNowMs());
        ctl->setEnabled(agc_, agcNowMs());
        std::lock_guard<std::mutex> lk(agcM_);
        agcCtl_ = std::move(ctl);
    }

    autoPending_ = opt_.autoServices;
    autoCandidates_.clear();
    autoWavStarted_ = false;
    ofdm_ = std::make_unique<ofdmHandler>(source_.get(), params_.get(), msc_.get(), callbacks_.get(), cpuSupport_);
    applyScopes();
    std::string ch = currentChannel();
    if (!ch.empty()) tuneChannel(ch, false);
}

bool DabCore::tuneChannel(const std::string& channel, bool scan) {
    const int32_t freq = channelFrequencyHz(channel);
    if (freq == 0) {
        sink_(events::log("error", "unbekannter Kanal: " + channel));
        return false;
    }
    if (!source_ || !ofdm_) {
        sink_(events::log("warn", "set_channel ohne Geraet: " + channel));
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(serviceM_);
        stopAllServicesLocked();
    }
    const bool trace = std::getenv("DABCORE_TRACE") != nullptr;
    auto t0 = std::chrono::steady_clock::now();
    ofdm_->stop();
    source_->stop();
    {
        std::lock_guard<std::mutex> lk(stateM_);
        state_["channel"] = channel;
        state_["synced"] = false;
        state_["ensemble"] = nullptr;
        state_["services"] = json::array();
    }
    autoPending_ = opt_.autoServices;
    autoCandidates_.clear();
    ofdm_->setScanMode(scan);
    // AGC: Ramp neu ab dem gesetzten bzw. zuletzt erfolgreichen Gain (der
    // OFDM-Thread steht, deshalb hier gefahrlos unter agcM_).
    {
        std::lock_guard<std::mutex> lk(agcM_);
        if (agcCtl_) agcCtl_->onRetune(agcNowMs());
    }
    // v1 radio.cpp startChannel: restartReader (freq, SAMPLERATE / 10)
    if (!source_->restart(freq, SAMPLERATE / 10)) {
        sink_(events::deviceError("Kanal " + channel + " (" + std::to_string(freq / 1000) + " kHz) nicht einstellbar"));
        return false;
    }
    ofdm_->start();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    sink_(events::log("info", "Kanal " + channel + " (" + std::to_string(freq / 1000) + " kHz)" +
                              (scan ? ", Scan" : "") + ", Umschaltung " + std::to_string(ms) + " ms"));
    if (trace) std::fprintf(stderr, "tuneChannel %s: %lld ms\n", channel.c_str(), static_cast<long long>(ms));
    return true;
}

void DabCore::onDeviceLost(const std::string& message) {
    sink_(events::deviceError(message));
    if (!deviceLost_.exchange(true) && deviceLostHandler_) deviceLostHandler_();
}

// --- Gain / AGC / ppm -----------------------------------------------------------

int64_t DabCore::agcNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void DabCore::emitGain() {
    DeviceGain g;
    if (source_) g = source_->gain();
    else {
        std::lock_guard<std::mutex> lk(stateM_);
        g.lna = state_["gain"].value("lna", 0);
        g.vga = state_["gain"].value("vga", 0);
        g.amp = state_["gain"].value("amp", false);
    }
    {
        std::lock_guard<std::mutex> lk(stateM_);
        state_["gain"] = {{"lna", g.lna}, {"vga", g.vga}, {"amp", g.amp}};
        state_["agc"] = agc_;
    }
    sink_(events::gainChanged(g.lna, g.vga, g.amp, agc_));
}

void DabCore::setGain(const json& gain) {
    DeviceGain g;
    {
        std::lock_guard<std::mutex> lk(stateM_);
        g.lna = gain.value("lna", state_["gain"].value("lna", 0));
        g.vga = gain.value("vga", state_["gain"].value("vga", 0));
        g.amp = gain.value("amp", state_["gain"].value("amp", false));
    }
    if (source_ && !source_->isFileInput()) {
        source_->setGain(g);
        // Bei AGC an ist das der neue Ausgangspunkt (Ramp/Tracking neu)
        std::lock_guard<std::mutex> lk(agcM_);
        if (agcCtl_) agcCtl_->setStart(source_->gainStep(), source_->gain().amp, agcNowMs());
    } else {
        // Ohne Geraet merken; attachDevice wendet den Satz beim Oeffnen an.
        pendingGain_ = g;
        std::lock_guard<std::mutex> lk(stateM_);
        state_["gain"] = {{"lna", g.lna}, {"vga", g.vga}, {"amp", g.amp}};
        return;
    }
    emitGain();
}

void DabCore::setAgc(bool enabled) {
    agc_ = enabled;
    {
        std::lock_guard<std::mutex> lk(agcM_);
        // Waehrend des Scans laeuft die Ramp ohnehin; der Wunsch gilt danach.
        if (agcCtl_ && !scanForcedAgc_) agcCtl_->setEnabled(enabled, agcNowMs());
    }
    emitGain();
}

void DabCore::setPpm(int ppm) {
    ppm_ = ppm;
    { std::lock_guard<std::mutex> lk(stateM_); state_["ppm"] = ppm; }
    if (source_ && !source_->isFileInput()) source_->setPpm(ppm);
    sink_(events::log("info", "ppm-Korrektur " + std::to_string(ppm)));
}

// --- Scan -----------------------------------------------------------------------

void DabCore::startScan(const std::vector<std::string>& channelsIn, const std::string& modeName) {
    stopScan();
    if (!source_ || !ofdm_ || source_->isFileInput()) {
        sink_(events::log("warn", "Scan nur mit Geraet (HackRF/RTL-SDR)"));
        sink_(events::scanFinished());
        return;
    }
    std::vector<std::string> channels = channelsIn;
    if (channels.empty())
        for (auto& c : bandIIIChannels()) channels.push_back(c.name);
    ScanMode mode = scanModeFromName(modeName);

    ScanHooks hooks;
    hooks.tune = [this](const std::string& ch) { return tuneChannel(ch, true); };
    hooks.snapshot = [this] {
        ScanSnapshot s;
        std::lock_guard<std::mutex> lk(stateM_);
        if (state_["ensemble"].is_object()) {
            s.eid = state_["ensemble"].value("eid", -1);
            s.ensemble = state_["ensemble"].value("name", "");
        }
        for (auto& e : state_["services"]) {
            ServiceInfo si;
            si.sid = e.value("sid", 0u); si.scids = e.value("scids", 0); si.name = e.value("name", "");
            si.isAudio = e.value("is_audio", true); si.isPrimary = e.value("is_primary", true);
            si.subCh = e.value("sub_ch", 0); si.bitrateKbps = e.value("bitrate_kbps", 0); si.pty = e.value("pty", 0);
            s.services.push_back(si);
        }
        s.snr = s.eid >= 0 ? lastSnrDb_.load() : 0.0f;
        return s;
    };
    hooks.emit = sink_;
    hooks.finished = [this] { scanFinished(); };

    {
        std::lock_guard<std::mutex> lk(serviceM_);
        stopAllServicesLocked();
    }
    // Die Akquisitions-Ramp laeuft im Scan immer (Suche); bei AGC aus wird
    // der Gain-Satz nach dem Scan wiederhergestellt (scanFinished).
    {
        std::lock_guard<std::mutex> lk(agcM_);
        if (agcCtl_ && !agc_) {
            scanForcedAgc_ = true;
            preScanGain_ = source_->gain();
            agcCtl_->setEnabled(true, agcNowMs());
        }
    }
    scanning_.store(true);
    sink_(events::log("info", "Scan (" + modeName + "): " + std::to_string(channels.size()) + " Kanaele, " +
                              std::to_string(opt_.scanDwellMs) + " ms je Kanal"));
    scan_ = std::make_unique<ScanController>(std::move(hooks), std::move(channels), mode, opt_.scanDwellMs);
    scan_->start();
}

// Scan-Modus verlassen: OFDM verarbeitet wieder den MSC, der Kern bleibt
// auf dem zuletzt eingestellten Kanal (single: letzter Kanal der Liste,
// to_data: der Kanal mit Diensten).
void DabCore::scanFinished() {
    scanning_.store(false);
    if (ofdm_) ofdm_->setScanMode(false);
    bool restore = false;
    {
        std::lock_guard<std::mutex> lk(agcM_);
        if (scanForcedAgc_) {
            scanForcedAgc_ = false;
            if (agcCtl_) agcCtl_->setEnabled(false, agcNowMs());
            restore = source_ != nullptr;
        } else if (agcCtl_) {
            // Ramp auf dem letzten (leeren) Kanal nicht bei VGA 62/AMP stehen lassen
            agcCtl_->finishAcquisition(agcNowMs());
        }
    }
    if (restore) {
        source_->setGain(preScanGain_);
        std::lock_guard<std::mutex> lk(agcM_);
        if (agcCtl_) agcCtl_->setStart(source_->gainStep(), source_->gain().amp, agcNowMs());
    }
    if (restore) emitGain();
    sink_(events::scanFinished());
}

void DabCore::stopScan() {
    if (!scan_) return;
    bool aborted = scan_->stop();
    scan_.reset();
    if (aborted) scanFinished();
}

// --- IQ-Dump --------------------------------------------------------------------

void DabCore::startIqDump(const std::string& path) {
    if (!source_) { sink_(events::log("error", "start_iq_dump ohne Quelle")); return; }
    std::string error;
    if (!source_->startDump(path, error)) { sink_(events::log("error", error)); return; }
    sink_(events::log("info", "IQ-Dump (.uff, 8 Bit) nach " + path));
}

void DabCore::stopIqDump() {
    if (!source_ || !source_->dumping()) return;
    source_->stopDump();
    sink_(events::log("info", "IQ-Dump beendet"));
}

void DabCore::openFile(const std::string& path, bool loop, bool fast) {
    FileSourceOptions fo;
    fo.loop = loop;
    fo.fast = fast;
    fo.durationS = opt_.replayDurationS;
    std::string error;
    std::unique_ptr<FileSourceBase> src;
    if (endsWithNoCase(path, ".uff") || endsWithNoCase(path, ".xml")) {
        auto x = std::make_unique<XmlFileSource>(path, fo);
        if (!x->open(error)) { sink_(events::deviceError(error)); return; }
        src = std::move(x);
    } else {
        auto r = std::make_unique<RawFileSource>(path, fo);
        if (!r->open(error)) { sink_(events::deviceError(error)); return; }
        src = std::move(r);
    }
    fileSource_ = src.get();
    fileSource_->setProgressCallback([this](double pos, double len) {
        sink_(events::fileProgress(pos, len));
    });
    fileSource_->setEndedCallback([this] {
        sink_(events::fileEnded());
        if (fileEndedHandler_) fileEndedHandler_();
    });
    source_ = std::move(src);

    // Kanalname aus der Aufnahmefrequenz ableiten (Band III), falls nicht gesetzt
    int32_t freq = source_->vfoFrequency();
    if (freq > 0) {
        static const struct { const char* name; int khz; } bandIII[] = {
            {"5A",174928},{"5B",176640},{"5C",178352},{"5D",180064},{"6A",181936},{"6B",183648},
            {"6C",185360},{"6D",187072},{"7A",188928},{"7B",190640},{"7C",192352},{"7D",194064},
            {"8A",195936},{"8B",197648},{"8C",199360},{"8D",201072},{"9A",202928},{"9B",204640},
            {"9C",206352},{"9D",208064},{"10A",209936},{"10B",211648},{"10C",213360},{"10D",215072},
            {"11A",216928},{"11B",218640},{"11C",220352},{"11D",222064},{"12A",223936},{"12B",225648},
            {"12C",227360},{"12D",229072},{"13A",230784},{"13B",232496},{"13C",234208},{"13D",235776},
            {"13E",237488},{"13F",239200}};
        for (auto& b : bandIII)
            if (std::abs(freq / 1000 - b.khz) < 100) {
                std::lock_guard<std::mutex> lk(stateM_);
                state_["channel"] = b.name;
            }
    }

    sink_(events::deviceOpened(source_->name(), source_->serial(), static_cast<uint8_t>(source_->bitDepth())));
    sink_(events::log("info", "Datei: " + path + ", " + std::to_string(fileSource_->lengthSeconds()) + " s" +
                              (fast ? ", ohne Pacing" : "") + (loop ? ", Schleife" : "")));

    autoPending_ = opt_.autoServices;
    autoCandidates_.clear();
    autoWavStarted_ = false;
    ofdm_ = std::make_unique<ofdmHandler>(source_.get(), params_.get(), msc_.get(), callbacks_.get(), cpuSupport_);
    applyScopes();
    source_->restart(freq);
    ofdm_->start();
}

void DabCore::closeDevice() {
    if (spikeRunning_) {
        stopSpike();
        sink_(events::deviceClosed());
    }
    stopScan();
    if (ofdm_ || source_) {
        if (source_ && source_->dumping()) stopIqDump();
        // Erst die Dienste (Backends + Pipelines), waehrend der OFDM-Thread
        // noch laeuft; closing_ verhindert, dass der OFDM-Thread (Auto-
        // Auswahl) zwischendurch neue Backends anlegt.
        {
            std::lock_guard<std::mutex> lk(serviceM_);
            closing_ = true;
            stopAllServicesLocked();
        }
        if (ofdm_) {
            int total = 0, good = 0, bad = 0;
            ofdm_->getFrameQuality(&total, &good, &bad);
            size_t nServices = 0;
            { std::lock_guard<std::mutex> lk(stateM_); nServices = state_["services"].size(); }
            sink_(events::log("info", "Empfang beendet: Rahmen " + std::to_string(total) +
                                      " (gut " + std::to_string(good) + ", schlecht " + std::to_string(bad) +
                                      "), Subkanaele " + std::to_string(ofdm_->fic().nrChannels()) +
                                      ", Dienste " + std::to_string(nServices)));
        }
        // Reihenfolge: erst OFDM-Thread (wartet auf Samples), dann Quelle
        const bool trace = std::getenv("DABCORE_TRACE") != nullptr;
        if (trace) std::fprintf(stderr, "closeDevice: ofdm stop\n");
        if (ofdm_) ofdm_->stop();
        if (trace) std::fprintf(stderr, "closeDevice: source stop\n");
        if (source_) source_->stop();
        if (trace) std::fprintf(stderr, "closeDevice: gestoppt\n");
        ofdm_.reset();
        {
            std::lock_guard<std::mutex> lk(agcM_);
            agcCtl_.reset();
            scanForcedAgc_ = false;
        }
        fileSource_ = nullptr;
        source_.reset();
        {
            std::lock_guard<std::mutex> lk(serviceM_);
            closing_ = false;
        }
        sink_(events::deviceClosed());
    }
    std::lock_guard<std::mutex> lk(stateM_);
    state_["source"] = nullptr;
    state_["synced"] = false;
    state_["ensemble"] = nullptr;
    state_["services"] = json::array();
}

// Spike 1 (Analyse 8.2): 10 Hz SNR, 20 Hz Pegel, 10 Hz Spektrum (2048 Bins),
// 1 Hz FIC-Qualitaet, alle 2 s DLS, 2 Hz Fortschritt – misst Pipe/JSON-Durchsatz.
void DabCore::startSpike() {
    spikeRunning_ = true;
    spikeThread_ = std::thread([this] {
        sink_(events::synced(true));
        sink_(events::ensembleFound(0x10BC, "DR Deutschland", "5C"));
        ServiceInfo dlf{0xD210, 0, "Dlf", true, true, 4, 96, 1};
        ServiceInfo asa{0x10C4, 0, "ASA DE", true, true, 1, 32, 0};
        sink_(events::serviceAdded(dlf));
        sink_(events::serviceAdded(asa));
        std::vector<uint8_t> bins(2048);
        auto t0 = std::chrono::steady_clock::now();
        uint64_t tick = 0;   // 20 Hz
        while (spikeRunning_) {
            double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            sink_(events::audioLevel(0.5f + 0.4f * std::sin(t * 3.1f), 0.5f + 0.4f * std::cos(t * 2.7f)));
            if (tick % 2 == 0) {
                sink_(events::snr(12.0f + 3.0f * std::sin(t)));
                if (spectrumOn_) {
                    for (size_t i = 0; i < bins.size(); ++i)
                        bins[i] = static_cast<uint8_t>(128 + 100 * std::sin(0.01 * i + t));
                    sink_(events::spectrum(bins));
                }
            }
            if (tick % 10 == 0) sink_(events::fileProgress(t, 3600.0));
            if (tick % 20 == 0) sink_(events::ficQuality(100, 100));
            if (tick % 40 == 0) sink_(events::dls(Slot::Primary, 0xD210, "Spike-DLS " + std::to_string(tick / 40)));
            if (tick % 20 == 0) sink_(events::ewsAlive(1));
            ++tick;
            std::this_thread::sleep_for(50ms);
        }
    });
}

void DabCore::stopSpike() {
    if (!spikeRunning_) return;
    spikeRunning_ = false;
    if (spikeThread_.joinable()) spikeThread_.join();
}

} // namespace dabcore
