// DAB Classic v3 – Band-Scan (Ersatz fuer den Scan-Ablauf aus Qt-DAB
// main/radio.cpp: startScanning / nextFor_scan_* / channel_timeOut /
// no_signal_found und support/scan-handler.cpp; Jan van Katwijk, GPLv2+).
//
// Ablauf je Kanal wie v1: Kanal einstellen (OFDM im Scan-Modus, nur FIC),
// dann bis zum Ablauf der Verweilzeit (v1 switchDelay, Default 6 s; im
// Dauerlauf 2 * switchDelay) oder bis "kein Signal" warten. Beim ersten
// "kein Signal" wird einmal der AMP umgeschaltet und derselbe Kanal weiter
// beobachtet (radio.cpp Z. 786-798: starke Signale wie 5C uebersteuern mit
// AMP); bringt das keine Dienste, wird der AMP-Zustand zurueckgesetzt.
// Modi: single (alle Kanaele einmal, dann scan_finished, Kern bleibt auf
// dem letzten Kanal), to_data (bis zum ersten Ensemble mit Diensten, dort
// bleiben), continuous (endlos bis stop_scan).
#pragma once

#include "dabcore/events.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dabcore {

enum class ScanMode { Single, ToData, Continuous };
ScanMode scanModeFromName(const std::string& s);

struct ScanSnapshot {
    int eid = -1;                    // < 0: kein Ensemble
    std::string ensemble;
    std::vector<ServiceInfo> services;
    float snr = 0;
};

// Verbindung zum Kern; alle Aufrufe kommen aus dem Scan-Thread.
struct ScanHooks {
    std::function<bool(const std::string& channel)> tune;   // Kanal einstellen (Scan-Modus)
    std::function<ScanSnapshot()> snapshot;                 // aktueller FIC-Stand
    std::function<bool()> ampGet;                           // AMP-Zustand (HackRF)
    std::function<bool(bool)> ampSet;                       // false = Geraet ohne AMP
    std::function<void(json)> emit;                         // Ereignisse
    std::function<void()> finished;                         // Scan-Modus verlassen
};

class ScanController {
public:
    ScanController(ScanHooks hooks, std::vector<std::string> channels, ScanMode mode, int dwellMs);
    ~ScanController();

    void start();
    // Bricht den Scan ab und wartet auf das Thread-Ende (blockiert kurz).
    // true, wenn ein laufender Scan abgebrochen wurde (der Aufrufer meldet
    // dann scan_finished); false, wenn er schon von selbst geendet hatte.
    bool stop();
    bool running() const { return running_.load(); }
    ScanMode mode() const { return mode_; }

    // Aus dem OFDM-Thread: nach `attempts` Sync-Versuchen kein Signal.
    void onNoSignal();

private:
    void run();

    ScanHooks hooks_;
    std::vector<std::string> channels_;
    ScanMode mode_;
    int dwellMs_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::mutex m_;
    std::condition_variable cv_;
    bool noSignal_ = false;
};

} // namespace dabcore
