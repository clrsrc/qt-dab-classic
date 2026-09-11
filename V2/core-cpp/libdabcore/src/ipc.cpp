#include "dabcore/ipc.h"

#include <iostream>
#include <string>

namespace dabcore::ipc {

EventWriter::EventWriter(std::ostream& out, size_t maxQueue)
    : out_(out), maxQueue_(maxQueue), thread_([this] { run(); }) {}

EventWriter::~EventWriter() { close(); }

void EventWriter::push(json event) {
    std::string type = event.value("type", "");
    std::unique_lock<std::mutex> lk(m_);
    if (closing_) return;
    if (isLatestWins(type)) {
        auto it = latest_.find(type);
        if (it == latest_.end()) {
            latestOrder_.push_back(type);
            latest_.emplace(type, std::move(event));
        } else {
            it->second = std::move(event);
            dropped_.fetch_add(1);
        }
    } else {
        cv_.wait(lk, [this] { return queue_.size() < maxQueue_ || closing_; });
        if (closing_) return;
        queue_.push_back(std::move(event));
    }
    cv_.notify_all();
}

void EventWriter::close() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (closing_) return;
        closing_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void EventWriter::run() {
    std::string line;
    for (;;) {
        std::deque<json> batch;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return !queue_.empty() || !latestOrder_.empty() || closing_; });
            batch.swap(queue_);
            for (auto& t : latestOrder_) {
                auto it = latest_.find(t);
                if (it != latest_.end()) {
                    batch.push_back(std::move(it->second));
                    latest_.erase(it);
                }
            }
            latestOrder_.clear();
            if (batch.empty() && closing_) break;
        }
        cv_.notify_all();
        for (auto& e : batch) {
            line = e.dump();
            line.push_back('\n');
            out_.write(line.data(), static_cast<std::streamsize>(line.size()));
            written_.fetch_add(1);
        }
        out_.flush();
    }
    out_.flush();
}

CommandReader::CommandReader(std::istream& in, EventSink sink) : in_(in), sink_(std::move(sink)) {}

bool CommandReader::next(json& command) {
    std::string line;
    while (std::getline(in_, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        try {
            command = json::parse(line);
            if (!command.is_object() || !command.contains("type")) {
                sink_(events::log("warn", "Kommando ohne type: " + line));
                continue;
            }
            return true;
        } catch (const std::exception& e) {
            sink_(events::log("warn", std::string("Kommando unlesbar: ") + e.what()));
        }
    }
    return false;
}

} // namespace dabcore::ipc
