// DAB Classic v3 – Timeshift-Ring, siehe timeshift-buffer.h.
#include "timeshift-buffer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dabcore {

const char* timeshiftModeName(TimeshiftMode m) {
    switch (m) {
        case TimeshiftMode::Live:    return "live";
        case TimeshiftMode::Paused:  return "paused";
        case TimeshiftMode::Playing: return "playing";
    }
    return "live";
}

// Sekunden -> Rahmen (24 ms), kaufmaennisch gerundet und nie negativ.
static uint64_t framesFromSeconds(double s) {
    if (!(s > 0.0)) return 0;
    double f = s / TimeshiftBuffer::kFrameSeconds;
    if (f > 1e12) f = 1e12;
    return static_cast<uint64_t>(std::llround(f));
}

void TimeshiftBuffer::unpack(const uint8_t* packed, uint32_t frameBits, uint8_t* hardBits) {
    for (uint32_t i = 0; i < frameBits; ++i)
        hardBits[i] = static_cast<uint8_t>((packed[i >> 3] >> (7 - (i & 7))) & 1);
}

uint64_t TimeshiftBuffer::oldestLocked() const {
    return writeIndex_ > capacityFrames_ ? writeIndex_ - capacityFrames_ : 0;
}

void TimeshiftBuffer::reallocLocked() {
    capacityFrames_ = static_cast<uint32_t>(static_cast<uint64_t>(capacityS_) * 1000 / 24);
    packedBytes_ = (frameBits_ + 7) / 8;
    writeIndex_ = readIndex_ = 0;
    liveUnix_ = 0;
    mode_ = TimeshiftMode::Live;
    if (capacityFrames_ == 0 || packedBytes_ == 0) {
        data_.clear(); data_.shrink_to_fit();
        stamps_.clear(); stamps_.shrink_to_fit();
        return;
    }
    data_.assign(static_cast<size_t>(capacityFrames_) * packedBytes_, 0);
    stamps_.assign(capacityFrames_, 0);
}

bool TimeshiftBuffer::setCapacitySeconds(uint32_t capacityS) {
    std::lock_guard<std::mutex> lk(m_);
    if (capacityS == capacityS_) return false;
    capacityS_ = capacityS;
    reallocLocked();
    return true;
}

void TimeshiftBuffer::setFrameBits(uint32_t frameBits) {
    std::lock_guard<std::mutex> lk(m_);
    frameBits_ = frameBits;
    reallocLocked();
}

void TimeshiftBuffer::clear() {
    std::lock_guard<std::mutex> lk(m_);
    writeIndex_ = readIndex_ = 0;
    liveUnix_ = 0;
    mode_ = TimeshiftMode::Live;
}

bool TimeshiftBuffer::active() const {
    std::lock_guard<std::mutex> lk(m_);
    return frameBits_ > 0 && capacityFrames_ > 0;
}

uint32_t TimeshiftBuffer::frameBits() const { std::lock_guard<std::mutex> lk(m_); return frameBits_; }
uint32_t TimeshiftBuffer::packedBytes() const { std::lock_guard<std::mutex> lk(m_); return packedBytes_; }
uint32_t TimeshiftBuffer::capacityFrames() const { std::lock_guard<std::mutex> lk(m_); return capacityFrames_; }
uint32_t TimeshiftBuffer::capacitySeconds() const { std::lock_guard<std::mutex> lk(m_); return capacityS_; }

uint64_t TimeshiftBuffer::capacityBytes() const {
    std::lock_guard<std::mutex> lk(m_);
    return static_cast<uint64_t>(capacityFrames_) * packedBytes_;
}

uint64_t TimeshiftBuffer::size() const {
    std::lock_guard<std::mutex> lk(m_);
    return writeIndex_ - oldestLocked();
}

uint64_t TimeshiftBuffer::writeIndex() const { std::lock_guard<std::mutex> lk(m_); return writeIndex_; }
uint64_t TimeshiftBuffer::readIndex() const { std::lock_guard<std::mutex> lk(m_); return readIndex_; }
int64_t TimeshiftBuffer::liveUnix() const { std::lock_guard<std::mutex> lk(m_); return liveUnix_; }

double TimeshiftBuffer::bufferedSeconds() const {
    std::lock_guard<std::mutex> lk(m_);
    return static_cast<double>(writeIndex_ - oldestLocked()) * kFrameSeconds;
}

double TimeshiftBuffer::offsetSeconds() const {
    std::lock_guard<std::mutex> lk(m_);
    return static_cast<double>(writeIndex_ - readIndex_) * kFrameSeconds;
}

TimeshiftMode TimeshiftBuffer::mode() const { std::lock_guard<std::mutex> lk(m_); return mode_; }
void TimeshiftBuffer::setMode(TimeshiftMode m) { std::lock_guard<std::mutex> lk(m_); mode_ = m; }

