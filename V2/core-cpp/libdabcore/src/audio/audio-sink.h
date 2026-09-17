// DAB Classic v3 – Schnittstelle der Audio-Ausgabe (Ersatz fuer das
// QObject-Interface audioPlayer aus Qt-DAB output/audio-player.h).
// Eingabe: verschachtelte Float-Paare (L, R) mit 48 kHz.
#pragma once

#include "dabcore/events.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class IAudioSink {
public:
    virtual ~IAudioSink() = default;
    // Ausgabe starten (Standardgeraet oder zuletzt gewaehltes).
    virtual bool start() = 0;
    virtual void stop() = 0;
    // Anzahl Floats (2 je Rahmen); ueberzaehlige Daten werden verworfen.
    virtual void write(const float* samples, uint32_t count) = 0;
    // Geraeteliste (nur Ausgabegeraete mit 48 kHz Stereo; unter Windows nur
    // WASAPI, damit jedes Geraet genau einmal erscheint) und die id des
    // Geraets, das gerade spielt bzw. beim Start benutzt wuerde ("" = keins).
    virtual std::vector<dabcore::AudioDeviceInfo> devices() = 0;
    virtual std::string currentDevice() = 0;
    // Geraet per id waehlen; "" = Standardgeraet des Systems (folgt dessen
    // Wechseln). Eine unbekannte id bleibt gemerkt (Geraet spaeter angesteckt),
    // bis dahin spielt das Standardgeraet; Rueckgabe dann false.
    virtual bool selectDevice(const std::string& id) = 0;
    // Geraeteliste neu einlesen (an-/abgesteckte Geraete), Ausgabe laeuft
    // danach auf dem passenden Geraet weiter.
    virtual void refreshDevices() {}
    // Wird (aus einem eigenen Thread) gerufen, wenn sich Liste, Standard-
    // geraet oder das benutzte Geraet von selbst geaendert haben.
    virtual void setChangeHandler(std::function<void()>) {}
    // Seit dem letzten Aufruf fehlende Samples (Underrun), setzt zurueck.
    virtual uint32_t takeMissed() = 0;
    // Ausstehende Ausgabedaten verwerfen (Timeshift: Sprung auf live, sonst
    // hoert man noch den Inhalt des Ausgabepuffers, ~0,7 s).
    virtual void flush() {}
    virtual const char* name() const = 0;
};

class NullAudioSink : public IAudioSink {
public:
    bool start() override { return true; }
    void stop() override {}
    void write(const float*, uint32_t) override {}
    std::vector<dabcore::AudioDeviceInfo> devices() override { return {}; }
    std::string currentDevice() override { return {}; }
    bool selectDevice(const std::string&) override { return false; }
    uint32_t takeMissed() override { return 0; }
    void flush() override {}
    const char* name() const override { return "null"; }
};
