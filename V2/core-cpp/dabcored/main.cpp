// DAB Classic – dabcored: Kernprozess.
//
//   dabcored [--no-audio] [--events DATEI] [--fast] [--duration S]
//            [--file DATEI [--loop] [--service NAME] [--wav DATEI]]
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

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

struct Args {
    bool audio = true;
    std::string eventsFile;
    std::string file;
    std::string service;
    std::string wav;
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
        else if (s == "--service") val(a.service);
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
        std::puts("dabcored [--no-audio] [--events DATEI] [--fast] [--duration S] "
                  "[--file DATEI [--loop] [--service NAME] [--wav DATEI]]");
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
        // --service/--wav werden mit dem MSC-Pfad (M0) wirksam.
        if (!args.service.empty())
            writer.push(dabcore::events::log("info", "--service " + args.service + " (MSC folgt)"));
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
