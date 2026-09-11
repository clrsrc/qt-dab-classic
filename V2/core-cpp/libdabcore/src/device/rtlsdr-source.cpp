// DAB Classic v3: portiert aus Qt-DAB devices/rtlsdr-handler/rtlsdr-handler.cpp
// (Jan van Katwijk, GPLv2+), Qt entfernt – siehe rtlsdr-source.h.
#
/*
 *    Copyright (C) 2014 .. 2025
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *    Lazy Chair Computing
 *
 *    This file is part of the Qt-DAB
 *
 *    Qt-DAB is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    Qt-DAB is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with Qt-SDR; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
// windows.h vor dab-constants.h ("using namespace std" dort macht std::byte
// mit dem RPC-byte der SDK-Header mehrdeutig).
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "rtlsdr-source.h"
#include "dab-constants.h"
#include "librtlsdr/rtl-sdr.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define LOADLIB(name)        reinterpret_cast<void*>(LoadLibraryA(name))
#define GETPROC(lib, name)   reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name))
#define FREELIB(lib)         FreeLibrary(reinterpret_cast<HMODULE>(lib))
#else
#include <dlfcn.h>
#define LOADLIB(name)        dlopen(name, RTLD_NOW)
#define GETPROC(lib, name)   dlsym(lib, name)
#define FREELIB(lib)         dlclose(lib)
#endif

#define READLEN_DEFAULT (4 * 8192)
#define DEFAULT_BANDWIDTH_KHZ 1750

// This is the user-side call back function; ctx is the calling task
static void RTLSDRCallBack(uint8_t* buf, uint32_t len, void* ctx) {
    RtlSdrSource* theStick = static_cast<RtlSdrSource*>(ctx);
    if ((theStick == nullptr) || (len != READLEN_DEFAULT)) {
        std::fprintf(stderr, "%d \n", len);
        return;
    }
    if (!theStick->isActive.load()) return;
    theStick->processBuffer(buf, len);
}

RtlSdrSource::RtlSdrSource() : _I_Buffer(8 * 1024 * 1024) {
    for (int i = 0; i < 256; i++) convTable_[i] = (i - 127.38f) / 128.0f;
    lastFrequency_ = 220000000;
}

RtlSdrSource::~RtlSdrSource() {
    RtlSdrSource::stop();
    RtlSdrSource::stopDump();
    if (workerRunning_.load() || worker_.joinable()) {
        cancelling_.store(true);
        if (rtlsdr_cancel_async && theDevice_) rtlsdr_cancel_async(theDevice_);
        if (worker_.joinable()) worker_.join();
        _I_Buffer.FlushRingBuffer();
    }
    if (theDevice_ != nullptr && rtlsdr_close) rtlsdr_close(theDevice_);
    closeLibrary();
}

void RtlSdrSource::closeLibrary() {
    if (library_ != nullptr) { FREELIB(library_); library_ = nullptr; }
}

bool RtlSdrSource::open(int index, std::string& error) {
#ifdef _WIN32
    const char* candidates[] = {"rtlsdr.dll", "librtlsdr.dll", "librtlsdr-0.dll"};
#elif __APPLE__
    const char* candidates[] = {"librtlsdr.dylib", "librtlsdr.0.dylib"};
#else
    const char* candidates[] = {"librtlsdr.so.0", "librtlsdr.so"};
#endif
    for (const char* c : candidates) {
        library_ = LOADLIB(c);
        if (library_ != nullptr) { libraryString_ = c; break; }
    }
    if (library_ == nullptr) {
        error = "keine RTL-SDR-Bibliothek gefunden (rtlsdr.dll / librtlsdr.dll neben dabcored.exe)";
        return false;
    }
    if (!loadRtlFunctions()) { closeLibrary(); error = "Funktionen fehlen in " + libraryString_; return false; }

    uint32_t deviceCount = rtlsdr_get_device_count();
    if (deviceCount == 0) { error = "kein RTL-SDR-Stick gefunden"; closeLibrary(); return false; }
    if (index < 0 || static_cast<uint32_t>(index) >= deviceCount) {
        error = "RTL-SDR-Index " + std::to_string(index) + " ausserhalb 0.." + std::to_string(deviceCount - 1);
        closeLibrary();
        return false;
    }
    int r = rtlsdr_open(&theDevice_, static_cast<uint32_t>(index));
    if (r < 0) { error = "RTL-SDR " + std::to_string(index) + " laesst sich nicht oeffnen"; theDevice_ = nullptr; closeLibrary(); return false; }

    r = rtlsdr_set_sample_rate(theDevice_, SAMPLERATE);
    if (r < 0) { error = "RTL-SDR: Samplerate nicht setzbar"; return false; }

    int gainsCount = rtlsdr_get_tuner_gains(theDevice_, nullptr);
    if (gainsCount > 0) {
        gains_.assign(gainsCount, 0);
        gainsCount = rtlsdr_get_tuner_gains(theDevice_, gains_.data());
        gains_.resize(gainsCount > 0 ? gainsCount : 0);
        std::sort(gains_.begin(), gains_.end());
    }
    tunerType_ = tunerTypeName(rtlsdr_get_tuner_type(theDevice_));
    deviceModel_ = rtlsdr_get_device_name(index);
    if (rtlsdr_set_tuner_bandwidth != nullptr)
        rtlsdr_set_tuner_bandwidth(theDevice_, KHz(DEFAULT_BANDWIDTH_KHZ));

    // v1-Default: Gain-Eintrag gainsCount/4 von oben in der absteigenden Liste
    if (!gains_.empty()) {
        int fromTop = static_cast<int>(gains_.size()) > 3 ? static_cast<int>(gains_.size()) / 4 : 0;
        gainIndex_ = static_cast<int>(gains_.size()) - 1 - fromTop;
        gain_.lna = gains_[gainIndex_];
    }
    // v1-Default agcMode = 1 ("agc off"): manueller Tuner-Gain
    rtlsdr_set_agc_mode(theDevice_, 0);
    rtlsdr_set_tuner_gain_mode(theDevice_, 1);
    rtlsdr_set_center_freq(theDevice_, 220000000);

    char manufac[256] = {0}, product[256] = {0}, serial[256] = {0};
    rtlsdr_get_usb_strings(theDevice_, manufac, product, serial);
    serial_ = serial;
    applyTunerGain();
    setPpm(ppm_);
    std::fprintf(stderr, "RTL-SDR: %s %s %s, Tuner %s, %zu Gain-Stufen\n",
                 manufac, product, serial, tunerType_.c_str(), gains_.size());
    return true;
}

bool RtlSdrSource::applyTunerGain() {
    if (theDevice_ == nullptr || gains_.empty()) return false;
    int res = rtlsdr_set_tuner_gain(theDevice_, gains_[gainIndex_]);
    if (res != 0) std::fprintf(stderr, "RTL-SDR: Gain %d nicht setzbar\n", gains_[gainIndex_]);
    return res == 0;
}

// lna = Tuner-Gain in 0,1 dB; naechster Tabellenwert wird genommen
DeviceGain RtlSdrSource::setGain(const DeviceGain& g) {
    if (!gains_.empty()) {
        int best = 0;
        for (size_t i = 0; i < gains_.size(); i++)
            if (std::abs(gains_[i] - g.lna) < std::abs(gains_[best] - g.lna)) best = static_cast<int>(i);
        gainIndex_ = best;
        gain_.lna = gains_[best];
        applyTunerGain();
    }
    gain_.vga = 0;
    gain_.amp = false;
    return gain_;
}

// SNR-Nachfuehrung: eine Tabellenstufe hoch (SNR < 8) oder runter (SNR > 18)
bool RtlSdrSource::adjustGain(float snr) {
    if (!isActive.load() || gains_.empty()) return false;
    int idx = gainIndex_;
    if (snr < 8.0f && idx < static_cast<int>(gains_.size()) - 1) idx++;
    else if (snr > 18.0f && idx > 0) idx--;
    if (idx == gainIndex_) return false;
    gainIndex_ = idx;
    gain_.lna = gains_[idx];
    applyTunerGain();
    return true;
}

// correction is in ppm
void RtlSdrSource::setPpm(int ppm) {
    ppm_ = ppm;
    if (theDevice_ == nullptr) return;
    int res;
    if (rtlsdr_set_freq_correction_ppb != nullptr)
        res = rtlsdr_set_freq_correction_ppb(theDevice_, ppm * 1000);
    else if (rtlsdr_set_freq_correction != nullptr)
        res = rtlsdr_set_freq_correction(theDevice_, ppm);
    else
        return;
    if (res != 0 && ppm != 0) std::fprintf(stderr, "RTL-SDR: ppm %d nicht setzbar\n", ppm);
}

// for handling the events in libusb, we need a controlthread
// whose sole purpose is to process the rtlsdr_read_async function
void RtlSdrSource::asyncThread() {
    workerRunning_.store(true);
    rtlsdr_read_async(theDevice_, (rtlsdr_read_async_cb_t)&RTLSDRCallBack, (void*)this, 0, READLEN_DEFAULT);
    workerRunning_.store(false);
    if (!cancelling_.load()) {
        isActive.store(false);
        dataCv_.notify_all();
        reportError("RTL-SDR liefert keine Daten mehr (USB-Verbindung verloren?)");
    }
}

bool RtlSdrSource::restart(int32_t freq, int32_t skipped) {
    if (theDevice_ == nullptr) return false;
    int res = rtlsdr_set_center_freq(theDevice_, static_cast<uint32_t>(freq));
    if (res != 0) { reportError("RTL-SDR: Frequenz " + std::to_string(freq) + " nicht setzbar"); return false; }
    lastFrequency_ = freq;
    toSkip = skipped;
    applyTunerGain();
    if (!worker_.joinable()) {   // usually it will be non zero
        (void)rtlsdr_reset_buffer(theDevice_);
        cancelling_.store(false);
        worker_ = std::thread([this] { asyncThread(); });
    }
    _I_Buffer.FlushRingBuffer();
    isActive.store(true);
    return true;
}

void RtlSdrSource::stop() {
    isActive.store(false);
    _I_Buffer.FlushRingBuffer();
    dataCv_.notify_all();
}

int32_t RtlSdrSource::getSamples(std::complex<float>* V, int32_t size) {
    if (!isActive.load()) return 0;
    return _I_Buffer.getDataFromBuffer(V, size);
}

int32_t RtlSdrSource::samples() {
    if (!isActive.load()) return 0;
    return static_cast<int32_t>(_I_Buffer.GetRingBufferReadAvailable());
}

bool RtlSdrSource::waitForSamples(int32_t n, int timeoutMs) {
    if (samples() >= n) return true;
    std::unique_lock<std::mutex> lk(m_);
    return dataCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                            [this, n] { return samples() >= n || !isActive.load(); })
           && samples() >= n;
}

void RtlSdrSource::resetBuffer() {
    _I_Buffer.FlushRingBuffer();
}

void RtlSdrSource::processBuffer(uint8_t* buf, uint32_t len) {
    int nrSamples = len / 2;
    auto* tempBuf = dynVec(std::complex<float>, nrSamples);
    if (!isActive.load()) return;
    if (toSkip > 0) {
        toSkip -= len / 2;
        return;
    }
    if (dumping_.load()) {
        std::lock_guard<std::mutex> lk(dumpM_);
        if (xmlWriter_) xmlWriter_->add(reinterpret_cast<std::complex<uint8_t>*>(buf), nrSamples);
    }
    for (int32_t i = 0; i < nrSamples; i++)
        tempBuf[i] = std::complex<float>(convTable_[buf[2 * i]], convTable_[buf[2 * i + 1]]);
    int ovf = _I_Buffer.GetRingBufferWriteAvailable() - nrSamples;
    if (ovf < 0)
        (void)_I_Buffer.putDataIntoBuffer(tempBuf, nrSamples + ovf);
    else
        (void)_I_Buffer.putDataIntoBuffer(tempBuf, nrSamples);
    dataCv_.notify_one();
}

bool RtlSdrSource::startDump(const std::string& path, std::string& error) {
    std::lock_guard<std::mutex> lk(dumpM_);
    if (xmlWriter_) { error = "Dump laeuft bereits: " + xmlWriter_->path(); return false; }
    int g = (theDevice_ && rtlsdr_get_tuner_gain) ? rtlsdr_get_tuner_gain(theDevice_) : -1;
    auto w = std::make_unique<XmlFileWriter>(path, 8, "uint8", SAMPLERATE, lastFrequency_, g,
                                             "RTLSDR", deviceModel_, "3.0");
    if (!w->ok()) { error = "kann " + path + " nicht schreiben"; return false; }
    xmlWriter_ = std::move(w);
    dumping_.store(true);
    return true;
}

void RtlSdrSource::stopDump() {
    std::lock_guard<std::mutex> lk(dumpM_);
    dumping_.store(false);
    if (xmlWriter_) { xmlWriter_->computeHeader(); xmlWriter_.reset(); }
}

std::string RtlSdrSource::tunerTypeName(int tunerType) {
    switch (tunerType) {
        case RTLSDR_TUNER_E4000:  return "E4000";
        case RTLSDR_TUNER_FC0012: return "FC0012";
        case RTLSDR_TUNER_FC0013: return "FC0013";
        case RTLSDR_TUNER_FC2580: return "FC2580";
        case RTLSDR_TUNER_R820T:  return "R820T";
        case RTLSDR_TUNER_R828D:  return "R828D";
        default: return "unknown";
    }
}

bool RtlSdrSource::loadRtlFunctions() {
    auto load = [this](const char* name, bool required = true) -> void* {
        void* p = GETPROC(library_, name);
        if (p == nullptr && required) std::fprintf(stderr, "Could not find %s\n", name);
        return p;
    };
    rtlsdr_open = (pfnrtlsdr_open)load("rtlsdr_open");
    rtlsdr_close = (pfnrtlsdr_close)load("rtlsdr_close");
    rtlsdr_get_usb_strings = (pfnrtlsdr_get_usb_strings)load("rtlsdr_get_usb_strings");
    rtlsdr_set_sample_rate = (pfnrtlsdr_set_sample_rate)load("rtlsdr_set_sample_rate");
    rtlsdr_get_sample_rate = (pfnrtlsdr_get_sample_rate)load("rtlsdr_get_sample_rate");
    rtlsdr_get_tuner_gains = (pfnrtlsdr_get_tuner_gains)load("rtlsdr_get_tuner_gains");
    rtlsdr_get_tuner_type = (pfnrtlsdr_get_tuner_type)load("rtlsdr_get_tuner_type");
    rtlsdr_set_tuner_gain_mode = (pfnrtlsdr_set_tuner_gain_mode)load("rtlsdr_set_tuner_gain_mode");
    rtlsdr_set_agc_mode = (pfnrtlsdr_set_agc_mode)load("rtlsdr_set_agc_mode");
    rtlsdr_set_tuner_gain = (pfnrtlsdr_set_tuner_gain)load("rtlsdr_set_tuner_gain");
    rtlsdr_get_tuner_gain = (pfnrtlsdr_get_tuner_gain)load("rtlsdr_get_tuner_gain");
    rtlsdr_set_center_freq = (pfnrtlsdr_set_center_freq)load("rtlsdr_set_center_freq");
    rtlsdr_get_center_freq = (pfnrtlsdr_get_center_freq)load("rtlsdr_get_center_freq");
    rtlsdr_reset_buffer = (pfnrtlsdr_reset_buffer)load("rtlsdr_reset_buffer");
    rtlsdr_read_async = (pfnrtlsdr_read_async)load("rtlsdr_read_async");
    rtlsdr_get_device_count = (pfnrtlsdr_get_device_count)load("rtlsdr_get_device_count");
    rtlsdr_cancel_async = (pfnrtlsdr_cancel_async)load("rtlsdr_cancel_async");
    rtlsdr_set_direct_sampling = (pfnrtlsdr_set_direct_sampling)load("rtlsdr_set_direct_sampling");
    rtlsdr_get_device_name = (pfnrtlsdr_get_device_name)load("rtlsdr_get_device_name");
    // optional (nullpointer is handled)
    rtlsdr_set_freq_correction = (pfnrtlsdr_set_freq_correction)load("rtlsdr_set_freq_correction", false);
    rtlsdr_set_tuner_bandwidth = (pfnrtlsdr_set_tuner_bandwidth)load("rtlsdr_set_tuner_bandwidth", false);
    rtlsdr_set_bias_tee = (pfnrtlsdr_set_bias_tee)load("rtlsdr_set_bias_tee", false);
    rtlsdr_set_freq_correction_ppb = (pfnrtlsdr_set_freq_correction_ppb)load("rtlsdr_set_freq_correction_ppb", false);
    rtlsdr_get_version = (pfnrtlsdr_get_version)load("rtlsdr_get_version", false);

    return rtlsdr_open && rtlsdr_close && rtlsdr_get_usb_strings && rtlsdr_set_sample_rate &&
           rtlsdr_get_sample_rate && rtlsdr_get_tuner_gains && rtlsdr_get_tuner_type &&
           rtlsdr_set_tuner_gain_mode && rtlsdr_set_agc_mode && rtlsdr_set_tuner_gain &&
           rtlsdr_get_tuner_gain && rtlsdr_set_center_freq && rtlsdr_get_center_freq &&
           rtlsdr_reset_buffer && rtlsdr_read_async && rtlsdr_get_device_count &&
           rtlsdr_cancel_async && rtlsdr_set_direct_sampling && rtlsdr_get_device_name;
}
