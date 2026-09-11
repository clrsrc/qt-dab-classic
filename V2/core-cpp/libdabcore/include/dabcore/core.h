// DAB Classic – Kern-Fassade.
//
// DabCore nimmt Kommandos (JSON, siehe dab-api Command) entgegen und liefert
// Ereignisse ueber eine EventSink. Intern besitzt sie Quelle, OFDM-Thread,
// Backends, Audio und Timeshift. In M0 (Stand: Gerüst + IPC-Spike) ist nur
// die Kommandoschleife und ein synthetischer Ereignisgenerator vorhanden;
// der portierte Empfangspfad wird schrittweise angeschlossen.
#pragma once

#include "dabcore/events.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace dabcore {

struct CoreOptions {
    bool audio = true;          // PortAudio-Ausgabe (Entscheidung 3)
    std::string audioDevice;    // leer = Standardgeraet
};

class DabCore {
public:
    DabCore(EventSink sink, CoreOptions options);
    ~DabCore();
    DabCore(const DabCore&) = delete;
    DabCore& operator=(const DabCore&) = delete;

    // Verarbeitet ein Kommando; wird vom Kommandothread aufgerufen.
    // Gibt false zurueck, wenn der Kern beendet werden soll (shutdown).
    bool handle(const json& command);

    // Vollstaendigen Zustand als state_snapshot-Ereignis senden.
    void emitState();

    static std::string version();

private:
    void openDevice(const json& source);
    void closeDevice();
    void startSpike();   // Spike 1: synthetische Ereignisse (Quelle "spike")
    void stopSpike();

    EventSink sink_;
    CoreOptions opt_;
    json state_;
    std::atomic<bool> spikeRunning_{false};
    std::atomic<bool> spectrumOn_{false};
    std::thread spikeThread_;
};

} // namespace dabcore
