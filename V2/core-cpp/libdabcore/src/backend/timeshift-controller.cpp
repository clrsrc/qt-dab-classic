// DAB Classic v3 – Steuerung des Timeshift-Rings, siehe timeshift-controller.h.
#include "timeshift-controller.h"

#include <algorithm>
#include <cstdio>

namespace dabcore {

using namespace std::chrono_literals;

// Ist der Lesezeiger hoechstens zwei Rahmen hinter dem Schreibzeiger, ist er
// an live angekommen (der Ring pendelt dann zwischen 0 und 1 Rahmen, weil
// Quelle und Takt-Thread mit derselben Rate laufen). Erst nach ~200 ms in
// diesem Zustand wird zurueckgeschaltet, damit ein einzelner Aussetzer der
// Quelle den Zeitversatz nicht verwirft.
static constexpr int kTicksToLive = 8;
static constexpr double kLiveSlackSeconds = 2 * TimeshiftBuffer::kFrameSeconds;

TimeshiftController::TimeshiftController() {
    buf_.setCapacitySeconds(3600);   // Entscheidung 4: Standard 60 min
}

TimeshiftController::~TimeshiftController() {
    stopThread();
}

void TimeshiftController::setNotify(Notify n) { std::lock_guard<std::mutex> lk(cbM_); notify_ = std::move(n); }
void TimeshiftController::setLogger(Logger l) { std::lock_guard<std::mutex> lk(cbM_); logger_ = std::move(l); }
void TimeshiftController::setFlush(Action f) { std::lock_guard<std::mutex> lk(cbM_); flush_ = std::move(f); }
void TimeshiftController::setStarvedHandler(std::function<void(bool)> h) {
    std::lock_guard<std::mutex> lk(cbM_); starved_ = std::move(h);
}
void TimeshiftController::setClock(Clock c) { std::lock_guard<std::mutex> lk(cbM_); clock_ = std::move(c); }

void TimeshiftController::log(const char* level, const std::string& text) const {
    Logger l;
    { std::lock_guard<std::mutex> lk(cbM_); l = logger_; }
    if (l) l(level, text);
}

void TimeshiftController::publish() {
    Notify n;
    { std::lock_guard<std::mutex> lk(cbM_); n = notify_; }
    if (n) n(snapshot());
}

// Der Sink zaehlt keine Underruns, solange der Ring bewusst nichts liefert.
void TimeshiftController::updateStarved() {
    std::function<void(bool)> h;
    { std::lock_guard<std::mutex> lk(cbM_); h = starved_; }
    if (h) h(buf_.mode() != TimeshiftMode::Live);
}

void TimeshiftController::setMode(TimeshiftMode m) {
    if (buf_.mode() == m) return;
    buf_.setMode(m);
    updateStarved();
    publish();
}

// "46.8 MB" statt ganzzahliger Division
static std::string megabytes(uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1000.0 * 1000.0));
    return buf;
}

void TimeshiftController::configure(uint32_t capacityS) {
    const uint32_t cap = std::clamp<uint32_t>(capacityS, 60, 14400);
    if (cap != capacityS)
        log("warn", "timeshift_configure: capacity_s " + std::to_string(capacityS) +
                    " ausserhalb 60..14400, auf " + std::to_string(cap) + " geklemmt");
    if (!buf_.setCapacitySeconds(cap)) {
        log("debug", "timeshift_configure: Kapazitaet unveraendert (" + std::to_string(cap) + " s)");
        publish();
        return;
    }
    // Der Speicher steht erst, wenn die Rahmengroesse des Dienstes bekannt
    // ist; ohne Dienst wird der Bedarf mit 104 kbit/s (Dlf) geschaetzt.
    const uint32_t frames = buf_.capacityFrames();
    const uint32_t bytes = buf_.packedBytes();
    if (bytes > 0) {
        log("info", "Timeshift-Ring: " + std::to_string(cap) + " s = " + std::to_string(frames) +
                    " Rahmen x " + std::to_string(bytes) + " Byte = " + megabytes(buf_.capacityBytes()));
    } else {
        log("info", "Timeshift-Ring: " + std::to_string(cap) + " s = " + std::to_string(frames) +
                    " Rahmen (Rahmengroesse erst mit dem Dienst bekannt, 104 kbit/s ~ " +
                    megabytes(static_cast<uint64_t>(frames) * 312) + ")");
    }
    publish();
}

void TimeshiftController::attach(uint32_t frameBits, FrameSink sink) {
    detach();
    {
        std::lock_guard<std::mutex> lk(sinkM_);
        sink_ = std::move(sink);
    }
    buf_.setFrameBits(frameBits);   // legt den Ring neu an (leer, live)
    attached_.store(true);
    log("info", "Timeshift: Ring fuer den Primary-Dienst, " + std::to_string(buf_.capacitySeconds()) +
                " s = " + std::to_string(buf_.capacityFrames()) + " Rahmen x " +
                std::to_string(buf_.packedBytes()) + " Byte = " + megabytes(buf_.capacityBytes()));
    startThread();
    updateStarved();
    publish();
}

void TimeshiftController::detach() {
    if (!attached_.exchange(false)) {
        stopThread();
        return;
    }
    stopThread();
    {
        std::lock_guard<std::mutex> lk(sinkM_);
        sink_ = nullptr;
    }
    buf_.setFrameBits(0);
    updateStarved();
    publish();
}

void TimeshiftController::startThread() {
    if (threadRun_.load()) return;
    threadRun_.store(true);
    thread_ = std::thread([this] { run(); });
}

void TimeshiftController::stopThread() {
    threadRun_.store(false);
    if (thread_.joinable()) thread_.join();
}

