#include "dabcore/core.h"

#include "device/xml-file-source.h"
#include "device/raw-file-source.h"
#include "frontend/ofdm-handler.h"
#include "frontend/msc-sink.h"
#include "frontend/receiver-callbacks.h"
#include "support/process-params.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace dabcore {

using namespace std::chrono_literals;

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

DabCore::DabCore(EventSink sink, CoreOptions options) : sink_(std::move(sink)), opt_(options) {
    state_ = {
        {"source", nullptr}, {"channel", nullptr},
        {"gain", {{"lna", 0}, {"vga", 0}, {"amp", false}}}, {"agc", true},
        {"synced", false}, {"ensemble", nullptr}, {"services", json::array()},
        {"primary", nullptr}, {"background", nullptr},
        {"volume_percent", 70}, {"muted", false}, {"timeshift", nullptr},
        {"recording", false}, {"ews_enabled", true}, {"ews_autoswitch", true},
    };
#ifdef __ARCH_X86__
    __builtin_cpu_init();
    int has_avx2 = __builtin_cpu_supports("avx2") != 0 ? AVX_SUPPORT : 0;
    int has_sse4 = __builtin_cpu_supports("sse4.1") != 0 ? SSE_SUPPORT : 0;
    cpuSupport_ = static_cast<uint8_t>(has_avx2 + has_sse4);
#endif
    params_ = std::make_unique<processParams>();
    callbacks_ = std::make_unique<ReceiverCallbacks>();
    mscSink_ = std::make_unique<NullMscSink>();
    wireCallbacks();

    std::vector<std::string> decoders;
#ifdef __WITH_FAAD__
    decoders.push_back("faad2");
#endif
#ifdef DABCORE_FDK_AAC_RUNTIME
    decoders.push_back("fdk-aac(runtime)");
#endif
    sink_(events::ready(version(), 1, decoders));
    sink_(events::log("info", std::string("Viterbi: ") +
                              (cpuSupport_ & AVX_SUPPORT ? "avx2" : cpuSupport_ & SSE_SUPPORT ? "sse4.1" : "scalar")));
}

DabCore::~DabCore() {
    stopSpike();
    closeDevice();
}

std::string DabCore::currentChannel() const {
    std::lock_guard<std::mutex> lk(stateM_);
    return state_["channel"].is_string() ? state_["channel"].get<std::string>() : "";
}

