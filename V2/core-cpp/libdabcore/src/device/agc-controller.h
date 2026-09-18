// DAB Classic v3 – Gain-Regelung (AGC) als geraeteneutrale, testbare Logik.
//
// Arbeitet auf einer abstrakten Gain-Stufe 0..maxStep (HackRF: VGA/2, also
// 0..31 fuer VGA 0..62; RTL-SDR: Index in die Tuner-Gain-Tabelle) plus einem
// getrennten AMP-Flag (nur HackRF). Jede Aenderung geht ueber den Callback
// `apply(step, amp)`; der Aufrufer setzt den Geraete-Gain und meldet
// gain_changed. Die Zeit kommt als Parameter (Millisekunden, monoton), damit
// die Logik ohne Geraet und ohne Warten getestet werden kann.
//
// Zwei Betriebsarten (Befund 12.09.2026, HackRF in NRW: Standard-VGA 24
// reicht fuer 11D nicht, AMP uebersteuert, VGA > 48 verschlechtert den SNR):
//
//  Akquisition (kein Sync): bei jedem no_signal (alle ~0,77 s) die Stufe um
//    acqIncrement erhoehen (HackRF VGA +8: 24 -> 32 -> 40 -> 48 -> 56 -> 62);
//    am Maximum einmal den AMP bei ampTrialStep (VGA 40) probieren; danach
//    zurueck auf fallbackStep (VGA 40) / AMP aus und die Ramp beenden
//    (Idle, keine Endlosschleife). Neustart der Ramp bei Kanalwechsel
//    (onRetune), set_gain (setStart) und set_agc (setEnabled).
//    Startpunkt: der aktuell gesetzte Gain; nach einer erschoepften Ramp die
//    zuletzt erfolgreiche Stufe (Scan: nicht jedes Mal von vorn).
//
//  Tracking (Sync): Bergsteiger auf dem SNR. Der ofdmHandler liefert den SNR
//    als EMA (0,85/0,15, ~5 Werte/s); deshalb nach jeder Stufenaenderung
//    settleMs warten, dann averageMs mitteln. Probeschritt +trackStep
//    (HackRF 2 Stufen = VGA 4, weil die EMA einen 2-dB-Schritt unter die
//    Schwelle daempft; RTL-SDR eine Tabellenstufe):
//    Mittelwert um >= improveDb besser -> behalten, weiter in dieselbe
//    Richtung; sonst zurueck, Basis neu messen, andere Richtung. Beide
//    Richtungen ohne Gewinn -> Halten (holdMs); danach Basis neu messen und
//    nur bei veraendertem SNR (oder spaetestens jede 4. Haltephase) neu
//    sondieren (Hysterese gegen dauernde gain_changed). AMP nur in der
//    Akquisition; ein Sync-Verlust fuehrt zurueck in die Akquisition, wobei
//    das erste no_signal (Fading) noch keine Stufe aendert.
//
//  Uebersteuerung (Befund 18.09.2026, aktive Antenne aussen vor dem Fenster,
//  5C bei VGA 40: Sync flattert, SNR < 4 dB, FIC 3 %; bei VGA 20-32 dagegen
//  16 dB): niedriger SNR (< lowSnrDb) auf hoher Stufe (>= highStep) gilt als
//  Verdacht auf zu viel Pegel. Dann probiert der Bergsteiger zuerst nach
//  unten, und nach einem Sync-Verlust in dieser Lage laeuft die Ramp abwaerts
//  (acqIncrement je no_signal bis Stufe 0, dann lastGood/fallbackStep und
//  Idle) statt aufwaerts. Ein schwaches Signal auf niedriger Stufe (Befund
//  11D bei VGA 24) bleibt davon unberuehrt: dort geht die Ramp wie bisher
//  nach oben.
#pragma once

#include <cstdint>
#include <functional>

namespace dabcore {

struct AgcConfig {
    int  maxStep       = 31;     // Stufen 0..maxStep
    int  acqIncrement  = 4;      // Stufen je no_signal in der Akquisition
    bool hasAmp        = false;  // AMP-Versuch am Ende der Ramp (HackRF)
    int  ampTrialStep  = 20;     // Stufe waehrend des AMP-Versuchs
    int  fallbackStep  = 20;     // Stufe nach erschoepfter Ramp
    int  trackStep     = 1;      // Probeschritt im Tracking (Stufen)
    int64_t firstSettleMs = 3500; // Einschwingen nach Sync (EMA startet bei 10 dB)
    int64_t settleMs      = 2000; // Einschwingen nach einer Stufenaenderung
    int64_t averageMs     = 1000; // Mittelungsfenster
    float   improveDb     = 0.25f; // Mindestgewinn fuer einen akzeptierten Schritt
    int64_t holdMs        = 15000;
    int  holdReprobeEvery = 4;    // spaetestens jede n-te Haltephase neu sondieren
    // Schein-Sync: so lange nach synced(true) ohne dekodierte FIBs (ficQuality
    // ok == 0) gilt der Sync als nicht vorhanden -> Ramp-Schritt wie no_signal
    int64_t ficTimeoutMs  = 2500;
    // Uebersteuerungsverdacht auf Stufe >= highStep (HackRF: gainDefaultStep
    // = 20 = VGA 40). Tracking: SNR unter lowSnrDb -> erste Probe nach unten,
    // und zwar um acqIncrement (VGA -8) statt trackStep, weil die Kante der
    // Uebersteuerung steil ist (Befund 5C: VGA 40-48 6 dB, VGA 32 deutlich
    // besser). Kostet bei einem schwachen Signal nur eine Probe (~3 s).
    // Sync-Verlust: erst unter overloadSnrDb laeuft die Ramp abwaerts; die
    // Schwelle liegt unter dem besten 11D-Wert der passiven Antenne (7,2 dB
    // bei VGA 46), damit ein schwaches Signal wie bisher nach oben rampt.
    float lowSnrDb      = 8.0f;
    float overloadSnrDb = 6.0f;
    int   highStep      = 20;
};

class AgcController {
public:
    enum class Mode { Off, Idle, Acquire, Track, Hold };
    using Apply = std::function<void(int step, bool amp)>;