// Aus dem Backend-Thread: Rahmen immer anhaengen; live zusaetzlich sofort
// an den Driver durchreichen (kein Umweg ueber den Ring, kein Jitter).
void TimeshiftController::onBackendFrame(const std::vector<uint8_t>& hardBits) {
    int64_t unix = 0;
    {
        std::lock_guard<std::mutex> lk(cbM_);
        if (clock_) unix = clock_();
    }
    buf_.push(hardBits.data(), static_cast<uint32_t>(hardBits.size()), unix);
    if (buf_.mode() != TimeshiftMode::Live) return;
    std::lock_guard<std::mutex> lk(sinkM_);
    if (sink_) sink_(hardBits);
}

void TimeshiftController::pause() {
    if (!attached_.load()) { log("warn", "timeshift_pause ohne Primary-Dienst"); return; }
    if (buf_.mode() == TimeshiftMode::Live) buf_.seekSeconds(0.0);   // Lesezeiger = Schreibzeiger
    setMode(TimeshiftMode::Paused);
    log("info", "Timeshift: pause bei " + std::to_string(buf_.offsetSeconds()) + " s hinter live");
}

void TimeshiftController::play() {
    if (!attached_.load()) { log("warn", "timeshift_play ohne Primary-Dienst"); return; }
    if (buf_.mode() == TimeshiftMode::Live) return;
    setMode(TimeshiftMode::Playing);
}

void TimeshiftController::live() {
    if (!attached_.load()) { log("warn", "timeshift_live ohne Primary-Dienst"); return; }
    const bool wasShifted = buf_.mode() != TimeshiftMode::Live || buf_.offsetSeconds() > 0.0;
    buf_.toLive();
    updateStarved();
    if (wasShifted) {
        // Ohne Verwerfen laeuft der Ausgabepuffer (0,7 s) noch mit altem Ton
        Action f;
        { std::lock_guard<std::mutex> lk(cbM_); f = flush_; }
        if (f) f();
    }
    publish();
}

// seek/skip lassen den Zustand stehen (paused bleibt paused); aus `live`
// heraus wird aber gespielt – sonst zoege der Schreibzeiger den Lesezeiger
// sofort wieder mit und der Sprung waere wirkungslos.
double TimeshiftController::seek(double offsetS) {
    if (!attached_.load()) { log("warn", "timeshift_seek ohne Primary-Dienst"); return 0.0; }
    const double off = buf_.seekSeconds(offsetS);
    if (off > 0.0 && buf_.mode() == TimeshiftMode::Live) { setMode(TimeshiftMode::Playing); return off; }
    publish();
    return off;
}

double TimeshiftController::skip(double deltaS) {
    if (!attached_.load()) { log("warn", "timeshift_skip ohne Primary-Dienst"); return 0.0; }
    const double off = buf_.skipSeconds(deltaS);
    // Ueber live hinaus (+) heisst live
    if (off <= 0.0 && buf_.mode() != TimeshiftMode::Live) {
        live();
        return 0.0;
    }
    if (off > 0.0 && buf_.mode() == TimeshiftMode::Live) { setMode(TimeshiftMode::Playing); return off; }
    publish();
    return off;
}

void TimeshiftController::dropToLive(const char* reason) {
    const bool wasShifted = attached_.load() &&
                            (buf_.mode() != TimeshiftMode::Live || buf_.offsetSeconds() > 0.0);
    buf_.clear();
    updateStarved();
    if (wasShifted) {
        Action f;
        { std::lock_guard<std::mutex> lk(cbM_); f = flush_; }
        if (f) f();
        log("info", std::string("Timeshift: Zeitversatz verworfen (") + reason + ")");
    }
    publish();
}

TimeshiftSnapshot TimeshiftController::snapshot() const {
    TimeshiftSnapshot s;
    s.mode = buf_.mode();
    s.bufferedS = buf_.bufferedSeconds();
    s.offsetS = buf_.offsetSeconds();
    s.capacityS = static_cast<double>(buf_.capacitySeconds());
    s.frameIndex = buf_.writeIndex();
    s.liveUnix = buf_.liveUnix();
    s.attached = attached_.load();
    return s;
}

// Takt-Thread: 24 ms je Rahmen. `next` laeuft absolut mit, deshalb gibt es
// keine Drift gegen steady_clock; ein grosser Rueckstand (Blockade im Sink)
// setzt den Takt neu auf.
void TimeshiftController::run() {
    auto next = std::chrono::steady_clock::now();
    auto lastPublish = next;
    std::vector<uint8_t> frame;
    int atLiveTicks = 0;
    while (threadRun_.load()) {
        next += std::chrono::microseconds(24000);
        auto now = std::chrono::steady_clock::now();
        if (next + 240ms < now) next = now;
        std::this_thread::sleep_until(next);
        if (!threadRun_.load()) break;
        if (buf_.mode() == TimeshiftMode::Playing) {
            if (buf_.pop(frame)) {
                std::lock_guard<std::mutex> lk(sinkM_);
                if (sink_) sink_(frame);
            }
            if (buf_.offsetSeconds() <= kLiveSlackSeconds) {
                if (++atLiveTicks >= kTicksToLive) {
                    atLiveTicks = 0;
                    // Lesezeiger hat den Schreibzeiger eingeholt: im selben
                    // Takt weiter, nur die Quelle wechselt auf Durchreichen.
                    buf_.toLive();
                    updateStarved();
                    publish();
                    log("info", "Timeshift: am Schreibzeiger angekommen, wieder live");
                }
            } else {
                atLiveTicks = 0;
            }
        } else {
            atLiveTicks = 0;
        }
        now = std::chrono::steady_clock::now();
        if (now - lastPublish >= 500ms) {   // timeshift_state mit 2 Hz
            lastPublish = now;
            publish();
        }
    }
}

} // namespace dabcore
