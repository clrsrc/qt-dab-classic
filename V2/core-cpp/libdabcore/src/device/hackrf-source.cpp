// DAB Classic v3: portiert aus Qt-DAB devices/hackrf-handler/hackrf-handler.cpp
// (Jan van Katwijk, Fabio Capozzi; GPLv2+), Qt entfernt – siehe hackrf-source.h.
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

#include "hackrf-source.h"

#include <algorithm>
#include "dab-constants.h"

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

#define DEFAULT_VGA_GAIN   24
#define DEFAULT_LNA_GAIN   40
#define OVERSAMPLE_FACTOR  2

HackRfSource::HackRfSource() : _I_Buffer(4 * 1024 * 1024) {
    gain_.lna = DEFAULT_LNA_GAIN;
    gain_.vga = DEFAULT_VGA_GAIN;
    gain_.amp = false;
    lastFrequency_ = KHz(220000);
}

HackRfSource::~HackRfSource() {
    HackRfSource::stop();
    HackRfSource::stopDump();
    if (theDevice_ != nullptr && hackrf_close) hackrf_close(theDevice_);
    if (inited_ && hackrf_exit) hackrf_exit();
    closeLibrary();
}

void HackRfSource::closeLibrary() {
    if (library_ != nullptr) { FREELIB(library_); library_ = nullptr; }
}

std::string HackRfSource::errName(int code) const {
    return hackrf_error_name ? hackrf_error_name(static_cast<hackrf_error>(code)) : "hackrf error " + std::to_string(code);
}

bool HackRfSource::open(const std::string& serial, std::string& error) {
#ifdef _WIN32
    const char* libraryString = "libhackrf.dll";
#elif __APPLE__
    const char* libraryString = "libhackrf.dylib";
#else
    const char* libraryString = "libhackrf.so.0";
#endif
    library_ = LOADLIB(libraryString);
    if (library_ == nullptr) { error = std::string("kann ") + libraryString + " nicht laden"; return false; }
    if (!loadHackrfFunctions()) { closeLibrary(); error = "Funktionen fehlen in libhackrf"; return false; }

    int rc = hackrf_init();
    if (rc != HACKRF_SUCCESS) { error = "hackrf_init: " + errName(rc); closeLibrary(); return false; }
    inited_ = true;

    if (!serial.empty() && hackrf_open_by_serial)
        rc = hackrf_open_by_serial(serial.c_str(), &theDevice_);
    else
        rc = hackrf_open(&theDevice_);
    if (rc != HACKRF_SUCCESS) {
        error = "HackRF nicht gefunden (" + errName(rc) + ")";
        theDevice_ = nullptr;
        return false;
    }
    rc = hackrf_set_sample_rate(theDevice_, static_cast<double>(SAMPLERATE * OVERSAMPLE_FACTOR));
    if (rc != HACKRF_SUCCESS) { error = "hackrf_set_sample_rate: " + errName(rc); return false; }
    // Bandbreite an die Samplerate gekoppelt wie v1 (1536 kHz bei 4,096 MS/s)
    rc = hackrf_baseband_filter(theDevice_, 1536000);
    if (rc != HACKRF_SUCCESS) { error = "hackrf_set_baseband_filter_bandwidth: " + errName(rc); return false; }
    rc = hackrf_set_freq(theDevice_, 220000000);
    if (rc != HACKRF_SUCCESS) { error = "hackrf_set_freq: " + errName(rc); return false; }
    uint16_t regValue = 0;
    rc = hackrf_si5351c_read(theDevice_, 162, &regValue);
    if (rc != HACKRF_SUCCESS) { error = "hackrf_si5351c_read: " + errName(rc); return false; }
    rc = hackrf_si5351c_write(theDevice_, 162, regValue);
    if (rc != HACKRF_SUCCESS) { error = "hackrf_si5351c_write: " + errName(rc); return false; }

    applyGain();
    hackrf_set_antenna_enable(theDevice_, antennaEnable_.load() ? 1 : 0);

    // Seriennummer wie hackrf_info aus der MCU lesen (die USB-String-
    // Deskriptoren der Geraeteliste sind unter Windows bei geoeffnetem
    // Geraet nicht lesbar); Rueckfall: Geraeteliste wie v1.
    read_partid_serialno_t ps{};
    if (hackrf_board_partid_serialno_read && hackrf_board_partid_serialno_read(theDevice_, &ps) == HACKRF_SUCCESS) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x", ps.serial_no[0], ps.serial_no[1], ps.serial_no[2], ps.serial_no[3]);
        serialNumber_ = buf;
        while (serialNumber_.size() > 1 && serialNumber_[0] == '0') serialNumber_.erase(0, 1);
    }
    hackrf_device_list_t* deviceList = hackrf_device_list();
    if (deviceList != nullptr) {
        if (deviceList->devicecount > 0) {
            // Der Listeneintrag des geoeffneten Geraets: bei serial die
            // passende Nummer, sonst Eintrag 0 (v1).
            int idx = 0;
            if (!serial.empty()) {
                for (int i = 0; i < deviceList->devicecount; i++) {
                    const char* s = deviceList->serial_numbers[i];
                    if (s != nullptr && std::string(s).find(serial) != std::string::npos) { idx = i; break; }
                }
            }
            const char* s = deviceList->serial_numbers[idx];
            if (serialNumber_.empty()) {
                serialNumber_ = s != nullptr ? s : "???";
                while (serialNumber_.size() > 1 && serialNumber_[0] == '0') serialNumber_.erase(0, 1);
            }
            boardInfo_ = hackrf_usb_board_id_name(deviceList->usb_board_ids[idx]);
        }
        if (hackrf_device_list_free) hackrf_device_list_free(deviceList);
    }
    uint8_t rev = 0;
    if (hackrf_board_rev_read(theDevice_, &rev) == HACKRF_SUCCESS)
        boardInfo_ += " rev " + std::to_string(rev);
    libraryVersion_ = std::string(hackrf_library_version()) + " (" + hackrf_library_release() + ")";
    return true;
}

