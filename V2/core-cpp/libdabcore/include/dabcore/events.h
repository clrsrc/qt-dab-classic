// DAB Classic – Ereignisse des Kerns (Kern -> App).
// Spiegelbild von crates/dab-api/src/lib.rs (enum Event). Jedes Ereignis ist
// ein JSON-Objekt mit "type" in snake_case; docs/protocol.md ist die Referenz.
#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dabcore {

using json = nlohmann::json;

// Senke fuer Ereignisse; wird aus beliebigen Kernthreads aufgerufen und muss
// thread-sicher sein (siehe ipc::EventWriter).
using EventSink = std::function<void(json)>;

// Ereignisse, bei denen nur der letzte Wert zaehlt (duerfen verworfen werden).
bool isLatestWins(const std::string& type);

enum class Slot { Primary, Background };
const char* slotName(Slot s);

enum class EwsPhase { PreTrigger, Trigger, Sustain, End };
const char* ewsPhaseName(EwsPhase p);

struct ServiceInfo {
    uint32_t sid = 0;
    uint8_t  scids = 0;
    std::string name;
    bool     isAudio = true;
    bool     isPrimary = true;
    uint8_t  subCh = 0;
    uint16_t bitrateKbps = 0;
    uint8_t  pty = 0;
    json toJson() const;
};

// Fabrikfunktionen – Namen und Felder exakt wie in dab-api.
namespace events {
json ready(const std::string& coreVersion, uint32_t protocolVersion, const std::vector<std::string>& decoders);
json deviceOpened(const std::string& name, const std::string& serial, uint8_t bitDepth);
json deviceClosed();
json deviceError(const std::string& message);
// Gain-Satz nach set_gain, AGC-Nachfuehrung, AMP-Retry im Scan oder beim
// Oeffnen eines Geraets (Entscheidung 26: die App speichert ihn je Kanal).
json gainChanged(int lna, int vga, bool amp, bool agc);
json fileProgress(double positionS, double lengthS);
json fileEnded();

json synced(bool synced);
json noSignal(const std::string& channel);
json snr(float db);
json ficQuality(uint16_t ok, uint16_t total);
json frequencyOffset(int32_t hz);
json ensembleFound(uint16_t eid, const std::string& name, const std::string& channel);
json serviceAdded(const ServiceInfo& s);
json ensembleReconfigured();
json clockTime(int64_t unixUtc, int16_t ltoMinutes);

json serviceStarted(Slot slot, uint32_t sid, uint8_t scids, bool heAac, bool sbr, bool ps, uint32_t sampleRate, bool stereo);
// Paketdienst (MOT/EPG): codec {codec: data}
json serviceStartedData(Slot slot, uint32_t sid, uint8_t scids);
json serviceStopped(Slot slot, uint32_t sid);
json serviceStats(Slot slot, uint32_t sid, uint16_t frameErrors, uint16_t rsErrors, uint16_t aacErrors, uint16_t rsCorrections);
json dls(Slot slot, uint32_t sid, const std::string& text);
json dlPlus(Slot slot, uint32_t sid, bool itemToggle, bool itemRunning, const std::vector<std::pair<uint8_t, std::string>>& tags);
json motSlide(Slot slot, uint32_t sid, const std::string& mime, const std::string& name, const std::vector<uint8_t>& data);
// MOT-Objekt eines Paketdienstes (SPI-Logo, Text, ...): sid = Dienst, dem
// das Objekt gilt (aus dem Namen "d210_Dlf_32x32.png"), sonst 0; eid = Ensemble.
json motObject(uint16_t eid, uint32_t sid, uint16_t contentType, const std::string& name, const std::vector<uint8_t>& data);
// EPG-Sendeplan (sid/date aus dem MOT-Namen) oder Service-Information
// (sid = 0, date = 0; v1: list.xml) als XML-Text des epg-compilers.
json epgObject(uint16_t eid, uint32_t sid, uint32_t dateYyyymmdd, const std::string& name, const std::string& xml);
// Durchsage (FIG 0/18 x 0/19, Verkehrsfunk-Vorbereitung 16.09.2026):
// sid = angekuendigter Dienst, kind = ASu & ASw (16 Flags, Bit 0 Alarm,
// Bit 1 Verkehr, ...), sub_ch = Subkanal der Durchsage aus FIG 0/19,
// active = kind != 0, cluster = Cluster-Id. dab-api kennt bisher
// kind/sub_ch/active; sid/cluster sind additiv (serde ignoriert Unbekanntes).
json announcement(uint32_t sid, uint16_t kind, uint8_t subCh, bool active, uint8_t cluster);

json audioFormat(uint32_t rate, uint8_t channels);
json audioLevel(float left, float right);
json audioUnderrun(uint32_t missed);
json audioDevices(const std::vector<std::string>& names, int current);

json ewsPresent();
// stageRaw: rohes Status-Byte der FIG 0/15 (Bit 7 Last, Bits 6..4 Stage, Bits 3..0 IId), Warntag 2026: 0x01
// relevant: Geofencing-Ergebnis (Annex-F-Ortscodes gegen die Heimatposition
// aus set_home_location). std::nullopt = keine Heimatposition gesetzt, das
// Feld wird dann als JSON null gesendet (App: "unbekannt" -> immer relevant).
json ewsAlert(EwsPhase phase, uint8_t subCh, uint8_t stage, uint8_t stageRaw, uint16_t iid, const std::vector<std::string>& locations, bool isTest,
              std::optional<bool> relevant);
// subCh < 0: Heartbeat ohne aktiven Alarm (sub_ch = null)
json ewsAlive(int subCh);
json ewfAlarm(bool active, uint8_t subCh);
json ewsSwitched(uint32_t toSid, int64_t fromSid /* <0 = keiner */);

json recordingState(Slot slot, uint32_t sid, bool active, const std::string& path, uint64_t bytes, double seconds);
// mode live|paused|playing; frame_index (Schreibzeiger) und live_unix
// (Ensemble-Uhrzeit am Schreibzeiger, 0 = unbekannt) kamen in M4 additiv dazu.
json timeshiftState(const char* mode, double bufferedS, double offsetS, double capacityS,
                    uint64_t frameIndex, int64_t liveUnix);

json scanProgress(const std::string& channel, uint16_t index, uint16_t total);
json scanResult(const std::string& channel, int eid, const std::string& ensemble, const std::vector<ServiceInfo>& services, float snr);
json scanFinished();

json tii(const std::vector<std::tuple<uint8_t, uint8_t, float>>& transmitters);
json spectrum(const std::vector<uint8_t>& binsDb);
// Konstellation eines OFDM-Symbols (1536 Traeger nach der Differenz-
// demodulation) als int8-Paare I,Q (127 = 1,0), Base64.
json iqSamples(const std::vector<int8_t>& iq);
json log(const char* level, const std::string& text);
json exiting(const std::string& reason);
} // namespace events

// Base64 (fuer Binaerdaten in Ereignissen).
std::string base64Encode(const uint8_t* data, size_t len);
inline std::string base64Encode(const std::vector<uint8_t>& v) { return base64Encode(v.data(), v.size()); }

} // namespace dabcore
