// ctest agc_controller: AgcController als reine Logik ohne Geraet.
// Zeit wird simuliert; der SNR-Pfad bildet die EMA des ofdmHandlers nach
// (0,85/0,15, ~5,2 Werte/s, Meldung jedes dritten Werts, Start bei 10 dB).
#include "agc-controller.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using dabcore::AgcConfig;
using dabcore::AgcController;
using Mode = AgcController::Mode;

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FEHLER %s:%d: %s\n", __FILE__, __LINE__, msg); failures++; } } while (0)

struct Applied { int step; bool amp; };

static AgcConfig hackrfConfig() {
    AgcConfig c;
    c.maxStep = 31; c.acqIncrement = 4; c.hasAmp = true;
    c.ampTrialStep = 20; c.fallbackStep = 20; c.trackStep = 2;
    c.improveDb = 0.25f;
    return c;
}

// Simulierter Empfang: SNR-Profil ueber der Stufe, EMA wie im ofdmHandler.
struct Sim {
    AgcController& agc;
    std::vector<Applied>& log;
    double (*profile)(int step);
    double offset = 0.0;       // Verschiebung des Profils (Umgebung aendert sich)
    int64_t now = 0;
    double ema = 10.0;
    int count = 0;
    Sim(AgcController& a, std::vector<Applied>& l, double (*p)(int)) : agc(a), log(l), profile(p) {}
    // laeuft `ms` Millisekunden mit Sync: SNR-Werte alle 192 ms berechnen,
    // jeden dritten melden
    void run(int64_t ms) {
        int64_t end = now + ms;
        while (now < end) {
            now += 192;
            double truth = profile(agc.step()) + offset;
            ema = 0.85 * ema + 0.15 * truth;
            if (++count >= 3) { count = 0; agc.onSnr(static_cast<float>(ema), now); }
            if (++ficCount >= 5) { ficCount = 0; agc.onFicQuality(50, now); }   // FIC ok ~1/s
        }
    }
    int ficCount = 0;
    void noSignalAfter(int64_t ms) { now += ms; }
};

static double profilePeak46(int step) {
    // Maximum bei VGA 46 (Stufe 23), 0,2 dB je 2 dB VGA
    return 7.2 - 0.2 * std::abs(step * 2 - 46) / 2.0;
}

static void testRampHackRf() {
    std::vector<Applied> log;
    AgcController agc(hackrfConfig(), [&](int s, bool a) { log.push_back({s, a}); });
    int64_t t = 0;
    agc.setStart(12, false, t);          // VGA 24 (Geraete-Default)
    agc.onRetune(t);
    CHECK(log.empty(), "onRetune mit explizitem Start aendert nichts");
    CHECK(agc.mode() == Mode::Acquire, "nach Retune in der Akquisition");

    std::vector<int> expect = {16, 20, 24, 28, 31};    // VGA 32, 40, 48, 56, 62
    for (int e : expect) {
        t += 770;
        CHECK(agc.onNoSignal(t), "Ramp: weiter probieren");
        CHECK(agc.step() == e && !agc.amp(), ("Ramp-Stufe " + std::to_string(e)).c_str());
    }
    t += 770;
    CHECK(agc.onNoSignal(t), "AMP-Versuch: weiter probieren");
    CHECK(agc.step() == 20 && agc.amp(), "AMP an bei VGA 40");
    t += 770;
    CHECK(!agc.onNoSignal(t), "nach dem AMP-Versuch: Ramp erschoepft");
    CHECK(agc.step() == 20 && !agc.amp(), "Rueckfall VGA 40 / AMP aus");
    CHECK(agc.mode() == Mode::Idle, "Ramp beendet");
    size_t n = log.size();
    for (int i = 0; i < 5; i++) { t += 770; CHECK(!agc.onNoSignal(t), "Idle: nichts mehr"); }
    CHECK(log.size() == n, "Idle: keine weiteren Aenderungen (keine Endlosschleife)");
    int ampOn = 0;
    for (auto& a : log) if (a.amp) ampOn++;
    CHECK(ampOn == 1, "AMP genau einmal versucht");
    CHECK(log.size() == 7, "7 Stufenaenderungen in der Ramp");

    // Neue Ramp bei Kanalwechsel: wieder genau ein AMP-Versuch
    log.clear();
    agc.onRetune(t);
    CHECK(agc.mode() == Mode::Acquire, "Retune startet die Ramp neu");
    for (int i = 0; i < 10; i++) { t += 770; agc.onNoSignal(t); }
    ampOn = 0;
    for (auto& a : log) if (a.amp) ampOn++;
    CHECK(ampOn == 1, "nach Retune erneut genau ein AMP-Versuch");
    CHECK(agc.mode() == Mode::Idle && agc.step() == 20 && !agc.amp(), "zweite Ramp endet ebenfalls im Rueckfall");
    std::printf("Ramp HackRF: ok (%zu Aenderungen)\n", log.size());
}

