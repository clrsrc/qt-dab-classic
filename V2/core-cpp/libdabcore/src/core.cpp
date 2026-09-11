#include "dabcore/core.h"

#include "device/xml-file-source.h"
#include "device/raw-file-source.h"
#include "device/hackrf-source.h"
#include "device/rtlsdr-source.h"
#include "scan/scan-controller.h"
#include "support/dab-channels.h"
#include "frontend/ofdm-handler.h"
#include "frontend/receiver-callbacks.h"
#include "backend/msc-handler.h"
#include "backend/backend.h"
#include "backend/backend-callbacks.h"
#include "backend/audio/aac-decoder.h"
#include "pad/mot-object.h"
#include "audio/audio-sink.h"
#include "audio/portaudio-sink.h"
#include "audio/audio-pipeline.h"
#include "support/process-params.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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
    std::string lastDls;
};

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
    };
#ifdef __ARCH_X86__
    __builtin_cpu_init();
    int has_avx2 = __builtin_cpu_supports("avx2") != 0 ? AVX_SUPPORT : 0;
    int has_sse4 = __builtin_cpu_supports("sse4.1") != 0 ? SSE_SUPPORT : 0;
    cpuSupport_ = static_cast<uint8_t>(has_avx2 + has_sse4);
#endif
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
}

void DabCore::emitAudioDevices() {
    if (!opt_.audio) return;
    sink_(events::audioDevices(audioSink_->devices(), audioSink_->currentDevice()));
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
    cb.noSignal = [this] {
        sink_(events::noSignal(currentChannel()));
        if (scanning_.load() && scan_) scan_->onNoSignal();
    };
    cb.syncLost = [this] { sink_(events::log("debug", "Synchronisation verloren")); };
    cb.snr = [this](float db) {
        lastSnrDb_.store(db);
        auto now = std::chrono::steady_clock::now();
        // SNR-AGC wie v1 (radio.cpp show_snr -> deviceHandler::adjustGain bei
        // jedem SNR-Wert; der ofdmHandler liefert ihn alle 3 Rahmen ~ 0,29 s,
        // also hoechstens ~7 dB/s VGA-Aenderung). Kein weiteres Takten.
        if (agc_ && source_ && !source_->isFileInput()) {
            lastAgc_ = now;
            if (source_->adjustGain(db)) emitGain();
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
    if (type == "get_state") { emitState(); return true; }
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
                       c.value("path", ""), c.value("format", json::object()));
        return true;
    }
    if (type == "stop_recording") {
        stopRecording(slotFromJson(c), c.contains("sid") && c["sid"].is_number() ? c["sid"].get<int64_t>() : -1);
        return true;
    }
    if (type == "start_frame_dump") { startFrameDump(c.value("path", "")); return true; }
    if (type == "stop_frame_dump") { stopFrameDump(); return true; }
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

    // Alles Weitere (Timeshift, MP3/AAC-Aufnahme, ...) folgt in M3/M4.
    sink_(events::log("debug", "Kommando noch ohne Wirkung: " + type));
    return true;
}

void DabCore::emitState() {
    std::lock_guard<std::mutex> lk(stateM_);
    json j = {{"type", "state_snapshot"}, {"state", state_}};
    sink_(j);
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
        if (t == rs->lastDls) return;
        rs->lastDls = t;
        sink_(events::dls(slot, sid, t));
    };
    cb.dlPlus = [this, slot, sid](bool it, bool ir, const std::vector<std::pair<uint8_t, std::string>>& tags) {
        sink_(events::dlPlus(slot, sid, it, ir, tags));
    };
    cb.motObject = [this, slot, rs](const std::vector<uint8_t>& data, const std::string& name,
                                    int contentType, bool dirElement, uint32_t objSid) {
        (void)dirElement;
        // X-PAD-Slides eines Audiodienstes -> mot_slide; alles aus
        // Paketdiensten (SPI/EPG, Logos) -> mot_object mit dem SId des Objekts.
        if (rs->isAudio && ((contentType >> 8) & 0x3F) == MOTBaseTypeImage)
            sink_(events::motSlide(slot, rs->sid, motMimeType(contentType), name, data));
        else
            sink_(events::motObject(objSid, static_cast<uint16_t>(contentType), name, data));
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
            if (first) {
                rs->started = true;
                sink_(events::serviceStarted(slot, sid, scids, true, sbr, ps, static_cast<uint32_t>(rate), stereo));
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
                                  (d.shortForm ? "UEP " : "EEP ") + std::to_string(d.protLevel) + ")"));
    }
    if (!rs->isAudio) {
        rs->started = true;
        sink_(events::serviceStartedData(slot, sid, scids));
    }
    RunningService* raw = rs.get();
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
    // Reihenfolge: erst Backend (danach keine Callbacks mehr), dann Pipeline
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

bool DabCore::startRecording(Slot slot, int64_t sid, const std::string& path, const json& format) {
    std::string fmt = format.value("format", "wav");
    if (fmt != "wav") {
        sink_(events::log("error", "Aufnahmeformat " + fmt + " folgt spaeter (nur wav)"));
        return false;
    }
    std::lock_guard<std::mutex> lk(serviceM_);
    auto* rs = findLocked(slot, sid);
    if (!rs || !rs->audio) {
        sink_(events::log("warn", "start_recording: kein Audiodienst im Slot"));
        return false;
    }
    std::string err;
    if (!rs->audio->startWav(path, err)) { sink_(events::log("error", err)); return false; }
    updateServiceState();
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

    autoPending_ = opt_.autoServices;
    autoCandidates_.clear();
    autoWavStarted_ = false;
    ofdm_ = std::make_unique<ofdmHandler>(source_.get(), params_.get(), msc_.get(), callbacks_.get(), cpuSupport_);
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
    emitGain();
}

void DabCore::setPpm(int ppm) {
    ppm_ = ppm;
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
    if (source_->hasAmp()) {
        hooks.ampGet = [this] { return source_->gain().amp; };
        hooks.ampSet = [this](bool amp) {
            DeviceGain g = source_->gain();
            g.amp = amp;
            source_->setGain(g);
            emitGain();
            return true;
        };
    }
    hooks.emit = sink_;
    hooks.finished = [this] { scanFinished(); };

    {
        std::lock_guard<std::mutex> lk(serviceM_);
        stopAllServicesLocked();
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
