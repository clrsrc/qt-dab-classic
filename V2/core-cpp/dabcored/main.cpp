// DAB Classic – dabcored: Kernprozess.
//
//   dabcored [--no-audio] [--events DATEI] [--file DATEI [--service NAME] [--wav DATEI] [--duration S]]
//
// Ohne --file: Kommandos von stdin (JSON-Zeilen), Ereignisse auf stdout.
// Mit --file: Headless-Replay; Ereignisse auf stdout (oder --events), stdin
// wird trotzdem gelesen, damit die App eingreifen kann. Der Prozess endet
// bei EOF auf stdin oder beim Kommando {"type":"shutdown"}.

#include "dabcore/core.h"
#include "dabcore/ipc.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

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
        std::puts("dabcored [--no-audio] [--events DATEI] [--file DATEI [--service NAME] [--wav DATEI] [--duration S]]");
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
    dabcore::DabCore core(writer.sink(), opt);

    if (!args.file.empty()) {
        core.handle({{"type", "open_device"},
                     {"source", {{"kind", "file"}, {"path", args.file}, {"loop", false}}}});
        // --service/--wav/--duration werden mit dem Empfangspfad (M0) wirksam.
        if (!args.service.empty())
            writer.push(dabcore::events::log("info", "--service " + args.service + " (M0)"));
    }

    dabcore::ipc::CommandReader reader(std::cin, writer.sink());
    dabcore::json cmd;
    bool shutdownRequested = false;
    while (reader.next(cmd)) {
        if (!core.handle(cmd)) { shutdownRequested = true; break; }
    }
    if (!shutdownRequested) writer.push(dabcore::events::exiting("stdin geschlossen"));
    writer.close();
    return 0;
}