// LNA 0..40 in 8er-Schritten, VGA 0..62 in 2er-Schritten (v1: & ~1);
// ungueltige Werte werden gerundet und geklemmt.
DeviceGain HackRfSource::setGain(const DeviceGain& g) {
    DeviceGain n;
    int lna = g.lna < 0 ? 0 : g.lna > 40 ? 40 : g.lna;
    n.lna = ((lna + 4) / 8) * 8;
    int vga = g.vga < 0 ? 0 : g.vga > 62 ? 62 : g.vga;
    n.vga = vga & ~0x01;
    n.amp = g.amp;
    std::lock_guard<std::mutex> lk(gainM_);   // Review G7
    gain_ = n;
    if (theDevice_ != nullptr) applyGain();
    return gain_;
}

bool HackRfSource::applyGain() {
    bool ok = true;
    int rc = hackrf_set_lna_gain(theDevice_, static_cast<uint32_t>(gain_.lna));
    if (rc != HACKRF_SUCCESS) { std::fprintf(stderr, "hackrf lnaGain: %s\n", errName(rc).c_str()); ok = false; }
    rc = hackrf_set_vga_gain(theDevice_, static_cast<uint32_t>(gain_.vga));
    if (rc != HACKRF_SUCCESS) { std::fprintf(stderr, "hackrf vgaGain: %s\n", errName(rc).c_str()); ok = false; }
    rc = hackrf_set_amp_enable(theDevice_, gain_.amp ? 1 : 0);
    if (rc != HACKRF_SUCCESS) { std::fprintf(stderr, "hackrf amp: %s\n", errName(rc).c_str()); ok = false; }
    return ok;
}