// Verbindet die Callbacks des Empfangspfads (OFDM-Thread) mit der EventSink.
void DabCore::wireCallbacks() {
    auto& cb = *callbacks_;
    cb.synced = [this](bool s) {
        { std::lock_guard<std::mutex> lk(stateM_); state_["synced"] = s; }
        sink_(events::synced(s));
    };
    cb.noSignal = [this] { sink_(events::noSignal(currentChannel())); };
    cb.syncLost = [this] { sink_(events::log("debug", "Synchronisation verloren")); };
    cb.snr = [this](float db) {
        auto now = std::chrono::steady_clock::now();
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
        if (v.empty()) return;
        std::vector<std::tuple<uint8_t, uint8_t, float>> tx;
        for (auto& t : v) tx.emplace_back(t.mainId, t.subId, t.strength);
        sink_(events::tii(tx));
    };
    cb.ficQuality = [this](int ok, int scaler) {
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
    cb.ltoEcc = [](int, int) {};
    cb.freqListChanged = [] {};
    cb.clockTime = [this](uint32_t mjd, int h, int m, int s, int ltoMinutes,
                          int, int, int, int, int) {
        int64_t unix = (static_cast<int64_t>(mjd) - 40587) * 86400 + h * 3600 + m * 60 + s;
        sink_(events::clockTime(unix, static_cast<int16_t>(ltoMinutes)));
    };
    cb.alarmFlag = [this](bool active) {
        sink_(events::log("info", std::string("FIG 0/0 Alarm-Flag ") + (active ? "gesetzt" : "geloescht")));
    };
    cb.ewfAlarm = [this](bool active, int subChId) {
        sink_(events::ewfAlarm(active, static_cast<uint8_t>(subChId < 0 ? 0 : subChId)));
    };
    cb.ewsAlert = [this](int phase, int subChId, int stage, int iid, const std::vector<std::string>& loc) {
        EwsPhase p = phase == 0 ? EwsPhase::PreTrigger : phase == 1 ? EwsPhase::Trigger
                   : phase == 2 ? EwsPhase::Sustain : EwsPhase::End;
        sink_(events::ewsAlert(p, static_cast<uint8_t>(subChId), static_cast<uint8_t>(stage),
                               static_cast<uint16_t>(iid), loc, false));
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
    {
        std::lock_guard<std::mutex> lk(stateM_);
        bool replaced = false;
        for (auto& e : state_["services"]) {
            if (e["sid"].get<uint32_t>() == sid && e["scids"].get<uint8_t>() == s.scids) {
                e = s.toJson(); replaced = true;
            }
        }
        if (!replaced) state_["services"].push_back(s.toJson());
    }
    sink_(events::serviceAdded(s));
}

bool DabCore::handle(const json& c) {
    const std::string type = c.value("type", "");

    if (type == "shutdown") {
        stopSpike();
        closeDevice();
        sink_(events::exiting("shutdown"));
        return false;
    }
    if (type == "get_state") { emitState(); return true; }
    if (type == "open_device") { openDevice(c.value("source", json::object())); return true; }
    if (type == "close_device") { closeDevice(); return true; }
    if (type == "set_channel") {
        std::lock_guard<std::mutex> lk(stateM_);
        state_["channel"] = c.value("channel", "");
        // Bei Datei-Quellen nur merken; Geraete folgen in M1.
        return true;
    }
    if (type == "set_gain") { std::lock_guard<std::mutex> lk(stateM_); state_["gain"] = c.value("gain", state_["gain"]); return true; }
    if (type == "set_agc") { std::lock_guard<std::mutex> lk(stateM_); state_["agc"] = c.value("enabled", true); return true; }
    if (type == "set_volume") { std::lock_guard<std::mutex> lk(stateM_); state_["volume_percent"] = c.value("percent", 70); return true; }
    if (type == "set_mute") { std::lock_guard<std::mutex> lk(stateM_); state_["muted"] = c.value("muted", false); return true; }
    if (type == "set_ews") {
        std::lock_guard<std::mutex> lk(stateM_);
        state_["ews_enabled"] = c.value("enabled", true);
        state_["ews_autoswitch"] = c.value("autoswitch", true);
        return true;
    }
    if (type == "set_scopes") { spectrumOn_ = c.value("spectrum", false); return true; }
    if (type == "set_tii") {
        params_->tiiEnabled = c.value("enabled", true);
        params_->tiiThreshold = static_cast<int16_t>(c.value("threshold", 6));
        if (ofdm_) ofdm_->setTIIThreshold(params_->tiiThreshold);
        return true;
    }

    // Alles Weitere (select_service, start_scan, Aufnahme, Timeshift, ...) folgt
    // mit dem MSC-Pfad; bis dahin nur bestaetigen.
    sink_(events::log("debug", "Kommando noch ohne Wirkung: " + type));
    return true;
}

void DabCore::emitState() {
    std::lock_guard<std::mutex> lk(stateM_);
    json j = {{"type", "state_snapshot"}, {"state", state_}};
    sink_(j);
}

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
    if (kind == "hack_rf" || kind == "rtl_sdr") {
        sink_(events::deviceError("Geraet " + kind + " folgt in M1"));
        return;
    }
    sink_(events::deviceError("unbekannte Quelle: " + kind));
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

    ofdm_ = std::make_unique<ofdmHandler>(source_.get(), params_.get(), mscSink_.get(), callbacks_.get(), cpuSupport_);
    source_->restart(freq);
    ofdm_->start();
}

void DabCore::closeDevice() {
    if (spikeRunning_) {
        stopSpike();
        sink_(events::deviceClosed());
    }
    if (ofdm_ || source_) {
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
        fileSource_ = nullptr;
        source_.reset();
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
            if (tick % 40 == 0) sink_(events::dls(Slot::Primary, "Spike-DLS " + std::to_string(tick / 40)));
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
