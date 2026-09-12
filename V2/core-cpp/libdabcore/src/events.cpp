#include "dabcore/events.h"

namespace dabcore {

bool isLatestWins(const std::string& t) {
    return t == "snr" || t == "fic_quality" || t == "frequency_offset" || t == "audio_level" ||
           t == "spectrum" || t == "iq_samples" || t == "timeshift_state" || t == "file_progress" ||
           t == "service_stats";
}

const char* slotName(Slot s) { return s == Slot::Primary ? "primary" : "background"; }

const char* ewsPhaseName(EwsPhase p) {
    switch (p) {
        case EwsPhase::PreTrigger: return "pre_trigger";
        case EwsPhase::Trigger:    return "trigger";
        case EwsPhase::Sustain:    return "sustain";
        case EwsPhase::End:        return "end";
    }
    return "end";
}

json ServiceInfo::toJson() const {
    return {{"sid", sid}, {"scids", scids}, {"name", name}, {"is_audio", isAudio},
            {"is_primary", isPrimary}, {"sub_ch", subCh}, {"bitrate_kbps", bitrateKbps}, {"pty", pty}};
}

static json ev(const char* type) { return json{{"type", type}}; }

namespace events {
json ready(const std::string& v, uint32_t pv, const std::vector<std::string>& d) {
    auto j = ev("ready"); j["core_version"] = v; j["protocol_version"] = pv; j["decoders"] = d; return j;
}
json deviceOpened(const std::string& n, const std::string& s, uint8_t b) {
    auto j = ev("device_opened"); j["name"] = n; j["serial"] = s; j["bit_depth"] = b; return j;
}
json deviceClosed() { return ev("device_closed"); }
json deviceError(const std::string& m) { auto j = ev("device_error"); j["message"] = m; return j; }
json gainChanged(int lna, int vga, bool amp, bool agc) {
    auto j = ev("gain_changed"); j["lna"] = lna; j["vga"] = vga; j["amp"] = amp; j["agc"] = agc; return j;
}
json fileProgress(double p, double l) { auto j = ev("file_progress"); j["position_s"] = p; j["length_s"] = l; return j; }
json fileEnded() { return ev("file_ended"); }

json synced(bool s) { auto j = ev("synced"); j["synced"] = s; return j; }
json noSignal(const std::string& c) { auto j = ev("no_signal"); j["channel"] = c; return j; }
json snr(float db) { auto j = ev("snr"); j["db"] = db; return j; }
json ficQuality(uint16_t ok, uint16_t total) { auto j = ev("fic_quality"); j["ok"] = ok; j["total"] = total; return j; }
json frequencyOffset(int32_t hz) { auto j = ev("frequency_offset"); j["hz"] = hz; return j; }
json ensembleFound(uint16_t eid, const std::string& n, const std::string& c) {
    auto j = ev("ensemble_found"); j["eid"] = eid; j["name"] = n; j["channel"] = c; return j;
}
json serviceAdded(const ServiceInfo& s) { auto j = ev("service_added"); j["service"] = s.toJson(); return j; }
json ensembleReconfigured() { return ev("ensemble_reconfigured"); }
json clockTime(int64_t u, int16_t lto) { auto j = ev("clock_time"); j["unix_utc"] = u; j["lto_minutes"] = lto; return j; }

json serviceStarted(Slot slot, uint32_t sid, uint8_t scids, bool heAac, bool sbr, bool ps, uint32_t rate, bool stereo) {
    auto j = ev("service_started");
    j["slot"] = slotName(slot); j["sid"] = sid; j["scids"] = scids; j["stereo"] = stereo;
    if (heAac) j["codec"] = {{"codec", "he_aac"}, {"sbr", sbr}, {"ps", ps}, {"sample_rate", rate}};
    else       j["codec"] = {{"codec", "mp2"}, {"sample_rate", rate}};
    return j;
}
json serviceStartedData(Slot slot, uint32_t sid, uint8_t scids) {
    auto j = ev("service_started");
    j["slot"] = slotName(slot); j["sid"] = sid; j["scids"] = scids; j["stereo"] = false;
    j["codec"] = {{"codec", "data"}};
    return j;
}
json serviceStopped(Slot slot, uint32_t sid) { auto j = ev("service_stopped"); j["slot"] = slotName(slot); j["sid"] = sid; return j; }
json serviceStats(Slot slot, uint32_t sid, uint16_t fe, uint16_t rs, uint16_t aac, uint16_t rsc) {
    auto j = ev("service_stats"); j["slot"] = slotName(slot); j["sid"] = sid;
    j["frame_errors"] = fe; j["rs_errors"] = rs; j["aac_errors"] = aac; j["rs_corrections"] = rsc; return j;
}
json dls(Slot slot, uint32_t sid, const std::string& t) { auto j = ev("dls"); j["slot"] = slotName(slot); j["sid"] = sid; j["text"] = t; return j; }
json dlPlus(Slot slot, uint32_t sid, bool it, bool ir, const std::vector<std::pair<uint8_t, std::string>>& tags) {
    auto j = ev("dl_plus"); j["slot"] = slotName(slot); j["sid"] = sid; j["item_toggle"] = it; j["item_running"] = ir;
    json arr = json::array();
    for (auto& [ct, s] : tags) arr.push_back(json::array({ct, s}));
    j["tags"] = arr; return j;
}
json motSlide(Slot slot, uint32_t sid, const std::string& mime, const std::string& name, const std::vector<uint8_t>& d) {
    auto j = ev("mot_slide"); j["slot"] = slotName(slot); j["sid"] = sid; j["mime"] = mime; j["name"] = name; j["data_b64"] = base64Encode(d); return j;
}
json motObject(uint16_t eid, uint32_t sid, uint16_t ct, const std::string& name, const std::vector<uint8_t>& d) {
    auto j = ev("mot_object"); j["eid"] = eid; j["sid"] = sid; j["content_type"] = ct; j["name"] = name; j["data_b64"] = base64Encode(d); return j;
}
json epgObject(uint16_t eid, uint32_t sid, uint32_t date, const std::string& name, const std::string& xml) {
    auto j = ev("epg_object"); j["eid"] = eid; j["sid"] = sid; j["date_yyyymmdd"] = date; j["name"] = name; j["xml"] = xml; return j;
}
json announcement(uint16_t kind, uint8_t subCh, bool active) {
    auto j = ev("announcement"); j["kind"] = kind; j["sub_ch"] = subCh; j["active"] = active; return j;
}

json audioFormat(uint32_t rate, uint8_t ch) { auto j = ev("audio_format"); j["rate"] = rate; j["channels"] = ch; return j; }
json audioLevel(float l, float r) { auto j = ev("audio_level"); j["left"] = l; j["right"] = r; return j; }
json audioUnderrun(uint32_t m) { auto j = ev("audio_underrun"); j["missed"] = m; return j; }
json audioDevices(const std::vector<std::string>& names, int current) {
    auto j = ev("audio_devices"); j["names"] = names;
    if (current >= 0) j["current"] = current; else j["current"] = nullptr; return j;
}

json ewsPresent() { return ev("ews_present"); }
json ewsAlert(EwsPhase phase, uint8_t subCh, uint8_t stage, uint8_t stageRaw, uint16_t iid, const std::vector<std::string>& loc, bool test) {
    auto j = ev("ews_alert"); j["phase"] = ewsPhaseName(phase); j["sub_ch"] = subCh; j["stage"] = stage;
    j["stage_raw"] = stageRaw; j["iid"] = iid; j["locations"] = loc; j["is_test"] = test; return j;
}
json ewsAlive(int subCh) {
    auto j = ev("ews_alive");
    if (subCh < 0) j["sub_ch"] = nullptr; else j["sub_ch"] = static_cast<uint8_t>(subCh);
    return j;
}
json ewfAlarm(bool active, uint8_t subCh) { auto j = ev("ewf_alarm"); j["active"] = active; j["sub_ch"] = subCh; return j; }
json ewsSwitched(uint32_t to, int64_t from) {
    auto j = ev("ews_switched"); j["to_sid"] = to;
    if (from >= 0) j["from_sid"] = static_cast<uint32_t>(from); else j["from_sid"] = nullptr; return j;
}

json recordingState(Slot slot, uint32_t sid, bool active, const std::string& path, uint64_t bytes, double s) {
    auto j = ev("recording_state"); j["slot"] = slotName(slot); j["sid"] = sid; j["active"] = active;
    if (path.empty()) j["path"] = nullptr; else j["path"] = path;
    j["bytes"] = bytes; j["seconds"] = s; return j;
}
json timeshiftState(const char* mode, double b, double o, double c) {
    auto j = ev("timeshift_state"); j["mode"] = mode; j["buffered_s"] = b; j["offset_s"] = o; j["capacity_s"] = c; return j;
}

json scanProgress(const std::string& ch, uint16_t i, uint16_t t) {
    auto j = ev("scan_progress"); j["channel"] = ch; j["index"] = i; j["total"] = t; return j;
}
json scanResult(const std::string& ch, int eid, const std::string& ens, const std::vector<ServiceInfo>& sv, float snr) {
    auto j = ev("scan_result"); j["channel"] = ch;
    if (eid >= 0) { j["eid"] = eid; j["ensemble"] = ens; } else { j["eid"] = nullptr; j["ensemble"] = nullptr; }
    json arr = json::array(); for (auto& s : sv) arr.push_back(s.toJson());
    j["services"] = arr; j["snr"] = snr; return j;
}
json scanFinished() { return ev("scan_finished"); }

json tii(const std::vector<std::tuple<uint8_t, uint8_t, float>>& tx) {
    auto j = ev("tii"); json arr = json::array();
    for (auto& [m, s, st] : tx) arr.push_back({{"main_id", m}, {"sub_id", s}, {"strength", st}});
    j["transmitters"] = arr; return j;
}
json spectrum(const std::vector<uint8_t>& bins) { auto j = ev("spectrum"); j["bins_b64"] = base64Encode(bins); return j; }
json iqSamples(const std::vector<int8_t>& iq) {
    auto j = ev("iq_samples");
    j["iq_b64"] = base64Encode(reinterpret_cast<const uint8_t*>(iq.data()), iq.size());
    return j;
}
json log(const char* level, const std::string& t) { auto j = ev("log"); j["level"] = level; j["text"] = t; return j; }
json exiting(const std::string& r) { auto j = ev("exiting"); j["reason"] = r; return j; }
} // namespace events

std::string base64Encode(const uint8_t* data, size_t len) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(tbl[(v >> 18) & 63]); out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);  out.push_back(tbl[v & 63]);
    }
    if (i < len) {
        uint32_t v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        out.push_back(tbl[(v >> 18) & 63]); out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(i + 1 < len ? tbl[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

} // namespace dabcore