// AGC-Stufe (VGA/2) und AMP; nur geaenderte Register schreiben
void HackRfSource::setGainStep(int step, bool amp) {
    int vga = (step < 0 ? 0 : step > 31 ? 31 : step) * 2;
    std::lock_guard<std::mutex> lk(gainM_);   // Review G7
    if (vga != gain_.vga) {
        gain_.vga = vga;
        if (theDevice_ != nullptr) {
            int rc = hackrf_set_vga_gain(theDevice_, static_cast<uint32_t>(vga));
            if (rc != HACKRF_SUCCESS) std::fprintf(stderr, "hackrf vgaGain: %s\n", errName(rc).c_str());
        }
    }
    if (amp != gain_.amp) {
        gain_.amp = amp;
        if (theDevice_ != nullptr) {
            int rc = hackrf_set_amp_enable(theDevice_, amp ? 1 : 0);
            if (rc != HACKRF_SUCCESS) std::fprintf(stderr, "hackrf amp: %s\n", errName(rc).c_str());
        }
    }
}

// v1: das si5351-Register laesst sich nicht beschreiben, deshalb wird die
// Frequenz mit dem ppm-Wert korrigiert.
void HackRfSource::setPpm(int ppm) {
    ppm_ = ppm;
    if (!running_.load() || theDevice_ == nullptr) return;
    int64_t adjustedFreq = static_cast<int64_t>(lastFrequency_ * (1 + ppm / 1000000.0));
    int rc = hackrf_set_freq(theDevice_, static_cast<uint64_t>(adjustedFreq));
    if (rc != HACKRF_SUCCESS) std::fprintf(stderr, "hackrf ppm: %s\n", errName(rc).c_str());
}

// Antennenspeisung (Bias-T, 3,3 V / 50 mA). Sofort ans Geraet, wenn es offen
// ist; restart() setzt den Wunsch nach jedem Stopp erneut, weil die Firmware
// die Speisung beim Verlassen des RX-Modus selbst abschaltet.
void HackRfSource::setAntennaPower(bool on) {
    antennaEnable_.store(on);
    if (theDevice_ == nullptr) return;
    int rc = hackrf_set_antenna_enable(theDevice_, on ? 1 : 0);
    if (rc != HACKRF_SUCCESS) std::fprintf(stderr, "hackrf antenna: %s\n", errName(rc).c_str());
}

// we use a static large buffer, rather than trying to allocate
// a buffer on the stack
static std::complex<int16_t> buffer[32 * 32768];
static int callback(hackrf_transfer* transfer) {
    HackRfSource* ctx = static_cast<HackRfSource*>(transfer->rx_ctx);
    int8_t* p = reinterpret_cast<int8_t*>(transfer->buffer);
    RingBuffer<std::complex<int16_t>>* q = &(ctx->_I_Buffer);
    int nrSamples = transfer->valid_length / 2;
    if (nrSamples > 2 * 32 * 32768 - 2) nrSamples = 2 * 32 * 32768 - 2;
    // Halbband-FIR 2:1 statt Boxcar (Nachbarkanal-Alias, siehe halfband-decimator.h)
    int bufferIndex = ctx->decimator_.process(p, nrSamples, buffer);
    if (ctx->toSkip > 0)
        ctx->toSkip -= bufferIndex;
    else {
        q->putDataIntoBuffer(buffer, bufferIndex);
        ctx->onCallbackData();
    }
    return 0;
}

void HackRfSource::onCallbackData() {
    dataCv_.notify_one();
}

bool HackRfSource::restart(int32_t freq, int32_t skipped) {
    if (running_.load()) return true;
    if (theDevice_ == nullptr) return false;

    lastFrequency_ = freq;
    toSkip = skipped;
    errorReported_.store(false);
    int64_t adjustedFreq = freq + static_cast<int64_t>(ppm_) * (freq / 1000000);
    {
        std::lock_guard<std::mutex> lk(gainM_);   // Review G7
        applyGain();
    }
    int rc = hackrf_set_antenna_enable(theDevice_, antennaEnable_.load() ? 1 : 0);
    if (rc != HACKRF_SUCCESS) std::fprintf(stderr, "hackrf antenna: %s\n", errName(rc).c_str());

    rc = hackrf_set_freq(theDevice_, static_cast<uint64_t>(adjustedFreq));
    if (rc != HACKRF_SUCCESS) { reportError("hackrf_set_freq: " + errName(rc)); return false; }
    _I_Buffer.FlushRingBuffer();
    rc = hackrf_start_rx(theDevice_, callback, this);
    if (rc != HACKRF_SUCCESS) { reportError("hackrf_start_rx: " + errName(rc)); return false; }
    running_.store(hackrf_is_streaming(theDevice_) != 0);
    // Referenztakt: die Firmware schaltet beim Start des Streams selbst auf
    // CLKIN (10 MHz, z. B. GPSDO) um, wenn dort ein Takt anliegt; der Kern
    // zeigt nur an, was sie gewaehlt hat (Stefan 16.09.2026, GPSDO-Frage).
    clockSource_.clear();
    if (hackrf_get_clkin_status) {
        uint8_t st = 0;
        if (hackrf_get_clkin_status(theDevice_, &st) == HACKRF_SUCCESS) clockSource_ = st ? "extern" : "intern";
    }
    return running_.load();
}

