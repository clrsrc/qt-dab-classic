// DAB Classic v3: portiert aus Qt-DAB devices/rtlsdr-handler/rtlsdr-handler.{h,cpp}
// (Jan van Katwijk, GPLv2+), Qt entfernt: QLibrary -> LoadLibrary
// ("rtlsdr.dll", ersatzweise "librtlsdr.dll"), dll_driver (QThread) ->
// std::thread um rtlsdr_read_async, Widget/QSettings gestrichen. Unveraendert:
// 2,048 MS/s nativ, uint8 -> float ueber convTable (x - 127.38) / 128,
// toSkip nach dem Umschalten, Tuner-Gain-Tabelle, manueller Gain-Modus
// (v1-Default "agc off"), ppm-Korrektur, Bandbreite 1750 kHz.
// Gain.lna = Tuner-Gain in 0,1 dB (naechster Tabellenwert), vga/amp ohne
// Bedeutung; adjustGain schaltet eine Tabellenstufe hoch/runter.
// Bias-T entfaellt.
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
 *
 *	This particular driver is a very simple wrapper around the
 *	librtlsdr.  In order to keep things simple, we dynamically
 *	load the dll (or .so). The librtlsdr is osmocom software and all rights
 *	are greatly acknowledged
 */
#pragma once

#include "isample-source.h"
#include "ringbuffer.h"
#include "xml-file-writer.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

typedef struct rtlsdr_dev rtlsdr_dev_t;
extern "C" {
typedef void (*rtlsdr_read_async_cb_t)(uint8_t* buf, uint32_t len, void* ctx);
typedef int (*pfnrtlsdr_open)(rtlsdr_dev_t**, uint32_t);
typedef int (*pfnrtlsdr_close)(rtlsdr_dev_t*);
typedef int (*pfnrtlsdr_get_usb_strings)(rtlsdr_dev_t*, char*, char*, char*);
typedef int (*pfnrtlsdr_set_center_freq)(rtlsdr_dev_t*, uint32_t);
typedef int (*pfnrtlsdr_set_tuner_bandwidth)(rtlsdr_dev_t*, uint32_t);
typedef uint32_t (*pfnrtlsdr_get_center_freq)(rtlsdr_dev_t*);
typedef int (*pfnrtlsdr_get_tuner_gains)(rtlsdr_dev_t*, int*);
typedef int (*pfnrtlsdr_get_tuner_type)(rtlsdr_dev_t*);
typedef int (*pfnrtlsdr_set_tuner_gain_mode)(rtlsdr_dev_t*, int);
typedef int (*pfnrtlsdr_set_agc_mode)(rtlsdr_dev_t*, int);
typedef int (*pfnrtlsdr_set_sample_rate)(rtlsdr_dev_t*, uint32_t);
typedef int (*pfnrtlsdr_get_sample_rate)(rtlsdr_dev_t*);
typedef int (*pfnrtlsdr_set_tuner_gain)(rtlsdr_dev_t*, int);
typedef int (*pfnrtlsdr_get_tuner_gain)(rtlsdr_dev_t*);
typedef int (*pfnrtlsdr_reset_buffer)(rtlsdr_dev_t*);
typedef int (*pfnrtlsdr_read_async)(rtlsdr_dev_t*, rtlsdr_read_async_cb_t, void*, uint32_t, uint32_t);
typedef int (*pfnrtlsdr_set_bias_tee)(rtlsdr_dev_t*, int);
typedef int (*pfnrtlsdr_cancel_async)(rtlsdr_dev_t*);
typedef int (*pfnrtlsdr_set_direct_sampling)(rtlsdr_dev_t*, int);
typedef uint32_t (*pfnrtlsdr_get_device_count)();
typedef int (*pfnrtlsdr_set_freq_correction)(rtlsdr_dev_t*, int);
typedef int (*pfnrtlsdr_set_freq_correction_ppb)(rtlsdr_dev_t*, int);
typedef char* (*pfnrtlsdr_get_device_name)(int);
typedef int (*pfnrtlsdr_get_version)();
}

class RtlSdrSource : public ISampleSource {
public:
    RtlSdrSource();
    ~RtlSdrSource() override;

    // Laedt die Bibliothek und oeffnet den Stick mit dem Index.
    bool open(int index, std::string& error);

