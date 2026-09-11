// DAB Classic v3 – Schnittstelle einer Sample-Quelle (Ersatz fuer Qt-DAB
// devices/device-handler.h: kein QThread, kein Widget). Geraete (HackRF,
// RTL-SDR) und Dateileser implementieren dieses Interface; der OFDM-Thread
// zieht Samples ueber getSamples()/waitForSamples().
#pragma once

#include <complex>
#include <cstdint>
#include <functional>
#include <string>

// Gain-Satz DeviceGain: siehe dabcore/gain.h (HackRF: LNA/VGA/AMP, RTL-SDR:
// lna = Tuner-Gain in 0,1 dB).
#include "dabcore/gain.h"

class ISampleSource {
public:
    virtual ~ISampleSource() = default;

    // Startet die Lieferung von Samples (bei Geraeten: Frequenz einstellen).
    // samplesToSkip: nach dem Umschalten verworfene Samples (v1 toSkip,
    // Einschwingen des Tuners; radio.cpp: SAMPLERATE / 10).
    virtual bool restart(int32_t frequencyHz, int32_t samplesToSkip = 0) = 0;
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

    // --- Gain / Korrektur (Geraete; Dateien ignorieren das) ---------------
    // Setzt den Gain-Satz; ungueltige Werte werden gerundet/geklemmt.
    // Rueckgabe: der tatsaechlich eingestellte Satz.
    virtual DeviceGain setGain(const DeviceGain& g) { (void)g; return gain_; }
    virtual DeviceGain gain() const { return gain_; }
    virtual bool hasAmp() const { return false; }
    // SNR-Nachfuehrung (v1 deviceHandler::adjustGain); true = Gain geaendert.
    virtual bool adjustGain(float snr) { (void)snr; return false; }
    virtual void setPpm(int ppm) { ppm_ = ppm; }
    virtual int  ppm() const { return ppm_; }
    virtual void resetBuffer() {}

    // --- Sample-Dump (.uff, xml-filewriter) -------------------------------
    virtual bool startDump(const std::string& path, std::string& error) { (void)path; error = "Dump von dieser Quelle nicht moeglich"; return false; }
    virtual void stopDump() {}
    virtual bool dumping() const { return false; }

    // Wird (aus einem Quellen- oder Verbraucher-Thread) aufgerufen, wenn das
    // Geraet keine Daten mehr liefert (USB-Abriss, Bibliotheksfehler). Der
    // Aufrufer darf daraus keine Kernfunktionen rufen, die die Quelle joinen.
    void setErrorCallback(std::function<void(const std::string&)> cb) { errorCb_ = std::move(cb); }

protected:
    void reportError(const std::string& msg) { if (errorCb_) errorCb_(msg); }

    int32_t lastFrequency_ = 0;
    DeviceGain gain_;
    int ppm_ = 0;
    std::function<void(const std::string&)> errorCb_;
};