void TimeshiftBuffer::push(const uint8_t* hardBits, uint32_t n, int64_t unixUtc) {
    std::lock_guard<std::mutex> lk(m_);
    if (frameBits_ == 0 || capacityFrames_ == 0 || n != frameBits_) return;
    const size_t slot = static_cast<size_t>(writeIndex_ % capacityFrames_);
    uint8_t* dst = data_.data() + slot * packedBytes_;
    std::memset(dst, 0, packedBytes_);
    for (uint32_t i = 0; i < frameBits_; ++i)
        if (hardBits[i] & 1) dst[i >> 3] |= static_cast<uint8_t>(0x80u >> (i & 7));
    stamps_[slot] = unixUtc;
    ++writeIndex_;
    liveUnix_ = unixUtc;
    // live: der Lesezeiger laeuft mit dem Schreibzeiger mit (offset_s = 0)
    if (mode_ == TimeshiftMode::Live) readIndex_ = writeIndex_;
    // Ueberlauf: der aelteste Rahmen faellt weg; steht der Lesezeiger dort,
    // rueckt er mit (offset_s bleibt dann bei der Kapazitaet stehen).
    const uint64_t oldest = oldestLocked();
    if (readIndex_ < oldest) readIndex_ = oldest;
}

bool TimeshiftBuffer::pop(std::vector<uint8_t>& hardBits, int64_t* unixUtc) {
    std::lock_guard<std::mutex> lk(m_);
    if (frameBits_ == 0 || capacityFrames_ == 0) return false;
    if (readIndex_ >= writeIndex_) return false;
    const size_t slot = static_cast<size_t>(readIndex_ % capacityFrames_);
    hardBits.resize(frameBits_);
    unpack(data_.data() + slot * packedBytes_, frameBits_, hardBits.data());
    if (unixUtc) *unixUtc = stamps_[slot];
    ++readIndex_;
    return true;
}

double TimeshiftBuffer::seekSeconds(double offsetS) {
    std::lock_guard<std::mutex> lk(m_);
    if (capacityFrames_ == 0) return 0.0;
    const uint64_t oldest = oldestLocked();
    uint64_t back = framesFromSeconds(offsetS);
    if (back > writeIndex_ - oldest) back = writeIndex_ - oldest;
    readIndex_ = writeIndex_ - back;
    return static_cast<double>(back) * kFrameSeconds;
}

double TimeshiftBuffer::skipSeconds(double deltaS) {
    std::lock_guard<std::mutex> lk(m_);
    if (capacityFrames_ == 0) return 0.0;
    const uint64_t oldest = oldestLocked();
    // + geht Richtung live (Versatz kleiner), - zurueck in die Vergangenheit
    double target = static_cast<double>(writeIndex_ - readIndex_) * kFrameSeconds - deltaS;
    uint64_t back = framesFromSeconds(target);
    if (back > writeIndex_ - oldest) back = writeIndex_ - oldest;
    readIndex_ = writeIndex_ - back;
    return static_cast<double>(back) * kFrameSeconds;
}

void TimeshiftBuffer::toLive() {
    std::lock_guard<std::mutex> lk(m_);
    readIndex_ = writeIndex_;
    mode_ = TimeshiftMode::Live;
}

uint64_t TimeshiftBuffer::copyRange(double fromS, double toS, std::vector<uint8_t>& packed,
                                    int64_t* firstUnix) const {
    packed.clear();
    if (firstUnix) *firstUnix = 0;
    std::lock_guard<std::mutex> lk(m_);
    if (capacityFrames_ == 0 || packedBytes_ == 0) return 0;
    if (!(fromS > toS)) return 0;
    const uint64_t oldest = oldestLocked();
    uint64_t backFrom = framesFromSeconds(fromS);
    uint64_t backTo = framesFromSeconds(toS);
    if (backFrom > writeIndex_ - oldest) backFrom = writeIndex_ - oldest;
    if (backTo > backFrom) backTo = backFrom;
    const uint64_t begin = writeIndex_ - backFrom;
    const uint64_t end = writeIndex_ - backTo;
    if (end <= begin) return 0;
    const uint64_t count = end - begin;
    packed.resize(static_cast<size_t>(count) * packedBytes_);
    for (uint64_t i = 0; i < count; ++i) {
        const size_t slot = static_cast<size_t>((begin + i) % capacityFrames_);
        std::memcpy(packed.data() + static_cast<size_t>(i) * packedBytes_,
                    data_.data() + slot * packedBytes_, packedBytes_);
    }
    if (firstUnix) *firstUnix = stamps_[static_cast<size_t>(begin % capacityFrames_)];
    return count;
}

} // namespace dabcore
