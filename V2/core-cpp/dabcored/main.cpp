// DAB Classic – dabcored: Kernprozess.
//
//   dabcored [--no-audio] [--audio-device NAME] [--aac auto|faad2|fdk]
//            [--events DATEI] [--fast] [--duration S]
//            [--file DATEI [--loop] [--service NAME|0xSID ...] [--all-audio]
//                          [--wav DATEI]]
//
// Ohne --file: Kommandos von stdin (JSON-Zeilen), Ereignisse auf stdout.
// Mit --file: Headless-Replay; Ereignisse auf stdout (oder --events), stdin
// wird trotzdem gelesen, damit die App eingreifen kann. Der Prozess endet
// bei EOF auf stdin, beim Kommando {"type":"shutdown"} oder – ohne --loop –
// am Dateiende (Ereignis file_ended, dann exiting).
//
// --fast      Datei ohne Echtzeit-Pacing abspielen (gilt auch fuer Dateien,
//             die spaeter per open_device geoeffnet werden)
// --duration  Wiedergabe nach S Sekunden *Dateizeit* beenden (nicht Echtzeit;
//             mit --fast also entsprechend frueher)
// --service   Dienst waehlen, sobald er in der FIC auftaucht (Name-Teilstring
//             oder 0xSID); der erste ist Primary (Audio), weitere Background
// --all-audio alle Audiodienste als Background dekodieren (DL+-Statistik)
// --wav       WAV-Dump (48 kHz, Stereo, 16 Bit) des Primary-Dienstes ab Start
// --aac       AAC-Decoder: auto (FDK, wenn libfdk-aac-2.dll vorliegt), faad2, fdk

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

} // namespace

int main(int argc, char** argv) {
    Args args = parse(argc, argv);
    if (args.help) {
        std::puts("dabcored [--no-audio] [--audio-device NAME] [--aac auto|faad2|fdk] "
                  "[--events DATEI] [--fast] [--duration S]\n"
                  "         [--file DATEI [--loop] [--service NAME|0xSID ...] [--all-audio] [--wav DATEI]]");
        return 0;
    }
#ifdef _WIN32
    // Binaermodus: keine \r\n-Umwandlung, kein Konsolen-Encoding.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false);

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
    dabcore::CoreOptions opt;
    opt.audio = args.audio;
    opt.fastReplay = args.fast;
    opt.replayDurationS = args.duration;
    opt.aacDecoder = args.aac;
    opt.audioDevice = args.audioDevice;
    opt.autoServices = args.services;
    opt.autoAllAudio = args.allAudio;
    opt.autoWav = args.wav;
    dabcore::DabCore core(writer.sink(), opt);

    // Beenden: entweder stdin-EOF/shutdown (Kommandothread) oder Dateiende.
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::string exitReason;
    auto finish = [&](const std::string& reason) {
        std::lock_guard<std::mutex> lk(m);
        if (!done) { done = true; exitReason = reason; }
        cv.notify_all();
    };
    core.setFileEndedHandler([&] { finish("file_ended"); });

    if (!args.file.empty()) {
        core.handle({{"type", "open_device"},
                     {"source", {{"kind", "file"}, {"path", args.file}, {"loop", args.loop}, {"fast", args.fast}}}});
    }

    // Kommandothread: liest stdin, bis EOF oder shutdown.
    std::mutex coreM;
    std::thread reader([&] {
        dabcore::ipc::CommandReader r(std::cin, writer.sink());
        dabcore::json cmd;
        while (r.next(cmd)) {
            std::lock_guard<std::mutex> lk(coreM);
            if (!core.handle(cmd)) { finish("shutdown"); return; }
        }
        if (std::getenv("DABCORE_TRACE")) std::fprintf(stderr, "dabcored: stdin EOF\n");
        finish("stdin");
    });

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return done; });
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
        }
    }
    if (trace) std::fprintf(stderr, "dabcored: close_device fertig, writer.close\n");
    writer.close();
    out->flush();
    if (trace) std::fprintf(stderr, "dabcored: writer geschlossen\n");
    if (exitReason == "shutdown" || exitReason == "stdin") {
        reader.join();
        return 0;
    }
    // Der Kommandothread haengt noch in getline(stdin); Prozess direkt beenden.
    std::fflush(stdout);
    std::_Exit(0);
}
