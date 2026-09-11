// DAB Classic – dabcored: Kernprozess.
//
//   dabcored [--no-audio] [--audio-device NAME] [--aac auto|faad2|fdk]
//            [--events DATEI] [--fast] [--duration S]
//            [--file DATEI [--loop] | --device hackrf|rtlsdr [--channel 5C]
//                     [--gain LNA,VGA,AMP] [--no-agc] [--ppm N] [--scan]]
//            [--service NAME|0xSID ...] [--all-audio] [--wav DATEI]
//            [--iq-dump DATEI]
//
// Ohne --file/--device: Kommandos von stdin (JSON-Zeilen), Ereignisse auf
// stdout. Mit --file: Headless-Replay; Ereignisse auf stdout (oder --events),
// stdin wird trotzdem gelesen, damit die App eingreifen kann. Der Prozess
// endet bei EOF auf stdin, beim Kommando {"type":"shutdown"} oder – ohne
// --loop – am Dateiende (Ereignis file_ended, dann exiting).
//
// --fast      Datei ohne Echtzeit-Pacing abspielen (gilt auch fuer Dateien,
//             die spaeter per open_device geoeffnet werden)
// --duration  Wiedergabe nach S Sekunden *Dateizeit* beenden (nicht Echtzeit;
//             mit --fast also entsprechend frueher). Bei Geraeten: Echtzeit.
// --service   Dienst waehlen, sobald er in der FIC auftaucht (Name-Teilstring
//             oder 0xSID); der erste ist Primary (Audio), weitere Background
// --all-audio alle Audiodienste als Background dekodieren (DL+-Statistik)
// --wav       WAV-Dump (48 kHz, Stereo, 16 Bit) des Primary-Dienstes ab Start
// --aac       AAC-Decoder: auto (FDK, wenn libfdk-aac-2.dll vorliegt), faad2, fdk
// --device    HackRF One oder RTL-SDR oeffnen (M1); --channel stellt den Kanal ein
// --gain      Gain-Satz LNA,VGA,AMP (HackRF: 0-40, 0-62, 0|1; RTL-SDR: LNA =
//             Tuner-Gain in 0,1 dB); --no-agc schaltet die SNR-Nachfuehrung ab
// --scan      Band-III-Scan (single) statt Empfang; Tabelle auf stderr, dann Ende
// --iq-dump   Samples der Quelle als .uff (8 Bit) mitschreiben

#include "dabcore/core.h"
#include "dabcore/ipc.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

struct Args {
    bool audio = true;
    std::string eventsFile;
    std::string file;
    std::string device;
    std::string channel;
    std::string gain;
    bool agc = true;
    int ppm = 0;
    bool scan = false;
    std::string iqDump;
    std::vector<std::string> services;
    bool allAudio = false;
    std::string wav;
    std::string aac = "auto";
    std::string audioDevice;
    double duration = 0;
    bool fast = false;
    bool loop = false;
    bool help = false;
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto val = [&](std::string& out) { if (i + 1 < argc) out = argv[++i]; };
        if (s == "--no-audio") a.audio = false;
        else if (s == "--events") val(a.eventsFile);
        else if (s == "--file") val(a.file);
        else if (s == "--device") val(a.device);
        else if (s == "--channel") val(a.channel);
        else if (s == "--gain") val(a.gain);
        else if (s == "--no-agc") a.agc = false;
        else if (s == "--ppm") { std::string v; val(v); a.ppm = std::atoi(v.c_str()); }
        else if (s == "--scan") a.scan = true;
        else if (s == "--iq-dump") val(a.iqDump);
        else if (s == "--service") { std::string v; val(v); if (!v.empty()) a.services.push_back(v); }
        else if (s == "--all-audio") a.allAudio = true;
        else if (s == "--aac") val(a.aac);
        else if (s == "--audio-device") val(a.audioDevice);
        else if (s == "--wav") val(a.wav);
        else if (s == "--fast") a.fast = true;
        else if (s == "--loop") a.loop = true;
        else if (s == "--duration") { std::string d; val(d); a.duration = std::atof(d.c_str()); }
        else if (s == "-h" || s == "--help") a.help = true;
        else std::fprintf(stderr, "dabcored: unbekannte Option %s\n", s.c_str());
    }
    return a;
}

