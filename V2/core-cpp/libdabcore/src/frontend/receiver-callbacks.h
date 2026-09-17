// DAB Classic v3 – Callbacks des Empfangspfads (Ersatz fuer die Qt-Signale
// von ofdmHandler, ficHandler, fibDecoder und fibConfig, die in v1 an
// RadioInterface gingen). Alle Aufrufe kommen aus dem OFDM-Thread.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "dab-constants.h"

struct ReceiverCallbacks {
    // ofdmHandler
    std::function<void(bool)>                 synced;          // setSynced
    std::function<void()>                     noSignal;        // noSignalFound
    std::function<void()>                     syncLost;        // setSyncLost
    std::function<void(float)>                snr;             // showSnr(float)
    std::function<void(int)>                  clockError;      // showClockError
    std::function<void(int, float)>           corrector;       // showCorrector(coarse, fine)
    std::function<void(const std::vector<tiiData>&)> tii;      // showTIIData
    // Scopes (nur aktiv nach set_scopes): Spektrum der Eingangssamples als
    // 2048 Bins (dB-Skala 0..255, fftshift: Bin 0 = -1,024 MHz) und die
    // Konstellation von Symbol 2 (1536 Traeger als int8-Paare I,Q).
    std::function<void(const std::vector<uint8_t>&)> spectrum;
    std::function<void(const std::vector<int8_t>&)>  iqSamples;
    // ficHandler
    std::function<void(int, int)>             ficQuality;      // showFICQuality(ok, total)
    std::function<void(float)>                ficBer;          // showFICBER
    // fibDecoder
    std::function<void(const std::string& name, uint32_t sid, int subChId, bool primary)> addToEnsemble;
    std::function<void(uint16_t eid, const std::string& name)> ensembleName;
    // clockTime: MJD und UTC-Zeit aus FIG 0/10, LTO in Minuten aus FIG 0/9,
    // dazu die lokal umgerechneten Felder wie in v1 (Jahr, Monat, Tag, h, min)
    std::function<void(uint32_t mjd, int utcHour, int utcMinute, int utcSeconds, int ltoMinutes,
                       int year, int month, int day, int hour, int minute)> clockTime;
    std::function<void()>                     changeInConfiguration;
    // Durchsage (FIG 0/18 x 0/19): sid = angekuendigter Dienst, flags = ASu & ASw
    // (16 Bit, 0 = Ende), clusterId, subChId = Subkanal der Durchsage (FIG 0/19)
    std::function<void(int sid, int flags, int clusterId, int subChId)> announcement;
    std::function<void(int)>                  nrServices;
    std::function<void(int lto, int ecc)>     ltoEcc;
    std::function<void()>                     freqListChanged;
    std::function<void(int sid, int pty)>     programType;
    // Sprache einer Komponente aus FIG 0/5 (TS 101 756), subChId = Subkanal
    // der Komponente; kommt haeufig vor dem Label des Dienstes, dann liest
    // emitService sie direkt aus audioData, sonst wird der Dienst neu gemeldet.
    std::function<void(int subChId, int language)> language;
    std::function<void(bool)>                 alarmFlag;
    std::function<void(bool active, int subChId)> ewfAlarm;
    // EWS (FIG 0/15): phase 0 Pre-trigger, 1 Trigger, 2 Sustain, 3 End;
    // stageRaw = rohes Status-Byte (Last/Stage/IId) der Trigger-Instanz
    std::function<void(int phase, int subChId, int stage, int stageRaw, int iid,
                       const std::vector<std::string>& locations)> ewsAlert;
    // subChId < 0: Heartbeat ohne aktiven Alarm
    std::function<void(int subChId)>          ewsAlive;
    std::function<void()>                     ewsPresent;
    // Diagnose-Ausgaben, die v1 per fprintf(stderr) machte
    std::function<void(const char* level, const std::string& text)> log;
};

// Hilfsfunktion: Callback nur aufrufen, wenn gesetzt.
template <class F, class... A>
inline void emitCb(const F& f, A&&... a) {
    if (f) f(std::forward<A>(a)...);
}
