#include "dabcore/core.h"

#include <chrono>
#include <cmath>
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

DabCore::DabCore(EventSink sink, CoreOptions options) : sink_(std::move(sink)), opt_(options) {
    state_ = {
        {"source", nullptr}, {"channel", nullptr},
        {"gain", {{"lna", 0}, {"vga", 0}, {"amp", false}}}, {"agc", true},
        {"synced", false}, {"ensemble", nullptr}, {"services", json::array()},
        {"primary", nullptr}, {"background", nullptr},
        {"volume_percent", 70}, {"muted", false}, {"timeshift", nullptr},
        {"recording", false}, {"ews_enabled", true}, {"ews_autoswitch", true},
    };
    std::vector<std::string> decoders;
#ifdef __WITH_FAAD__
    decoders.push_back("faad2");
#endif
#ifdef DABCORE_FDK_AAC_RUNTIME
    decoders.push_back("fdk-aac(runtime)");
#endif
    sink_(events::ready(version(), 1, decoders));
}

DabCore::~DabCore() { stopSpike(); }

bool DabCore::handle(const json& c) {
    const std::string type = c.value("type", "");

    if (type == "shutdown") {
        stopSpike();
        sink_(events::exiting("shutdown"));
        return false;
    }
    if (type == "get_state") { emitState(); return true; }
    if (type == "open_device") { openDevice(c.value("source", json::object())); return true; }
    if (type == "close_device") { closeDevice(); return true; }
    if (type == "set_channel") {
        state_["channel"] = c.value("channel", "");
        sink_(events::log("info", "set_channel " + state_["channel"].get<std::string>() + " (Empfangspfad folgt in M0)"));
        return true;
    }
    if (type == "set_gain") { state_["gain"] = c.value("gain", state_["gain"]); return true; }
    if (type == "set_agc") { state_["agc"] = c.value("enabled", true); return true; }
    if (type == "set_volume") { state_["volume_percent"] = c.value("percent", 70); return true; }
    if (type == "set_mute") { state_["muted"] = c.value("muted", false); return true; }
    if (type == "set_ews") {
        state_["ews_enabled"] = c.value("enabled", true);
        state_["ews_autoswitch"] = c.value("autoswitch", true);
        return true;
    }
    if (type == "set_scopes") { spectrumOn_ = c.value("spectrum", false); return true; }

    // Alles Weitere (select_service, start_scan, Aufnahme, Timeshift, ...) folgt
    // mit dem portierten Empfangspfad; bis dahin nur bestaetigen.
    sink_(events::log("debug", "Kommando noch ohne Wirkung: " + type));
    return true;
}

void DabCore::emitState() {
    json j = {{"type", "state_snapshot"}, {"state", state_}};
    sink_(j);
}

void DabCore::openDevice(const json& source) {
    closeDevice();
    state_["source"] = source;
    const std::string kind = source.value("kind", "");
    if (kind == "file") {
        const std::string path = source.value("path", "");
        if (path == "spike") {
            sink_(events::deviceOpened("spike", "synthetisch", 8));
            startSpike();
            return;
        }
        sink_(events::deviceError("Datei-Wiedergabe folgt in M0: " + path));
        return;
    }
    if (kind == "hack_rf" || kind == "rtl_sdr") {
        sink_(events::deviceError("Geraet " + kind + " folgt in M1"));
        return;
    }
    sink_(events::deviceError("unbekannte Quelle: " + kind));
}

void DabCore::closeDevice() {
    if (spikeRunning_) {
        stopSpike();
        sink_(events::deviceClosed());
    }
    state_["source"] = nullptr;
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
