// DAB Classic v3 – Kanaltabelle Band III, siehe dab-channels.h.
#include "dab-channels.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

const std::vector<DabChannel>& bandIIIChannels() {
    static const std::vector<DabChannel> table = {
        {"5A", 174928}, {"5B", 176640}, {"5C", 178352}, {"5D", 180064},
        {"6A", 181936}, {"6B", 183648}, {"6C", 185360}, {"6D", 187072},
        {"7A", 188928}, {"7B", 190640}, {"7C", 192352}, {"7D", 194064},
        {"8A", 195936}, {"8B", 197648}, {"8C", 199360}, {"8D", 201072},
        {"9A", 202928}, {"9B", 204640}, {"9C", 206352}, {"9D", 208064},
        {"10A", 209936}, {"10B", 211648}, {"10C", 213360}, {"10D", 215072},
        {"11A", 216928}, {"11B", 218640}, {"11C", 220352}, {"11D", 222064},
        {"12A", 223936}, {"12B", 225648}, {"12C", 227360}, {"12D", 229072},
        {"13A", 230784}, {"13B", 232496}, {"13C", 234208}, {"13D", 235776},
        {"13E", 237488}, {"13F", 239200},
    };
    return table;
}

int32_t channelFrequencyHz(const std::string& channel) {
    std::string c = channel;
    while (!c.empty() && std::isspace(static_cast<unsigned char>(c.back()))) c.pop_back();
    std::transform(c.begin(), c.end(), c.begin(), ::toupper);
    for (auto& ch : bandIIIChannels())
        if (c == ch.name) return ch.kHz * 1000;
    return 0;
}

std::string channelForFrequency(int32_t hz) {
    for (auto& ch : bandIIIChannels())
        if (std::abs(hz / 1000 - ch.kHz) < 100) return ch.name;
    return std::string();
}
