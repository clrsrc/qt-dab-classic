// DAB Classic – JSON-Zeilen-Protokoll auf stdin/stdout.
#pragma once

#include "dabcore/events.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <iosfwd>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace dabcore::ipc {

// Schreibt Ereignisse als JSON-Zeilen in einen Stream (stdout oder Datei).
// Thread-sicher: alle Kernthreads rufen push() auf, ein eigener Thread
// serialisiert und schreibt. Latest-wins-Ereignisse belegen je Typ genau
// einen Slot; lueckenlose Ereignisse stehen in einer Warteschlange, die bei
// Ueberlauf den Erzeuger kurz blockiert (bei einer Pipe praktisch nie).
class EventWriter {
public:
    explicit EventWriter(std::ostream& out, size_t maxQueue = 4096);
    ~EventWriter();
    EventWriter(const EventWriter&) = delete;
    EventWriter& operator=(const EventWriter&) = delete;

    void push(json event);
    EventSink sink() { return [this](json e) { push(std::move(e)); }; }

    // Schreibt alles Ausstehende und beendet den Thread.
    void close();

    uint64_t written() const { return written_.load(); }
    uint64_t dropped() const { return dropped_.load(); }

private:
    void run();

    std::ostream& out_;
    size_t maxQueue_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<json> queue_;
    std::unordered_map<std::string, json> latest_;
    std::deque<std::string> latestOrder_;
    bool closing_ = false;
    std::atomic<uint64_t> written_{0};
    std::atomic<uint64_t> dropped_{0};
    std::thread thread_;
};

// Liest Kommandos zeilenweise (blockierend) aus einem Stream.
// Gibt false bei EOF zurueck; unlesbare Zeilen werden uebersprungen und
// als "log"-Ereignis gemeldet.
class CommandReader {
public:
    CommandReader(std::istream& in, EventSink sink);
    bool next(json& command);

private:
    std::istream& in_;
    EventSink sink_;
};

} // namespace dabcore::ipc
