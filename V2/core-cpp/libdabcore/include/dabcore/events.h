// DAB Classic – Ereignisse des Kerns (Kern -> App).
// Spiegelbild von crates/dab-api/src/lib.rs (enum Event). Jedes Ereignis ist
// ein JSON-Objekt mit "type" in snake_case; docs/protocol.md ist die Referenz.
#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
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
json motObject(uint32_t sid, uint16_t contentType, const std::string& name, const std::vector<uint8_t>& data);
json epgObject(uint32_t sid, uint32_t dateYyyymmdd, const std::string& xml);
json announcement(uint16_t kind, uint8_t subCh, bool active);

json audioFormat(uint32_t rate, uint8_t channels);
json audioLevel(float left, float right);
json audioUnderrun(uint32_t missed);
json audioDevices(const std::vector<std::string>& names, int current);

json ewsPresent();
json ewsAlert(EwsPhase phase, uint8_t subCh, uint8_t stage, uint16_t iid, const std::vector<std::string>& locations, bool isTest);
// subCh < 0: Heartbeat ohne aktiven Alarm (sub_ch = null)
json ewsAlive(int subCh);
json ewfAlarm(bool active, uint8_t subCh);
json ewsSwitched(uint32_t toSid, int64_t fromSid /* <0 = keiner */);

json recordingState(Slot slot, uint32_t sid, bool active, const std::string& path, uint64_t bytes, double seconds);
json timeshiftState(const char* mode, double bufferedS, double offsetS, double capacityS);

json scanProgress(const std::string& channel, uint16_t index, uint16_t total);
json scanResult(const std::string& channel, int eid, const std::string& ensemble, const std::vector<ServiceInfo>& services, float snr);
json scanFinished();

json tii(const std::vector<std::tuple<uint8_t, uint8_t, float>>& transmitters);
json spectrum(const std::vector<uint8_t>& binsDb);
json log(const char* level, const std::string& text);
json exiting(const std::string& reason);
} // namespace events

// Base64 (fuer Binaerdaten in Ereignissen).
std::string base64Encode(const uint8_t* data, size_t len);
inline std::string base64Encode(const std::vector<uint8_t>& v) { return base64Encode(v.data(), v.size()); }

} // namespace dabcore