static void testRampRtlSdr() {
    AgcConfig c;
    c.maxStep = 28; c.acqIncrement = 3; c.hasAmp = false; c.fallbackStep = 21; c.ampTrialStep = 21;
    std::vector<Applied> log;
    AgcController agc(c, [&](int s, bool a) { log.push_back({s, a}); });
    int64_t t = 0;
    agc.setStart(21, false, t);
    agc.onRetune(t);
    std::vector<int> expect = {24, 27, 28};
    for (int e : expect) { t += 770; CHECK(agc.onNoSignal(t), "RTL-Ramp weiter"); CHECK(agc.step() == e, "RTL-Ramp-Stufe"); }
    t += 770;
    CHECK(!agc.onNoSignal(t), "RTL-Ramp erschoepft (kein AMP)");
    CHECK(agc.step() == 21, "RTL-Rueckfall auf Default-Index");
    for (auto& a : log) CHECK(!a.amp, "RTL: nie AMP");
    std::printf("Ramp RTL-SDR: ok\n");
}

static void testRampAmpFromUser() {
    // AMP kam per set_gain: erstes no_signal schaltet ihn aus, Ramp laeuft weiter
    std::vector<Applied> log;
    AgcController agc(hackrfConfig(), [&](int s, bool a) { log.push_back({s, a}); });
    agc.setStart(20, true, 0);
    agc.onRetune(0);
    CHECK(agc.amp(), "expliziter Start behaelt AMP");
    CHECK(agc.onNoSignal(770), "AMP aus, weiter");
    CHECK(!agc.amp() && agc.step() == 20, "AMP aus, Stufe bleibt");
    CHECK(agc.onNoSignal(1540) && agc.step() == 24, "Ramp laeuft weiter");
    std::printf("Ramp mit Nutzer-AMP: ok\n");
}

