// DAB Classic v3 – Standardimplementierung des Wartens auf Samples.
#include "isample-source.h"

#include <chrono>
#include <thread>

bool ISampleSource::waitForSamples(int32_t n, int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (samples() < n) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}