void HackRfSource::stop() {
    if (!running_.load()) return;
    int rc = hackrf_stop_rx(theDevice_);
    if (rc != HACKRF_SUCCESS) std::fprintf(stderr, "hackrf_stop_rx: %s\n", errName(rc).c_str());
    running_.store(false);
    dataCv_.notify_all();
}

int32_t HackRfSource::getSamples(std::complex<float>* V, int32_t size) {
    if (temp_.size() < static_cast<size_t>(size)) temp_.resize(size);
    int amount = _I_Buffer.getDataFromBuffer(temp_.data(), size);
    constexpr float norm = 1.0f / (128.0f * dabcore::HalfbandDecimator::kOutScale);
    for (int i = 0; i < amount; i++)
        V[i] = std::complex<float>(real(temp_[i]) * norm, imag(temp_[i]) * norm);
    if (dumping_.load()) {
        std::lock_guard<std::mutex> lk(dumpM_);
        if (xmlWriter_) {
            // Aufnahme bleibt 8 Bit: Skalierung 128 zuruecknehmen, runden, begrenzen
            if (dump8_.size() < static_cast<size_t>(amount)) dump8_.resize(amount);
            auto to8 = [](int16_t v) {
                int r = v >= 0 ? (v + dabcore::HalfbandDecimator::kOutScale / 2) / dabcore::HalfbandDecimator::kOutScale
                               : -((-v + dabcore::HalfbandDecimator::kOutScale / 2) / dabcore::HalfbandDecimator::kOutScale);
                return static_cast<int8_t>(std::clamp(r, -127, 127));
            };
            for (int i = 0; i < amount; i++)
                dump8_[i] = std::complex<int8_t>(to8(real(temp_[i])), to8(imag(temp_[i])));
            xmlWriter_->add(dump8_.data(), amount);
        }
    }
    return amount;
}

int32_t HackRfSource::samples() {
    return static_cast<int32_t>(_I_Buffer.GetRingBufferReadAvailable());
}

bool HackRfSource::waitForSamples(int32_t n, int timeoutMs) {
    if (samples() >= n) return true;
    std::unique_lock<std::mutex> lk(m_);
    bool got = dataCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                [this, n] { return samples() >= n || !running_.load(); })
               && samples() >= n;
    if (!got && running_.load() && theDevice_ != nullptr && toSkip <= 0 &&
        hackrf_is_streaming(theDevice_) == 0 && !errorReported_.exchange(true)) {
        // libhackrf hat die Uebertragung beendet (USB-Fehler/Abriss)
        lk.unlock();
        reportError("HackRF liefert keine Daten mehr (USB-Verbindung verloren?)");
    }
    return got;
}

void HackRfSource::resetBuffer() {
    _I_Buffer.FlushRingBuffer();
    decimator_.reset();
}

bool HackRfSource::startDump(const std::string& path, std::string& error) {
    std::lock_guard<std::mutex> lk(dumpM_);
    if (xmlWriter_) { error = "Dump laeuft bereits: " + xmlWriter_->path(); return false; }
    auto w = std::make_unique<XmlFileWriter>(path, 8, "int8", SAMPLERATE, lastFrequency_, -1,
                                             "Hackrf", serialNumber_, "3.0");
    if (!w->ok()) { error = "kann " + path + " nicht schreiben"; return false; }
    xmlWriter_ = std::move(w);
    dumping_.store(true);
    return true;
}