static void testHillClimb() {
    std::vector<Applied> log;
    AgcController agc(hackrfConfig(), [&](int s, bool a) { log.push_back({s, a}); });
    Sim sim(agc, log, profilePeak46);
    agc.setStart(12, false, sim.now);
    agc.onRetune(sim.now);
    sim.noSignalAfter(900);
    agc.onNoSignal(sim.now);                 // -> 16 (VGA 32), dort Sync
    CHECK(agc.step() == 16, "Sync bei VGA 32");
    agc.onSynced(true, sim.now);
    CHECK(agc.mode() == Mode::Track, "nach Sync im Tracking");
    CHECK(agc.lastGoodStep() < 0, "lastGood erst mit dekodierter FIC");
    agc.onFicQuality(50, sim.now);
    CHECK(agc.lastGoodStep() == 16, "lastGood = Sync-Stufe");

    // Konvergenz beobachten: spaetestens nach 60 s im Halten nahe dem Maximum
    int64_t start = sim.now;
    int64_t holdAt = -1;
    while (sim.now - start < 60000) {
        sim.run(577);
        if (agc.mode() == Mode::Hold) { holdAt = sim.now - start; break; }
    }
    CHECK(holdAt >= 0, "Bergsteiger kommt ins Halten");
    CHECK(agc.step() >= 22 && agc.step() <= 24, ("Halten nahe Maximum, Stufe " + std::to_string(agc.step())).c_str());
    CHECK(agc.lastGoodStep() == agc.step(), "lastGood = Haltestufe");
    for (auto& a : log) CHECK(!a.amp, "Tracking: nie AMP");
    // Stufe bei 20 s Laufzeit: schon deutlich ueber dem Startwert
    std::printf("Bergsteiger: Halten nach %.1f s bei VGA %d, %zu Aenderungen\n",
                holdAt / 1000.0, agc.step() * 2, log.size());

    // Halten: 15 s keine Aenderung; danach Basis neu messen, SNR unveraendert
    // -> Hysterese, weiter halten ohne Probe
    size_t n = log.size();
    sim.run(15000 - 577);
    CHECK(log.size() == n, "Halten: keine gain_changed in 15 s");
    sim.run(3000);
    CHECK(log.size() == n && agc.mode() == Mode::Hold, "unveraenderter SNR: weiter halten ohne Probe");

    // Umgebung aendert sich (SNR-Profil +1 dB, Maximum verschiebt sich auf
    // VGA 50): nach dem Halten wird neu sondiert
    sim.profile = [](int step) { return 8.2 - 0.2 * std::abs(step * 2 - 50) / 2.0; };
    int64_t before = sim.now;
    bool probed = false;
    while (sim.now - before < 40000) {
        sim.run(577);
        if (log.size() > n) { probed = true; break; }
    }
    CHECK(probed, "veraenderter SNR: neu sondieren nach dem Halten");
    // ... und konvergiert erneut
    before = sim.now;
    while (sim.now - before < 60000) {
        sim.run(577);
        if (agc.mode() == Mode::Hold && agc.step() >= 24 && agc.step() <= 26) break;
    }
    CHECK(agc.mode() == Mode::Hold && agc.step() >= 24 && agc.step() <= 26,
          ("erneute Konvergenz auf VGA ~50, Stufe " + std::to_string(agc.step())).c_str());
    std::printf("Bergsteiger nach Aenderung: VGA %d\n", agc.step() * 2);
}

static void testHoldReprobeEvery() {
    // Unveraenderter SNR: spaetestens nach holdReprobeEvery Haltephasen eine Probe
    std::vector<Applied> log;
    AgcConfig c = hackrfConfig();
    AgcController agc(c, [&](int s, bool a) { log.push_back({s, a}); });
    Sim sim(agc, log, profilePeak46);
    agc.setStart(23, false, 0);
    agc.onRetune(0);
    agc.onSynced(true, 0);
    while (agc.mode() != Mode::Hold && sim.now < 60000) sim.run(577);
    CHECK(agc.mode() == Mode::Hold, "Halten erreicht");
    size_t n = log.size();
    int64_t t0 = sim.now;
    while (log.size() == n && sim.now - t0 < 120000) sim.run(577);
    int64_t dt = sim.now - t0;
    CHECK(log.size() > n, "irgendwann wird neu sondiert");
    CHECK(dt > 3 * c.holdMs && dt < 5 * c.holdMs + 10000, ("Neu-Sondierung nach " + std::to_string(dt) + " ms").c_str());
    std::printf("Halten/Neu-Sondieren: nach %.1f s\n", dt / 1000.0);
}

