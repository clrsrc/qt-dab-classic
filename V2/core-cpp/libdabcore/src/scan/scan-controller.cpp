// DAB Classic v3 – Band-Scan, siehe scan-controller.h.
#include "scan-controller.h"

#include <chrono>

namespace dabcore {

ScanMode scanModeFromName(const std::string& s) {
    if (s == "to_data") return ScanMode::ToData;
    if (s == "continuous") return ScanMode::Continuous;
    return ScanMode::Single;
}

ScanController::ScanController(ScanHooks hooks, std::vector<std::string> channels, ScanMode mode, int dwellMs)
    : hooks_(std::move(hooks)), channels_(std::move(channels)), mode_(mode), dwellMs_(dwellMs) {}

ScanController::~ScanController() {
    stop();
}

void ScanController::start() {
    if (running_.load()) return;
    stop_.store(false);
    running_.store(true);
    thread_ = std::thread([this] { run(); });
}

bool ScanController::stop() {
    // Genau einer von stop() und dem natuerlichen Ende in run() gewinnt
    // das exchange und ist fuer scan_finished zustaendig.
    bool aborted = !stop_.exchange(true) && thread_.joinable();
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    return aborted;
}

void ScanController::onNoSignal(bool moreToTry) {
    if (moreToTry) return;       // AGC-Ramp laeuft noch: weiter beobachten
    {
        std::lock_guard<std::mutex> lk(m_);
        noSignal_ = true;
    }
    cv_.notify_all();
}

void ScanController::run() {
    using clock = std::chrono::steady_clock;
    const size_t total = channels_.size();
    size_t idx = 0;

    while (!stop_.load() && total > 0) {
        const std::string& ch = channels_[idx];
        hooks_.emit(events::scanProgress(ch, static_cast<uint16_t>(idx), static_cast<uint16_t>(total)));
        {
            std::lock_guard<std::mutex> lk(m_);
            noSignal_ = false;
        }
        if (!hooks_.tune(ch)) break;

        // v1: channelTimer = switchDelay (continuous: 2 * switchDelay); die
        // AGC-Ramp startet den Timer nicht neu. Ende: Verweilzeit abgelaufen
        // oder "kein Signal" bei erschoepfter Ramp.
        auto deadline = clock::now() + std::chrono::milliseconds(mode_ == ScanMode::Continuous ? 2 * dwellMs_ : dwellMs_);
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_until(lk, deadline, [this] { return stop_.load() || noSignal_; });
        }
        if (stop_.load()) break;

        ScanSnapshot snap = hooks_.snapshot();
        hooks_.emit(events::scanResult(ch, snap.eid, snap.ensemble, snap.services, snap.snr));

        if (mode_ == ScanMode::ToData && !snap.services.empty()) break;
        idx++;
        if (idx >= total) {
            if (mode_ == ScanMode::Single) break;
            idx = 0;
        }
    }
    running_.store(false);
    if (!stop_.exchange(true) && hooks_.finished) hooks_.finished();
}

} // namespace dabcore