void HackRfSource::stopDump() {
    std::lock_guard<std::mutex> lk(dumpM_);
    dumping_.store(false);
    if (xmlWriter_) { xmlWriter_->computeHeader(); xmlWriter_.reset(); }
}

bool HackRfSource::loadHackrfFunctions() {
    auto load = [this](const char* name, bool required = true) -> void* {
        void* p = GETPROC(library_, name);
        if (p == nullptr && required) std::fprintf(stderr, "Could not find %s\n", name);
        return p;
    };
    hackrf_init = (pfn_hackrf_init)load("hackrf_init");
    hackrf_open = (pfn_hackrf_open)load("hackrf_open");
    hackrf_open_by_serial = (pfn_hackrf_open_by_serial)load("hackrf_open_by_serial", false);
    hackrf_close = (pfn_hackrf_close)load("hackrf_close");
    hackrf_exit = (pfn_hackrf_exit)load("hackrf_exit");
    hackrf_start_rx = (pfn_hackrf_start_rx)load("hackrf_start_rx");
    hackrf_stop_rx = (pfn_hackrf_stop_rx)load("hackrf_stop_rx");
    hackrf_device_list = (pfn_hackrf_device_list)load("hackrf_device_list");
    hackrf_device_list_free = (pfn_hackrf_device_list_free)load("hackrf_device_list_free", false);
    hackrf_baseband_filter = (pfn_hackrf_set_baseband_filter_bandwidth)load("hackrf_set_baseband_filter_bandwidth");
    hackrf_set_lna_gain = (pfn_hackrf_set_lna_gain)load("hackrf_set_lna_gain");
    hackrf_set_vga_gain = (pfn_hackrf_set_vga_gain)load("hackrf_set_vga_gain");
    hackrf_set_freq = (pfn_hackrf_set_freq)load("hackrf_set_freq");
    hackrf_set_sample_rate = (pfn_hackrf_set_sample_rate)load("hackrf_set_sample_rate");
    hackrf_is_streaming = (pfn_hackrf_is_streaming)load("hackrf_is_streaming");
    hackrf_error_name = (pfn_hackrf_error_name)load("hackrf_error_name");
    hackrf_usb_board_id_name = (pfn_hackrf_usb_board_id_name)load("hackrf_usb_board_id_name");
    hackrf_set_antenna_enable = (pfn_hackrf_set_antenna_enable)load("hackrf_set_antenna_enable");
    hackrf_set_amp_enable = (pfn_hackrf_set_amp_enable)load("hackrf_set_amp_enable");
    hackrf_si5351c_read = (pfn_hackrf_si5351c_read)load("hackrf_si5351c_read");
    hackrf_si5351c_write = (pfn_hackrf_si5351c_write)load("hackrf_si5351c_write");
    hackrf_version_string_read = (pfn_hackrf_version_string_read)load("hackrf_version_string_read");
    hackrf_library_version = (pfn_hackrf_library_version)load("hackrf_library_version");
    hackrf_library_release = (pfn_hackrf_library_release)load("hackrf_library_release");
    hackrf_board_rev_read = (pfn_hackrf_board_rev_read)load("hackrf_board_rev_read");
    hackrf_board_partid_serialno_read = (pfn_hackrf_board_partid_serialno_read)load("hackrf_board_partid_serialno_read", false);
    hackrf_get_clkin_status = (pfn_hackrf_get_clkin_status)load("hackrf_get_clkin_status", false);

    return hackrf_init && hackrf_open && hackrf_close && hackrf_exit && hackrf_start_rx &&
           hackrf_stop_rx && hackrf_device_list && hackrf_baseband_filter && hackrf_set_lna_gain &&
           hackrf_set_vga_gain && hackrf_set_freq && hackrf_set_sample_rate && hackrf_is_streaming &&
           hackrf_error_name && hackrf_usb_board_id_name && hackrf_set_antenna_enable &&
           hackrf_set_amp_enable && hackrf_si5351c_read && hackrf_si5351c_write &&
           hackrf_version_string_read && hackrf_library_version && hackrf_library_release &&
           hackrf_board_rev_read;
}
