// DAB Classic v3 – Schnittstelle einer Sample-Quelle (Ersatz fuer Qt-DAB
// devices/device-handler.h: kein QThread, kein Widget). Geraete (HackRF,
// RTL-SDR) und Dateileser implementieren dieses Interface; der OFDM-Thread
// zieht Samples ueber getSamples()/waitForSamples().
#pragma once

#include <complex>
#include <cstdint>
#include <string>

class ISampleSource {
public:
    virtual ~ISampleSource() = default;

    // Startet die Lieferung von Samples (bei Geraeten: Frequenz einstellen).
    virtual bool restart(int32_t frequencyHz) = 0;
    virtual void stop() = 0;

    // Holt bis zu n Samples (normiert auf etwa -1..1); Rueckgabe: Anzahl.
    virtual int32_t getSamples(std::complex<float>* buffer, int32_t n) = 0;
    // Anzahl der aktuell abholbaren Samples.
    virtual int32_t samples() = 0;
    // Wartet, bis mindestens n Samples vorliegen oder timeoutMs abgelaufen
    // sind. Standard: Polling mit kurzem Schlaf; Quellen mit eigenem Thread
    // ueberschreiben das mit einer Condition-Variable.
    virtual bool waitForSamples(int32_t n, int timeoutMs);

    virtual int16_t bitDepth() const { return 10; }
    virtual std::string name() const = 0;
    virtual std::string serial() const { return std::string(); }
    virtual bool isFileInput() const { return false; }
    virtual int32_t vfoFrequency() const { return lastFrequency_; }

    virtual void setGain(int lna, int vga, bool amp) { (void)lna; (void)vga; (void)amp; }
    virtual void resetBuffer() {}

protected:
    int32_t lastFrequency_ = 0;
};
