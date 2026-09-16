// DAB Classic v3: portiert aus Qt-DAB devices/hackrf-handler/hackrf-handler.{h,cpp}
// (Jan van Katwijk, Fabio Capozzi; GPLv2+), Qt entfernt: QLibrary ->
// LoadLibrary/GetProcAddress mit derselben Funktionstabelle, Widget/
// QSettings gestrichen (Gain kommt ueber setGain, Persistenz liegt in der
// App), Warten per Condition-Variable statt usleep-Polling. Unveraendert:
// 4,096 MS/s mit Boxcar-Mittelung 2:1 im Callback, Bandbreite 1,536 MHz,
// toSkip nach dem Umschalten, Normierung /128, AMP-Schalter, ppm-Korrektur
// ueber die Frequenz. Die v1-Nachfuehrung adjustGain (SNR < 8: VGA+2,
// SNR > 18: VGA-2) ist durch den AgcController ersetzt (setGainStep).
#
/*
 *    Copyright (C) 2014 .. 2025
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *    Lazy Chair Computing
 *
 *    Copyright (C) 2019 Amplifier, antenna and ppm correctors
 *    Fabio Capozzi
 *
 *    This file is part of Qt-DAB
 *
 *    Qt-DAB is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation version 2 of the License.
 *
 *    Qt-DAB is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with Qt-DAB if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#pragma once

#include "isample-source.h"
#include "ringbuffer.h"
#include "xml-file-writer.h"
#include "libhackrf/hackrf.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

typedef int (*hackrf_sample_block_cb_fn)(hackrf_transfer* transfer);

// Dll-Funktionsprototypen (wie v1)
typedef int (*pfn_hackrf_init)();
typedef int (*pfn_hackrf_open)(hackrf_device** device);
typedef int (*pfn_hackrf_open_by_serial)(const char* serial, hackrf_device** device);
typedef int (*pfn_hackrf_close)(hackrf_device* device);
typedef int (*pfn_hackrf_exit)();
typedef int (*pfn_hackrf_start_rx)(hackrf_device*, hackrf_sample_block_cb_fn, void*);
typedef int (*pfn_hackrf_stop_rx)(hackrf_device*);
typedef hackrf_device_list_t* (*pfn_hackrf_device_list)();
typedef void (*pfn_hackrf_device_list_free)(hackrf_device_list_t*);
typedef int (*pfn_hackrf_set_baseband_filter_bandwidth)(hackrf_device*, const uint32_t bandwidth_hz);
typedef int (*pfn_hackrf_set_lna_gain)(hackrf_device*, uint32_t);
typedef int (*pfn_hackrf_set_vga_gain)(hackrf_device*, uint32_t);
typedef int (*pfn_hackrf_set_freq)(hackrf_device*, const uint64_t);
typedef int (*pfn_hackrf_set_sample_rate)(hackrf_device*, const double freq_hz);
typedef int (*pfn_hackrf_is_streaming)(hackrf_device*);
typedef const char* (*pfn_hackrf_error_name)(enum hackrf_error errcode);
typedef const char* (*pfn_hackrf_usb_board_id_name)(enum hackrf_usb_board_id);
typedef int (*pfn_hackrf_version_string_read)(hackrf_device*, char*, int);
typedef const char* (*pfn_hackrf_library_version)();
typedef const char* (*pfn_hackrf_library_release)();
typedef int (*pfn_hackrf_set_antenna_enable)(hackrf_device*, const uint8_t);
typedef int (*pfn_hackrf_set_amp_enable)(hackrf_device*, const uint8_t);
typedef int (*pfn_hackrf_si5351c_read)(hackrf_device*, const uint16_t, uint16_t*);
typedef int (*pfn_hackrf_si5351c_write)(hackrf_device*, const uint16_t, const uint16_t);
typedef int (*pfn_hackrf_board_rev_read)(hackrf_device* device, uint8_t* value);
typedef int (*pfn_hackrf_board_partid_serialno_read)(hackrf_device* device, read_partid_serialno_t* out);
typedef int (*pfn_hackrf_get_clkin_status)(hackrf_device* device, uint8_t* status);

class HackRfSource : public ISampleSource {
public:
    HackRfSource();
    ~HackRfSource() override;

    // Laedt libhackrf.dll und oeffnet das Geraet (serial leer = erstes).
    // Gain-Defaults wie v1: LNA 40, VGA 24, AMP aus.
    bool open(const std::string& serial, std::string& error);

    bool restart(int32_t frequencyHz, int32_t samplesToSkip = 0) override;
    void stop() override;
    int32_t getSamples(std::complex<float>* buffer, int32_t n) override;
    int32_t samples() override;
    bool waitForSamples(int32_t n, int timeoutMs) override;
    void resetBuffer() override;

    int16_t bitDepth() const override { return 8; }
    std::string name() const override { return "hackrf"; }
    std::string serial() const override { return serialNumber_; }
    std::string clockSource() const override { return clockSource_; }

    DeviceGain setGain(const DeviceGain& g) override;
    bool hasAmp() const override { return true; }
    // AGC-Stufen: VGA 0..62 in 2er-Schritten -> Stufe 0..31; AMP getrennt
    int gainStepCount() const override { return 32; }
    int gainStep() const override { std::lock_guard<std::mutex> lk(gainM_); return gain_.vga / 2; }
    int gainAcqIncrement() const override { return 4; }     // VGA +8
    int gainDefaultStep() const override { return 20; }     // VGA 40
    // VGA +-4 je Probe: die SNR-EMA des ofdmHandlers (0,85) daempft kleine
    // Schritte, +-2 laege unter der Nachweisschwelle (0,25 dB)
    int gainTrackStep() const override { return 2; }
    void setGainStep(int step, bool amp) override;
    void setPpm(int ppm) override;

    bool startDump(const std::string& path, std::string& error) override;
    void stopDump() override;
    bool dumping() const override { return dumping_.load(); }

    std::string libraryVersion() const { return libraryVersion_; }
    std::string boardInfo() const { return boardInfo_; }

    // vom Callback benutzt
    RingBuffer<std::complex<int8_t>> _I_Buffer;
    std::atomic<int> toSkip{0};
    void onCallbackData();

private:
    bool loadHackrfFunctions();
    void closeLibrary();
    std::string errName(int code) const;
    bool applyGain();

    void* library_ = nullptr;
    hackrf_device* theDevice_ = nullptr;
    bool inited_ = false;
    std::atomic<bool> running_{false};
    std::string serialNumber_;
    std::string libraryVersion_;
    std::string boardInfo_;
    bool antennaEnable_ = false;   // Bias-T bleibt aus (v1-Default)

    std::mutex m_;
    std::condition_variable dataCv_;
    std::atomic<bool> errorReported_{false};

    std::mutex dumpM_;
    std::unique_ptr<XmlFileWriter> xmlWriter_;
    std::atomic<bool> dumping_{false};
    std::vector<std::complex<int8_t>> temp_;

    pfn_hackrf_init hackrf_init = nullptr;
    pfn_hackrf_open hackrf_open = nullptr;
    pfn_hackrf_open_by_serial hackrf_open_by_serial = nullptr;
    pfn_hackrf_close hackrf_close = nullptr;
    pfn_hackrf_exit hackrf_exit = nullptr;
    pfn_hackrf_start_rx hackrf_start_rx = nullptr;
    pfn_hackrf_stop_rx hackrf_stop_rx = nullptr;
    pfn_hackrf_device_list hackrf_device_list = nullptr;
    pfn_hackrf_device_list_free hackrf_device_list_free = nullptr;
    pfn_hackrf_set_baseband_filter_bandwidth hackrf_baseband_filter = nullptr;
    pfn_hackrf_set_lna_gain hackrf_set_lna_gain = nullptr;
    pfn_hackrf_set_vga_gain hackrf_set_vga_gain = nullptr;
    pfn_hackrf_set_freq hackrf_set_freq = nullptr;
    pfn_hackrf_set_sample_rate hackrf_set_sample_rate = nullptr;
    pfn_hackrf_is_streaming hackrf_is_streaming = nullptr;
    pfn_hackrf_error_name hackrf_error_name = nullptr;
    pfn_hackrf_usb_board_id_name hackrf_usb_board_id_name = nullptr;
    pfn_hackrf_version_string_read hackrf_version_string_read = nullptr;
    pfn_hackrf_set_antenna_enable hackrf_set_antenna_enable = nullptr;
    pfn_hackrf_set_amp_enable hackrf_set_amp_enable = nullptr;
    pfn_hackrf_si5351c_read hackrf_si5351c_read = nullptr;
    pfn_hackrf_si5351c_write hackrf_si5351c_write = nullptr;
    pfn_hackrf_board_rev_read hackrf_board_rev_read = nullptr;
    pfn_hackrf_board_partid_serialno_read hackrf_board_partid_serialno_read = nullptr;
    pfn_hackrf_get_clkin_status hackrf_get_clkin_status = nullptr;   // optional (libhackrf >= 2021.03)
    std::string clockSource_;
    pfn_hackrf_library_version hackrf_library_version = nullptr;
    pfn_hackrf_library_release hackrf_library_release = nullptr;
};
