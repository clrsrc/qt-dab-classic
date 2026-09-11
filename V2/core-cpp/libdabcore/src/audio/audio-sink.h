// DAB Classic v3 – Schnittstelle der Audio-Ausgabe (Ersatz fuer das
// QObject-Interface audioPlayer aus Qt-DAB output/audio-player.h).
// Eingabe: verschachtelte Float-Paare (L, R) mit 48 kHz.
#pragma once

#include <cstdint>
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
    // Geraeteliste (nur Geraete mit 48 kHz Stereo) und aktueller Index darin.
    virtual std::vector<std::string> devices() = 0;
    virtual int currentDevice() = 0;
    virtual bool selectDevice(int index) = 0;
    // Seit dem letzten Aufruf fehlende Samples (Underrun), setzt zurueck.
    virtual uint32_t takeMissed() = 0;
    virtual const char* name() const = 0;
};

class NullAudioSink : public IAudioSink {
public:
    bool start() override { return true; }
    void stop() override {}
    void write(const float*, uint32_t) override {}
    std::vector<std::string> devices() override { return {}; }
    int currentDevice() override { return -1; }
    bool selectDevice(int) override { return false; }
    uint32_t takeMissed() override { return 0; }
    const char* name() const override { return "null"; }
};