static void testFalseSync() {
    // Sync ohne FIBs (11D bei VGA 24): nach ficTimeoutMs je eine Ramp-Stufe,
    // bis FIBs kommen; erst dann Tracking und lastGood
    std::vector<Applied> log;
    AgcController agc(hackrfConfig(), [&](int s, bool a) { log.push_back({s, a}); });
    agc.setStart(12, false, 0);
    agc.onRetune(0);
    // Wartezeit zaehlt ab dem Kanalwechsel (t = 0), nicht ab dem (flatternden) Sync
    int64_t t = 300;
    agc.onSynced(true, t);
    for (t = 800; t <= 2400; t += 800) agc.onFicQuality(0, t);          // 0,8 / 1,6 / 2,4 s: noch nichts
    CHECK(log.empty() && agc.mode() == Mode::Track, "vor Ablauf des FIC-Timeouts keine Aenderung");
    t = 3200; agc.onFicQuality(0, t);                                    // 3,2 s
    CHECK(log.size() == 1 && agc.step() == 16, "Schein-Sync: nach 2,5 s ohne FIBs eine Stufe hoeher");
    CHECK(agc.mode() == Mode::Acquire && agc.lastGoodStep() < 0, "Schein-Sync zaehlt nicht als Erfolg");
    for (int i = 0; i < 4; i++) { t += 800; agc.onFicQuality(0, t); }
    CHECK(log.size() == 2 && agc.step() == 20, "weiter ohne FIBs: naechste Stufe");
    // ein no_signal dazwischen (OFDM verliert den Schein-Sync) laeuft normal weiter
    agc.onSynced(false, t);
    t += 770;
    CHECK(agc.onNoSignal(t) && agc.step() == 24, "no_signal nach Schein-Sync: Ramp ohne Schonfrist");
    agc.onSynced(true, t);
    t += 800;
    agc.onFicQuality(50, t);
    CHECK(agc.mode() == Mode::Track && agc.lastGoodStep() == 24, "FIBs: echter Sync, Tracking, lastGood");
    // Fading bei echtem Empfang (FIC 0 nach vorherigem ok) loest keinen Ramp-Schritt aus
    size_t n = log.size();
    for (int i = 0; i < 6; i++) { t += 800; agc.onFicQuality(0, t); }
    CHECK(log.size() == n && agc.mode() == Mode::Track, "FIC-Einbruch nach echtem Empfang: kein Ramp-Schritt");

    // Flatternder Schein-Sync (Live-Befund 11D/VGA 24: S/L/S/L, kaum no_signal):
    // die FIC-Wartezeit laeuft ueber die Wechsel hinweg, kein no_signal wird
    // durch eine Schonfrist verschluckt
    log.clear();
    agc.setStart(12, false, t);
    agc.onRetune(t);
    int64_t t0 = t;
    for (int i = 0; i < 6 && log.empty(); i++) {
        t += 300; agc.onSynced(true, t);
        t += 400; agc.onFicQuality(0, t);
        t += 200; agc.onSynced(false, t);
    }
    CHECK(log.size() == 1 && agc.step() == 16, "flatternder Schein-Sync: Stufe nach FIC-Timeout");
    CHECK(t - t0 <= 3600, ("Stufe nach " + std::to_string(t - t0) + " ms").c_str());
    t += 200; agc.onSynced(true, t); t += 200; agc.onSynced(false, t);
    t += 500;
    CHECK(agc.onNoSignal(t) && agc.step() == 20, "no_signal nach Schein-Sync-Verlust: keine Schonfrist");

    // Scan-Ende mitten in der Ramp (AMP-Versuch): zurueck auf lastGood/Rueckfall, AMP aus
    for (int i = 0; i < 4; i++) { t += 770; agc.onNoSignal(t); }
    CHECK(agc.amp(), "Ramp steht im AMP-Versuch");
    agc.finishAcquisition(t);
    // lastGood = 12: setStart bei Sync uebernimmt den expliziten Startwert als "zuletzt gut"
    CHECK(!agc.amp() && agc.step() == 12 && agc.mode() == Mode::Idle, "Scan-Ende: lastGood, AMP aus, Ramp beendet");
    std::printf("Schein-Sync: ok\n");
}

static void testFreeze() {
    std::vector<Applied> log;
    AgcController agc(hackrfConfig(), [&](int s, bool a) { log.push_back({s, a}); });
    Sim sim(agc, log, profilePeak46);
    agc.setStart(12, false, 0);
    agc.onRetune(0);
    agc.setEnabled(false, 0);
    CHECK(agc.mode() == Mode::Off, "AGC aus");
    for (int i = 0; i < 5; i++) { sim.noSignalAfter(770); CHECK(!agc.onNoSignal(sim.now), "eingefroren: kein Ramp"); }
    agc.onSynced(true, sim.now);
    sim.run(20000);
    CHECK(log.empty(), "eingefroren: keine Aenderung durch no_signal/snr/synced");
    CHECK(agc.step() == 12, "eingefroren: Stufe bleibt");
    // Wieder an bei Sync: Tracking beginnt
    agc.setEnabled(true, sim.now);
    CHECK(agc.mode() == Mode::Track, "AGC an bei Sync -> Tracking");
    sim.run(20000);
    CHECK(!log.empty() && agc.step() > 12, "nach dem Einschalten steigt der Gain");
    std::printf("Einfrieren: ok\n");
}

