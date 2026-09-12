// DAB Classic v3 – Timeshift-Ring des Primary-Dienstes (Entscheidung 4,
// Plan M4 1.1/1.2).
//
// Gespeichert werden die **Hardbits** eines logischen DAB-Rahmens (24 ms),
// also genau das, was Backend::processSegment an backendDriver::addtoFrame
// uebergibt: 24 * bitRate Byte mit je einem Bit. Im Ring liegen sie gepackt
// (bitRate * 3 Byte je Rahmen), damit eine Stunde Dlf (104 kbit/s) ~46,8 MB
// braucht statt der achtfachen Menge.
//
// Diese Klasse ist reiner Speicher: kein Thread, keine Kern- oder
// Protokollabhaengigkeit (ctest `timeshift_buffer`). Alle Methoden sind
// thread-sicher (ein Schreiber aus dem Backend-Thread, ein Leser aus dem
// Takt-Thread, Bedienung aus dem Kommandothread).
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace dabcore {

enum class TimeshiftMode { Live, Paused, Playing };
const char* timeshiftModeName(TimeshiftMode m);

class TimeshiftBuffer {
public:
    // Dauer eines logischen Rahmens (DAB Mode I: 24 ms)
    static constexpr double kFrameSeconds = 0.024;

    // Kapazitaet in Sekunden. Aenderung legt den Ring neu an (leert ihn);
    // gibt true zurueck, wenn sich die Kapazitaet geaendert hat.
    bool setCapacitySeconds(uint32_t capacityS);
    // Rahmengroesse des gerade laufenden Dienstes in Hardbits (24 * bitRate);
    // 0 = kein Dienst. Setzt den Ring in jedem Fall zurueck.
    void setFrameBits(uint32_t frameBits);
    void clear();

    bool active() const;                 // Rahmengroesse und Kapazitaet gesetzt
    uint32_t frameBits() const;
    uint32_t packedBytes() const;        // Bytes je Rahmen im Ring
    uint32_t capacityFrames() const;
    uint32_t capacitySeconds() const;
    uint64_t capacityBytes() const;      // belegter Speicher des Rings
    uint64_t size() const;               // gefuellte Rahmen
    uint64_t writeIndex() const;         // monoton seit clear()
    uint64_t readIndex() const;
    int64_t liveUnix() const;            // Zeitstempel des letzten Rahmens (0 = unbekannt)
    double bufferedSeconds() const;
    double offsetSeconds() const;        // Abstand Lesezeiger -> Schreibzeiger

    TimeshiftMode mode() const;
    void setMode(TimeshiftMode m);

    // Rahmen anhaengen (Backend-Thread). `n` muss frameBits() sein, sonst
    // wird der Rahmen verworfen. Laeuft der Ring ueber, faellt der aelteste
    // Rahmen weg; steht der Lesezeiger dort, rueckt er mit.
    void push(const uint8_t* hardBits, uint32_t n, int64_t unixUtc);
    // Rahmen am Lesezeiger entpacken und den Lesezeiger vorruecken.
    // false, wenn der Lesezeiger den Schreibzeiger erreicht hat.
    bool pop(std::vector<uint8_t>& hardBits, int64_t* unixUtc = nullptr);

    // Lesezeiger auf "offsetS Sekunden hinter live" (0 = live); geklemmt auf
    // 0..bufferedSeconds(). Gibt den tatsaechlichen Versatz zurueck.
    double seekSeconds(double offsetS);
    // Relativ: +delta geht Richtung live, -delta zurueck.
    double skipSeconds(double deltaS);
    void toLive();

    // Rahmen im Bereich [fromS .. toS) hinter live gepackt herauskopieren
    // (Export, Aufnahme-Vorlauf). fromS > toS; Rueckgabe = Anzahl Rahmen.
    uint64_t copyRange(double fromS, double toS, std::vector<uint8_t>& packed,
                       int64_t* firstUnix = nullptr) const;

    // Einen gepackten Rahmen wieder auf ein Bit je Byte bringen.
    static void unpack(const uint8_t* packed, uint32_t frameBits, uint8_t* hardBits);

private:
    // Aelteste noch vorhandene Rahmennummer (ohne Sperre)
    uint64_t oldestLocked() const;
    void reallocLocked();

    mutable std::mutex m_;
    std::vector<uint8_t> data_;     // capacityFrames_ * packedBytes_
    std::vector<int64_t> stamps_;   // Ensemble-Uhrzeit je Rahmen (0 = unbekannt)
    uint32_t capacityS_ = 0;
    uint32_t capacityFrames_ = 0;
    uint32_t frameBits_ = 0;
    uint32_t packedBytes_ = 0;
    uint64_t writeIndex_ = 0;
    uint64_t readIndex_ = 0;
    int64_t liveUnix_ = 0;
    TimeshiftMode mode_ = TimeshiftMode::Live;
};

} // namespace dabcore