    AgcController(const AgcConfig& cfg, Apply apply);

    // set_agc: aus = einfrieren; an = Ramp bzw. Tracking neu ab aktuellem Gain.
    void setEnabled(bool enabled, int64_t nowMs);
    bool enabled() const { return enabled_; }

    // set_gain (Geraet hat den Wert schon): neuer Ausgangspunkt fuer Ramp
    // bzw. Tracking; der naechste onRetune startet genau hier.
    void setStart(int step, bool amp, int64_t nowMs);

    // Kanalwechsel: Ramp neu. Start = expliziter Startpunkt (setStart), sonst
    // die zuletzt erfolgreiche Stufe, sonst die aktuelle; AMP aus.
    void onRetune(int64_t nowMs);

    // Rueckgabe true: eine neue Stufe wurde gesetzt (oder Schonfrist) und
    // ist zu bewerten – weiter warten. false: nichts mehr zu probieren
    // (Ramp erschoepft, AGC aus).
    bool onNoSignal(int64_t nowMs);

    void onSynced(bool synced, int64_t nowMs);
    void onSnr(float db, int64_t nowMs);
    // FIC-Qualitaet (ok = erfolgreiche FIBs von 50, kommt nur bei Sync etwa
    // 1/s). ok > 0 bestaetigt den Sync (lastGood); Sync ohne FIBs ueber
    // ficTimeoutMs ist ein Schein-Sync bei zu wenig Gain (Befund 11D bei
    // VGA 24: synced, SNR 3 dB, kein Ensemble, nie no_signal).
    void onFicQuality(int ok, int64_t nowMs);
    // Scan-Ende: eine noch laufende Ramp (letzter Kanal leer, Verweilzeit
    // abgelaufen) abbrechen und auf die zuletzt erfolgreiche Stufe (sonst
    // fallbackStep) ohne AMP zurueck; bei echtem Empfang keine Aenderung.
    void finishAcquisition(int64_t nowMs);

    int  step() const { return step_; }
    bool amp() const { return amp_; }
    Mode mode() const { return mode_; }
    bool acquisitionExhausted() const { return mode_ == Mode::Idle; }
    int  lastGoodStep() const { return lastGood_; }   // < 0: noch kein Sync
    bool rampDown() const { return rampDown_; }        // Ramp laeuft abwaerts (Uebersteuerung)
    const AgcConfig& config() const { return cfg_; }

private:
    enum class Phase { MeasureBase, Probe };
    void set(int step, bool amp, int64_t nowMs);
    bool rampStep(int64_t nowMs);      // eine Akquisitionsstufe; false = erschoepft
    void startTracking(int64_t nowMs, int64_t settle);
    void startProbe(int dir, int64_t nowMs);
    void beginMeasure(int64_t nowMs, int64_t settle);
    void evaluate(float mean, int64_t nowMs);
    void hold(int64_t nowMs);
    bool overloadSuspect() const;
    int  probeDirection() const;

    AgcConfig cfg_;
    Apply apply_;
    bool enabled_ = true;
    Mode mode_ = Mode::Idle;
    bool synced_ = false;          // Sync aus Sicht der Regelung (Schein-Sync verworfen)
    bool ofdmSynced_ = false;      // Sync-Zustand des ofdmHandlers
    bool ficOkSeen_ = false;       // seit dem Sync mindestens ein FIB dekodiert
    int64_t ficWait_ = 0;          // Beginn der Wartezeit auf FIBs
    bool emaFresh_ = true;         // SNR-EMA seit dem letzten Kanalwechsel noch nicht eingeschwungen
    int  step_ = 0;
    bool amp_ = false;
    int  lastGood_ = -1;
    bool explicitStart_ = false;
    // Akquisition
    bool ampTried_ = false;
    bool ampTrial_ = false;        // AMP aktuell durch den eigenen Versuch an
    int  graceNoSignals_ = 0;      // no_signals ohne Stufenaenderung nach Sync-Verlust
    bool rampDown_ = false;        // Ramp abwaerts (Uebersteuerungsverdacht beim Sync-Verlust)
    float lastSnr_ = -1.0f;        // letzter SNR-Wert bei Sync (ofdmHandler), < 0: keiner seit Retune
    // Tracking
    Phase phase_ = Phase::MeasureBase;
    int   baseStep_ = 0;
    float baseSnr_ = 0.0f;
    int   dir_ = +1;
    bool  failedUp_ = false, failedDown_ = false;
    bool  bigDown_ = false;        // erste Abwaertsprobe um acqIncrement (Uebersteuerungsverdacht)
    int64_t measStart_ = 0, settle_ = 0;
    double  sum_ = 0.0;
    int     count_ = 0;
    int64_t holdUntil_ = 0;
    int     holdCount_ = 0;
    bool    afterHold_ = false;
};

} // namespace dabcore