static void testRetuneUsesLastGood() {
    std::vector<Applied> log;
    AgcController agc(hackrfConfig(), [&](int s, bool a) { log.push_back({s, a}); });
    Sim sim(agc, log, profilePeak46);
    agc.setStart(12, false, 0);
    agc.onRetune(0);
    sim.noSignalAfter(900); agc.onNoSignal(sim.now);
    sim.noSignalAfter(770); agc.onNoSignal(sim.now);      // Stufe 20 (VGA 40)
    agc.onSynced(true, sim.now);
    sim.run(30000);
    int good = agc.lastGoodStep();
    CHECK(good >= 20, "lastGood nach Tracking");
    // Kanalwechsel auf einen leeren Kanal: Ramp startet bei lastGood
    agc.onRetune(sim.now);
    CHECK(agc.step() == good, "Retune startet bei der zuletzt erfolgreichen Stufe");
    for (int i = 0; i < 10; i++) { sim.noSignalAfter(770); agc.onNoSignal(sim.now); }
    CHECK(agc.mode() == Mode::Idle && agc.step() == 20, "leerer Kanal: Rueckfall");
    // naechster Kanal: wieder bei lastGood (nicht beim Rueckfall, nicht bei 24)
    agc.onRetune(sim.now);
    CHECK(agc.step() == good && !agc.amp(), "naechster Kanal startet wieder bei lastGood");
    // Sync-Verlust waehrend einer Probe: Probe wird zurueckgenommen
    {
        agc.onSynced(true, sim.now);
        sim.run(5000);                       // Basis gemessen, erste Probe (+2) laeuft
        CHECK(agc.step() == good + 2, "erste Probe nach oben");
        agc.onSynced(false, sim.now);
        CHECK(agc.step() == good, "Sync-Verlust in der Probe: zurueck auf die Basis");
        agc.onRetune(sim.now);
    }
    // Sync-Verlust im Tracking (noch vor der ersten Probe, waehrend der
    // Basismessung): erstes no_signal ohne Aenderung, dann Ramp
    agc.onSynced(true, sim.now);
    sim.run(3000);
    int s = agc.step();
    agc.onSynced(false, sim.now);
    CHECK(agc.step() == s, "Sync-Verlust ohne laufende Probe: Stufe bleibt");
    CHECK(agc.mode() == Mode::Acquire, "Sync-Verlust -> Akquisition");
    sim.noSignalAfter(770);
    CHECK(agc.onNoSignal(sim.now) && agc.step() == s, "erstes no_signal nach Sync-Verlust: Schonfrist");
    sim.noSignalAfter(770);
    CHECK(agc.onNoSignal(sim.now) && agc.step() == std::min(s + 4, 31), "danach Ramp");
    // Sync zurueck ohne Kanalwechsel: kurze Einschwingzeit (EMA laeuft weiter),
    // erste Probe nach settle + average (3 s), nicht nach 4,5 s
    {
        size_t n = log.size();
        agc.onSynced(true, sim.now);
        sim.run(2500);
        CHECK(log.size() == n, "vor 3 s keine Probe");
        sim.run(1200);
        CHECK(log.size() == n + 1, "nach Sync-Verlust/-Rueckkehr: Probe nach ~3 s");
    }
    std::printf("Retune/lastGood/Sync-Verlust: ok\n");
}

int main() {
    testRampHackRf();
    testRampRtlSdr();
    testRampAmpFromUser();
    testHillClimb();
    testHoldReprobeEvery();
    testFalseSync();
    testFreeze();
    testRetuneUsesLastGood();
    if (failures) { std::printf("%d Fehler\n", failures); return 1; }
    std::printf("OK\n");
    return 0;
}