// "LNA,VGA,AMP" -> {lna, vga, amp}
dabcore::json parseGain(const std::string& s) {
    dabcore::json g = dabcore::json::object();
    int lna = 0, vga = 0, amp = 0;
    int n = std::sscanf(s.c_str(), "%d,%d,%d", &lna, &vga, &amp);
    if (n >= 1) g["lna"] = lna;
    if (n >= 2) g["vga"] = vga;
    if (n >= 3) g["amp"] = amp != 0;
    return g;
}

} // namespace

int main(int argc, char** argv) {
    Args args = parse(argc, argv);
    if (args.help) {
        std::puts("dabcored [--no-audio] [--audio-device NAME] [--aac auto|faad2|fdk] "
                  "[--events DATEI] [--fast] [--duration S]\n"
                  "         [--file DATEI [--loop] | --device hackrf|rtlsdr [--channel 5C] "
                  "[--gain LNA,VGA,AMP] [--no-agc] [--ppm N] [--scan]]\n"
                  "         [--service NAME|0xSID ...] [--all-audio] [--wav DATEI] [--iq-dump DATEI]");
        return 0;
    }
#ifdef _WIN32
    // Binaermodus: keine \r\n-Umwandlung, kein Konsolen-Encoding.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false);
    // cin ist an cout gebunden: jedes getline im Kommandothread wuerde cout
    // flushen, waehrend der EventWriter-Thread schreibt (unsynchronisierter
    // Streambuf -> sporadisch doppelte Zeilen). Bindung loesen.
    std::cin.tie(nullptr);

    std::ofstream eventsFile;
    std::ostream* out = &std::cout;
    if (!args.eventsFile.empty()) {
        eventsFile.open(args.eventsFile, std::ios::binary);
        if (!eventsFile) {
            std::fprintf(stderr, "dabcored: kann %s nicht schreiben\n", args.eventsFile.c_str());
            return 2;
        }
        out = &eventsFile;
    }

    dabcore::ipc::EventWriter writer(*out);

    // Beenden: stdin-EOF/shutdown (Kommandothread), Dateiende, Scan-Ende
    // (--scan) oder Zeitlimit (--duration bei Geraeten).
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::string exitReason;
    auto finish = [&](const std::string& reason) {
        std::lock_guard<std::mutex> lk(m);
        if (!done) { done = true; exitReason = reason; }
        cv.notify_all();
    };

    // Ereignis-Senke: alles an den Writer; bei --scan die Ergebnisse
    // mitschneiden und bei scan_finished beenden.
    std::mutex scanM;
    std::vector<dabcore::json> scanResults;
    dabcore::EventSink sink = [&](dabcore::json e) {
        if (args.scan) {
            const std::string t = e.value("type", "");
            if (t == "scan_result") { std::lock_guard<std::mutex> lk(scanM); scanResults.push_back(e); }
            else if (t == "scan_finished") finish("scan_finished");
        }
        writer.push(std::move(e));
    };

    dabcore::CoreOptions opt;
    opt.audio = args.audio;
    opt.fastReplay = args.fast;
    opt.replayDurationS = args.duration;
    opt.aacDecoder = args.aac;
    opt.audioDevice = args.audioDevice;
    opt.autoServices = args.services;
    opt.autoAllAudio = args.allAudio;
    opt.autoWav = args.wav;
    dabcore::DabCore core(sink, opt);
    core.setFileEndedHandler([&] { finish("file_ended"); });

    // Geraeteverlust: device_error kam schon; close_device aus einem
    // eigenen Thread (der Aufrufer ist ein Kernthread und darf nicht joinen).
    std::mutex coreM;
    std::vector<std::thread> helpers;
    std::mutex helpersM;
    core.setDeviceLostHandler([&] {
        std::lock_guard<std::mutex> hl(helpersM);
        helpers.emplace_back([&] {
            std::lock_guard<std::mutex> lk(coreM);
            core.handle({{"type", "close_device"}});
        });
    });

    if (!args.file.empty()) {
        core.handle({{"type", "open_device"},
                     {"source", {{"kind", "file"}, {"path", args.file}, {"loop", args.loop}, {"fast", args.fast}}}});
    } else if (!args.device.empty()) {
        dabcore::json source;
        if (args.device == "hackrf") source = {{"kind", "hack_rf"}};
        else if (args.device == "rtlsdr" || args.device == "rtl-sdr" || args.device == "rtl") source = {{"kind", "rtl_sdr"}, {"index", 0}};
        else { std::fprintf(stderr, "dabcored: unbekanntes Geraet %s\n", args.device.c_str()); return 2; }
        if (!args.gain.empty()) core.handle({{"type", "set_gain"}, {"gain", parseGain(args.gain)}});
        core.handle({{"type", "set_agc"}, {"enabled", args.agc}});
        if (args.ppm != 0) core.handle({{"type", "set_ppm"}, {"ppm", args.ppm}});
        if (!args.channel.empty() && !args.scan) core.handle({{"type", "set_channel"}, {"channel", args.channel}});
        core.handle({{"type", "open_device"}, {"source", source}});
        if (args.scan) core.handle({{"type", "start_scan"}, {"channels", dabcore::json::array()}, {"mode", "single"}});
    }
    if (!args.iqDump.empty()) core.handle({{"type", "start_iq_dump"}, {"path", args.iqDump}});

    // Kommandothread: liest stdin, bis EOF oder shutdown. Im zeitgesteuerten
    // Geraetebetrieb (--scan, --device mit --duration) beendet ein
    // geschlossenes stdin (z. B. Aufruf aus einem Skript) den Kern nicht.
    const bool timedHeadless = !args.device.empty() && (args.scan || args.duration > 0);
    std::thread reader([&] {
        dabcore::ipc::CommandReader r(std::cin, writer.sink());
        dabcore::json cmd;
        while (r.next(cmd)) {
            std::lock_guard<std::mutex> lk(coreM);
            if (!core.handle(cmd)) { finish("shutdown"); return; }
        }
        if (std::getenv("DABCORE_TRACE")) std::fprintf(stderr, "dabcored: stdin EOF\n");
        if (!timedHeadless) finish("stdin");
    });

    {
        std::unique_lock<std::mutex> lk(m);
        if (!args.device.empty() && args.duration > 0 && !args.scan) {
            // Geraet: --duration ist Echtzeit
            cv.wait_for(lk, std::chrono::duration<double>(args.duration), [&] { return done; });
            if (!done) { done = true; exitReason = "duration"; }
        } else {
            cv.wait(lk, [&] { return done; });
        }
    }
    const bool trace = std::getenv("DABCORE_TRACE") != nullptr;
    if (trace) std::fprintf(stderr, "dabcored: Ende wegen %s\n", exitReason.c_str());
    {
        std::lock_guard<std::mutex> lk(coreM);
        if (trace) std::fprintf(stderr, "dabcored: coreM gehalten, close_device\n");
        if (exitReason == "file_ended") {
            core.handle({{"type", "close_device"}});
            writer.push(dabcore::events::exiting("Dateiende"));
        } else if (exitReason == "stdin") {
            core.handle({{"type", "close_device"}});
            writer.push(dabcore::events::exiting("stdin geschlossen"));
        } else if (exitReason == "scan_finished" || exitReason == "duration") {
            core.handle({{"type", "close_device"}});
            writer.push(dabcore::events::exiting(exitReason == "duration" ? "Zeitlimit" : "Scan beendet"));
        }
    }
    if (args.scan) {
        // Tabelle aller Kanaele auf stderr
        std::lock_guard<std::mutex> lk(scanM);
        std::fprintf(stderr, "\n%-5s %-6s %-24s %6s %8s\n", "Kanal", "EId", "Ensemble", "Dienste", "SNR dB");
        int found = 0;
        for (auto& r : scanResults) {
            bool has = r["eid"].is_number();
            if (has) found++;
            std::fprintf(stderr, "%-5s %-6s %-24s %6zu %8.1f\n", r.value("channel", "").c_str(),
                         has ? (std::to_string(r["eid"].get<int>())).c_str() : "-",
                         has ? r.value("ensemble", "").c_str() : "-",
                         r["services"].is_array() ? r["services"].size() : 0,
                         r.value("snr", 0.0));
        }
        std::fprintf(stderr, "%zu Kanaele, %d Ensembles\n", scanResults.size(), found);
    }
    if (trace) std::fprintf(stderr, "dabcored: close_device fertig, writer.close\n");
    writer.close();
    out->flush();
    if (trace) std::fprintf(stderr, "dabcored: writer geschlossen\n");
    {
        std::lock_guard<std::mutex> hl(helpersM);
        for (auto& h : helpers) if (h.joinable()) h.join();
    }
    if (exitReason == "shutdown" || exitReason == "stdin" || (timedHeadless && std::cin.eof())) {
        reader.join();
        return 0;
    }
    // Der Kommandothread haengt noch in getline(stdin); Prozess direkt beenden.
    std::fflush(stdout);
    std::_Exit(0);
}
