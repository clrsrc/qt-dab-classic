// DAB Classic v3 – Gain-Regelung, siehe agc-controller.h.
#include "agc-controller.h"

#include <algorithm>
#include <cmath>

namespace dabcore {

AgcController::AgcController(const AgcConfig& cfg, Apply apply)
    : cfg_(cfg), apply_(std::move(apply)) {
    step_ = std::clamp(cfg_.fallbackStep, 0, cfg_.maxStep);
    clipCeil_ = cfg_.maxStep + 1;
}

int AgcController::topStep() const {
    return std::min(cfg_.maxStep, clipCeil_);
}

int AgcController::fallbackStep() const {
    return std::min(lastGood_ >= 0 ? lastGood_ : cfg_.fallbackStep, topStep());
}

void AgcController::set(int step, bool amp, int64_t nowMs) {
    step = std::clamp(step, 0, topStep());
    if (!cfg_.hasAmp) amp = false;
    if (step == step_ && amp == amp_) return;
    step_ = step;
    amp_ = amp;
    measStart_ = nowMs;
    lastSetMs_ = nowMs;
    ficWait_ = nowMs;              // FIC-Wartezeit ab der neuen Stufe
    if (apply_) apply_(step_, amp_);
}

// --- Schalter -----------------------------------------------------------------

void AgcController::setEnabled(bool enabled, int64_t nowMs) {
    enabled_ = enabled;
    if (!enabled) { mode_ = Mode::Off; return; }
    ampTried_ = false;
    ampTrial_ = amp_;
    graceNoSignals_ = 0;
    rampDown_ = false;
    exhausted_ = false;
    clipCeil_ = cfg_.maxStep + 1;
    if (synced_) startTracking(nowMs, cfg_.firstSettleMs);
    else mode_ = Mode::Acquire;
}

void AgcController::setStart(int step, bool amp, int64_t nowMs) {
    step_ = std::clamp(step, 0, cfg_.maxStep);
    amp_ = cfg_.hasAmp && amp;
    explicitStart_ = true;
    clipCeil_ = cfg_.maxStep + 1;  // der Nutzer setzt den Gain: Obergrenze neu ermitteln
    lastSetMs_ = nowMs;
    if (!enabled_) return;
    ampTried_ = false;
    ampTrial_ = false;
    graceNoSignals_ = 0;
    rampDown_ = false;
    exhausted_ = false;
    if (synced_) { lastGood_ = step_; startTracking(nowMs, cfg_.settleMs); }
    else mode_ = Mode::Acquire;
}

void AgcController::onRetune(int64_t nowMs) {
    synced_ = false;
    ofdmSynced_ = false;
    ficOkSeen_ = false;
    ficWait_ = nowMs;
    lastGoodFicMs_ = -1;
    emaFresh_ = true;
    ampTried_ = false;
    ampTrial_ = false;
    graceNoSignals_ = 0;
    rampDown_ = false;
    exhausted_ = false;
    lastSnr_ = -1.0f;
    clipCeil_ = cfg_.maxStep + 1;  // Obergrenze gilt je Kanal
    lastSetMs_ = nowMs;            // Umschalten: Samples des alten Kanals nicht bewerten
    if (!enabled_) { explicitStart_ = false; mode_ = Mode::Off; return; }
    mode_ = Mode::Acquire;
    if (explicitStart_) {
        explicitStart_ = false;
        // der Geraete-Gain ist der gewuenschte Startpunkt; nichts anwenden
    } else if (lastGood_ >= 0) {
        set(lastGood_, false, nowMs);
    } else {
        set(step_, false, nowMs);
    }
}

// --- ADC-Obergrenze ---------------------------------------------------------------

void AgcController::onAdcClip(float ratio, int64_t nowMs) {
    if (!enabled_ || mode_ == Mode::Off) return;
    if (ratio <= cfg_.clipLimit) return;
    if (nowMs - lastSetMs_ < cfg_.clipSettleMs) return;   // noch Samples der alten Stufe
    if (amp_) {
        // erst der AMP (14 dB) weg. Der AMP-Versuch am Ende der Ramp gilt
        // damit als erfolglos (Rueckfall, Idle); ein AMP vom Nutzer geht
        // nur aus, die Regelung laeuft auf der Stufe weiter.
        ampTried_ = true;
        const bool trial = ampTrial_;
        ampTrial_ = false;
        if (trial && mode_ == Mode::Acquire) {
            set(std::min(cfg_.fallbackStep, topStep()), false, nowMs);
            mode_ = Mode::Idle;
            exhausted_ = true;
        } else {
            set(step_, false, nowMs);
        }
        return;
    }
    int target = std::max(step_ - cfg_.clipStep, 0);
    clipCeil_ = target;
    rampDown_ = false;
    lastSetMs_ = nowMs;
    if (target == step_) return;           // Stufe 0: nichts mehr zu senken
    set(target, false, nowMs);
    if (mode_ == Mode::Track || mode_ == Mode::Hold) {
        // Bergsteiger: neue Basis unter der Obergrenze messen, nach oben ist zu
        mode_ = Mode::Track;
        phase_ = Phase::MeasureBase;
        baseStep_ = step_;
        failedUp_ = true;
        failedDown_ = false;
        afterHold_ = false;
        holdCount_ = 0;
        beginMeasure(nowMs, cfg_.settleMs);
    }
    // Acquire: die Ramp endet beim naechsten no_signal an der Obergrenze;
    // Idle (Ramp erschoepft) bleibt Idle auf der gesenkten Stufe
}

// --- Akquisition ------------------------------------------------------------------

bool AgcController::onNoSignal(int64_t nowMs) {
    if (!enabled_ || mode_ == Mode::Off) return false;
    if (mode_ == Mode::Track || mode_ == Mode::Hold) {
        // Sync-Verlust ohne synced(false) – wie onSynced(false)
        onSynced(false, nowMs);
    }
    if (mode_ == Mode::Idle) return false;
    if (graceNoSignals_ > 0) { graceNoSignals_--; return true; }
    return rampStep(nowMs);
}

bool AgcController::rampStep(int64_t nowMs) {
    if (amp_) {
        ampTried_ = true;
        if (ampTrial_) {
            // AMP-Versuch erfolglos: zurueck und Ramp beenden
            ampTrial_ = false;
            set(std::min(cfg_.fallbackStep, topStep()), false, nowMs);
            mode_ = Mode::Idle;
            exhausted_ = true;
            return false;
        }
        // AMP kam von aussen (set_gain): aus und mit der Ramp weiter
        set(step_, false, nowMs);
        return true;
    }
    if (rampDown_) {
        // Uebersteuerungsverdacht: abwaerts bis Stufe 0, dann Rueckfall
        if (step_ > 0) {
            set(std::max(step_ - cfg_.acqIncrement, 0), false, nowMs);
            return true;
        }
        rampDown_ = false;
        set(fallbackStep(), false, nowMs);
        mode_ = Mode::Idle;
        exhausted_ = true;
        return false;
    }
    const int top = topStep();
    if (step_ < top) {
        set(std::min(step_ + cfg_.acqIncrement, top), false, nowMs);
        return true;
    }
    // AMP (+14 dB) nur ohne ADC-Obergrenze: darueber ist der ADC schon voll
    if (cfg_.hasAmp && !ampTried_ && !clipLimited()) {
        ampTried_ = true;
        ampTrial_ = true;
        set(cfg_.ampTrialStep, true, nowMs);
        return true;
    }
    set(std::min(cfg_.fallbackStep, top), false, nowMs);
    mode_ = Mode::Idle;
    exhausted_ = true;
    return false;
}

void AgcController::onSynced(bool synced, int64_t nowMs) {
    ofdmSynced_ = synced;
    if (synced == synced_) return;
    synced_ = synced;
    if (!enabled_ || mode_ == Mode::Off) return;
    if (synced) {
        // Die FIC-Wartezeit laeuft ueber flatternde Schein-Syncs hinweg
        // weiter; nur nach echtem Empfang beginnt sie neu.
        if (ficOkSeen_) { ficOkSeen_ = false; ficWait_ = nowMs; }
        ampTrial_ = false;           // ein erfolgreicher AMP-Versuch bleibt
        // Die SNR-EMA des ofdmHandlers startet nur nach einem Neustart
        // (Kanalwechsel) bei 10 dB; nach einem blossen Sync-Verlust laeuft
        // sie weiter, dann reicht die normale Einschwingzeit.
        startTracking(nowMs, emaFresh_ ? cfg_.firstSettleMs : cfg_.settleMs);
        emaFresh_ = false;
        return;
    }
    // Sync verloren: laufende Probe zuruecknehmen, zurueck in die Akquisition;
    // nach echtem Empfang aendert das erste no_signal noch nichts (kurzes
    // Fading) – nach einem Schein-Sync ohne FIBs gibt es keine Schonfrist,
    // sonst wuerde ein flatternder Schein-Sync die Ramp dauerhaft blockieren.
    if ((mode_ == Mode::Track || mode_ == Mode::Hold) && phase_ == Phase::Probe)
        set(baseStep_, amp_, nowMs);
    mode_ = Mode::Acquire;
    graceNoSignals_ = (lastGoodFicMs_ >= 0 && nowMs - lastGoodFicMs_ <= cfg_.graceAfterGoodMs) ? 1 : 0;
    // Sync verloren bei niedrigem SNR auf hoher Stufe: eher zu viel als zu
    // wenig Pegel -> die Ramp laeuft abwaerts
    rampDown_ = overloadSuspect();
    // Ramp schon erschoepft und nie FIBs: der Schein-Sync startet sie nicht neu
    if (exhausted_ && lastGoodFicMs_ < 0) mode_ = Mode::Idle;
}

bool AgcController::overloadSuspect() const {
    return step_ >= cfg_.highStep && lastSnr_ >= 0.0f && lastSnr_ < cfg_.overloadSnrDb;
}

// Erste Proberichtung des Bergsteigers: bei Uebersteuerungsverdacht nach unten
int AgcController::probeDirection() const {
    return (step_ >= cfg_.highStep && baseSnr_ < cfg_.lowSnrDb) ? -1 : +1;
}

void AgcController::finishAcquisition(int64_t nowMs) {
    if (!enabled_ || mode_ == Mode::Off) return;
    if (synced_ && ficOkSeen_) return;      // echter Empfang: nichts zu tun
    set(fallbackStep(), false, nowMs);
    ampTrial_ = false;
    mode_ = Mode::Idle;
}

void AgcController::onFicQuality(int ok, int64_t nowMs) {
    if (!enabled_ || mode_ == Mode::Off || !ofdmSynced_) return;
    if (ok > 0) {
        lastGoodFicMs_ = nowMs;
        exhausted_ = false;
        if (!ficOkSeen_) { ficOkSeen_ = true; lastGood_ = step_; }
        if (!synced_) {
            // als Schein-Sync verworfen, liefert jetzt aber FIBs: echter Sync
            synced_ = true;
            startTracking(nowMs, cfg_.settleMs);
        }
        return;
    }
    if (ficOkSeen_) return;                    // Fading bei echtem Empfang
    if (nowMs - ficWait_ < cfg_.ficTimeoutMs) return;
    // Schein-Sync: seit ficTimeoutMs keine FIBs -> wie no_signal eine Stufe
    // weiter (bei Uebersteuerungsverdacht abwaerts, sonst hoeher)
    ficWait_ = nowMs;
    if (synced_) {
        synced_ = false;
        // nach erschoepfter Ramp ohne je FIBs: Idle statt Neustart der Ramp
        mode_ = (exhausted_ && lastGoodFicMs_ < 0) ? Mode::Idle : Mode::Acquire;
        graceNoSignals_ = 0;
        rampDown_ = overloadSuspect();
    }
    if (mode_ == Mode::Acquire) rampStep(nowMs);
}

// --- Tracking ---------------------------------------------------------------------

void AgcController::startTracking(int64_t nowMs, int64_t settle) {
    mode_ = Mode::Track;
    baseStep_ = step_;
    dir_ = +1;
    failedUp_ = failedDown_ = false;
    holdCount_ = 0;
    afterHold_ = false;
    phase_ = Phase::MeasureBase;
    beginMeasure(nowMs, settle);
}

void AgcController::beginMeasure(int64_t nowMs, int64_t settle) {
    measStart_ = nowMs;
    settle_ = settle;
    sum_ = 0.0;
    count_ = 0;
}

void AgcController::onSnr(float db, int64_t nowMs) {
    // Der SNR zaehlt fuer den Uebersteuerungsverdacht nur bei echtem Empfang
    // (FIBs gesehen). Bei einem flatternden Schein-Sync (Befund 12C/11C,
    // 18.09.2026) liefert der ofdmHandler 2-6 dB auf Rauschen; damit pendelte
    // die Ramp endlos zwischen VGA 40 und 32 und kam nie zu VGA 48-62/AMP.
    if (ofdmSynced_ && ficOkSeen_) lastSnr_ = db;
    if (!enabled_) return;
    if (mode_ == Mode::Hold) {
        if (nowMs < holdUntil_) return;
        mode_ = Mode::Track;
        phase_ = Phase::MeasureBase;
        afterHold_ = true;
        beginMeasure(nowMs, 0);
    }
    if (mode_ != Mode::Track) return;
    if (nowMs - measStart_ < settle_) return;
    sum_ += db;
    count_++;
    if (nowMs - measStart_ < settle_ + cfg_.averageMs) return;
    evaluate(static_cast<float>(sum_ / count_), nowMs);
}

void AgcController::evaluate(float mean, int64_t nowMs) {
    if (phase_ == Phase::MeasureBase) {
        if (afterHold_) {
            afterHold_ = false;
            bool changed = std::fabs(mean - baseSnr_) >= cfg_.improveDb;
            baseSnr_ = mean;
            if (!changed && holdCount_ < cfg_.holdReprobeEvery) { hold(nowMs); return; }
            holdCount_ = 0;
            failedUp_ = failedDown_ = false;
        } else {
            baseSnr_ = mean;
        }
        baseStep_ = step_;
        dir_ = probeDirection();
        bigDown_ = dir_ < 0;
        startProbe(dir_, nowMs);
        return;
    }
    // Probe bewerten
    bigDown_ = false;
    if (mean >= baseSnr_ + cfg_.improveDb) {
        baseStep_ = step_;
        baseSnr_ = mean;
        lastGood_ = step_;
        failedUp_ = failedDown_ = false;
        startProbe(dir_, nowMs);
        return;
    }
    (dir_ > 0 ? failedUp_ : failedDown_) = true;
    set(baseStep_, amp_, nowMs);
    if (failedUp_ && failedDown_) { hold(nowMs); return; }
    dir_ = -dir_;
    phase_ = Phase::MeasureBase;
    beginMeasure(nowMs, cfg_.settleMs);
}

void AgcController::startProbe(int dir, int64_t nowMs) {
    for (int i = 0; i < 2; i++) {
        bool& failed = dir > 0 ? failedUp_ : failedDown_;
        int size = (dir < 0 && bigDown_) ? std::max(cfg_.trackStep, cfg_.acqIncrement) : cfg_.trackStep;
        int target = baseStep_ + dir * size;
        if (!failed && target >= 0 && target <= topStep()) {
            dir_ = dir;
            set(target, amp_, nowMs);
            phase_ = Phase::Probe;
            beginMeasure(nowMs, cfg_.settleMs);
            return;
        }
        failed = true;            // Grenze erreicht oder schon erfolglos
        dir = -dir;
    }
    hold(nowMs);
}

void AgcController::hold(int64_t nowMs) {
    mode_ = Mode::Hold;
    holdUntil_ = nowMs + cfg_.holdMs;
    holdCount_++;
    phase_ = Phase::MeasureBase;
}

} // namespace dabcore