    bool restart(int32_t frequencyHz, int32_t samplesToSkip = 0) override;
    void stop() override;
    int32_t getSamples(std::complex<float>* buffer, int32_t n) override;
    int32_t samples() override;
    bool waitForSamples(int32_t n, int timeoutMs) override;
    void resetBuffer() override;

    int16_t bitDepth() const override { return 8; }
    std::string name() const override { return "rtlsdr"; }
    std::string serial() const override { return serial_; }
    std::string model() const { return deviceModel_; }
    std::string tunerType() const { return tunerType_; }
    const std::vector<int>& gainTable() const { return gains_; }

    DeviceGain setGain(const DeviceGain& g) override;
    bool adjustGain(float snr) override;
    void setPpm(int ppm) override;

    bool startDump(const std::string& path, std::string& error) override;
    void stopDump() override;
    bool dumping() const override { return dumping_.load(); }

    // vom Callback (libusb-Thread) benutzt
    void processBuffer(uint8_t* buf, uint32_t len);
    std::atomic<bool> isActive{false};
    std::atomic<int> toSkip{0};

private:
    bool loadRtlFunctions();
    void closeLibrary();
    void asyncThread();
    bool applyTunerGain();
    static std::string tunerTypeName(int t);

    void* library_ = nullptr;
    rtlsdr_dev_t* theDevice_ = nullptr;
    std::thread worker_;
    std::atomic<bool> workerRunning_{false};
    std::atomic<bool> cancelling_{false};
    RingBuffer<std::complex<float>> _I_Buffer;
    std::mutex m_;
    std::condition_variable dataCv_;
    float convTable_[256];
    std::vector<int> gains_;    // aufsteigend, 0,1 dB
    int gainIndex_ = 0;
    std::string serial_, deviceModel_, tunerType_, libraryString_;

    std::mutex dumpM_;
    std::unique_ptr<XmlFileWriter> xmlWriter_;
    std::atomic<bool> dumping_{false};

    pfnrtlsdr_open rtlsdr_open = nullptr;
    pfnrtlsdr_close rtlsdr_close = nullptr;
    pfnrtlsdr_get_usb_strings rtlsdr_get_usb_strings = nullptr;
    pfnrtlsdr_set_center_freq rtlsdr_set_center_freq = nullptr;
    pfnrtlsdr_set_tuner_bandwidth rtlsdr_set_tuner_bandwidth = nullptr;
    pfnrtlsdr_get_center_freq rtlsdr_get_center_freq = nullptr;
    pfnrtlsdr_get_tuner_gains rtlsdr_get_tuner_gains = nullptr;
    pfnrtlsdr_get_tuner_type rtlsdr_get_tuner_type = nullptr;
    pfnrtlsdr_set_tuner_gain_mode rtlsdr_set_tuner_gain_mode = nullptr;
    pfnrtlsdr_set_agc_mode rtlsdr_set_agc_mode = nullptr;
    pfnrtlsdr_set_sample_rate rtlsdr_set_sample_rate = nullptr;
    pfnrtlsdr_get_sample_rate rtlsdr_get_sample_rate = nullptr;
    pfnrtlsdr_set_tuner_gain rtlsdr_set_tuner_gain = nullptr;
    pfnrtlsdr_get_tuner_gain rtlsdr_get_tuner_gain = nullptr;
    pfnrtlsdr_reset_buffer rtlsdr_reset_buffer = nullptr;
    pfnrtlsdr_read_async rtlsdr_read_async = nullptr;
    pfnrtlsdr_cancel_async rtlsdr_cancel_async = nullptr;
    pfnrtlsdr_set_bias_tee rtlsdr_set_bias_tee = nullptr;
    pfnrtlsdr_set_direct_sampling rtlsdr_set_direct_sampling = nullptr;
    pfnrtlsdr_get_device_count rtlsdr_get_device_count = nullptr;
    pfnrtlsdr_set_freq_correction rtlsdr_set_freq_correction = nullptr;
    pfnrtlsdr_set_freq_correction_ppb rtlsdr_set_freq_correction_ppb = nullptr;
    pfnrtlsdr_get_device_name rtlsdr_get_device_name = nullptr;
    pfnrtlsdr_get_version rtlsdr_get_version = nullptr;
};
