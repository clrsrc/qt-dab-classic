// DAB Classic v3: portiert aus Qt-DAB devices/xml-filewriter.cpp
// (Jan van Katwijk, GPLv2+), Qt entfernt – siehe xml-file-writer.h.
#
/*
 *    Copyright (C) 2016 .. 2024
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
 *    along with Qt-DAB; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "xml-file-writer.h"

#include <ctime>

#define BLOCK_SIZE 8192

XmlFileWriter::XmlFileWriter(const std::string& path, int nrBits, const std::string& container,
                             int sampleRate, int frequencyHz, int deviceGain,
                             const std::string& deviceName, const std::string& deviceModel,
                             const std::string& recorderVersion)
    : path_(path), nrBits_(nrBits), container_(container), sampleRate_(sampleRate),
      frequency_(frequencyHz), deviceName_(deviceName), deviceGain_(deviceGain),
      deviceModel_(deviceModel), recorderVersion_(recorderVersion),
      bufInt16_(BLOCK_SIZE), bufUint8_(BLOCK_SIZE), bufInt8_(BLOCK_SIZE), bufFloat_(BLOCK_SIZE) {
    xmlFile_ = std::fopen(path.c_str(), "w+b");
    if (xmlFile_ == nullptr) return;

    uint8_t t = 0;
    for (int i = 0; i < 5000; i++) std::fwrite(&t, 1, 1, xmlFile_);

    // Byte-Reihenfolge der Maschine (v1: kort_woord-Test)
    int16_t testWord = 0xFF;
    byteOrder_ = (*reinterpret_cast<uint8_t*>(&testWord) == 0xFF) ? "LSB" : "MSB";
    nrElements_ = 0;

    std::time_t now = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::gmtime(&now));
    timeString_ = buf;
}

XmlFileWriter::~XmlFileWriter() {
    if (xmlFile_ != nullptr) std::fclose(xmlFile_);
}

void XmlFileWriter::computeHeader() {
    if (xmlFile_ == nullptr) return;
    // Restpuffer: v1 verwarf angebrochene Bloecke; hier werden sie
    // geschrieben und mitgezaehlt, damit Count exakt zur Datenlaenge passt.
    if (bufferP_int16_ > 0) { std::fwrite(bufInt16_.data(), sizeof(int16_t), bufferP_int16_, xmlFile_); nrElements_ += bufferP_int16_; bufferP_int16_ = 0; }
    if (bufferP_uint8_ > 0) { std::fwrite(bufUint8_.data(), sizeof(uint8_t), bufferP_uint8_, xmlFile_); nrElements_ += bufferP_uint8_; bufferP_uint8_ = 0; }
    if (bufferP_int8_ > 0)  { std::fwrite(bufInt8_.data(),  sizeof(int8_t),  bufferP_int8_,  xmlFile_); nrElements_ += bufferP_int8_;  bufferP_int8_ = 0; }
    if (bufferP_float_ > 0) { std::fwrite(bufFloat_.data(), sizeof(float),   bufferP_float_, xmlFile_); nrElements_ += bufferP_float_; bufferP_float_ = 0; }

    std::string topLine = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    std::string s = createXmlTree();
    std::fseek(xmlFile_, 0, SEEK_SET);
    std::fprintf(xmlFile_, "%s", topLine.c_str());
    std::fprintf(xmlFile_, "%s", s.c_str());
    std::fflush(xmlFile_);
}

void XmlFileWriter::add(const std::complex<int16_t>* data, int count) {
    if (xmlFile_ == nullptr) return;
    for (int i = 0; i < count; i++) {
        bufInt16_[bufferP_int16_++] = real(data[i]);
        bufInt16_[bufferP_int16_++] = imag(data[i]);
        if (bufferP_int16_ >= BLOCK_SIZE) {
            std::fwrite(bufInt16_.data(), sizeof(int16_t), BLOCK_SIZE, xmlFile_);
            bufferP_int16_ = 0;
            nrElements_ += BLOCK_SIZE;
        }
    }
}

void XmlFileWriter::add(const std::complex<uint8_t>* data, int count) {
    if (xmlFile_ == nullptr) return;
    for (int i = 0; i < count; i++) {
        bufUint8_[bufferP_uint8_++] = real(data[i]);
        bufUint8_[bufferP_uint8_++] = imag(data[i]);
        if (bufferP_uint8_ >= BLOCK_SIZE) {
            std::fwrite(bufUint8_.data(), sizeof(uint8_t), BLOCK_SIZE, xmlFile_);
            bufferP_uint8_ = 0;
            nrElements_ += BLOCK_SIZE;
        }
    }
}

void XmlFileWriter::add(const std::complex<int8_t>* data, int count) {
    if (xmlFile_ == nullptr) return;
    for (int i = 0; i < count; i++) {
        bufInt8_[bufferP_int8_++] = real(data[i]);
        bufInt8_[bufferP_int8_++] = imag(data[i]);
        if (bufferP_int8_ >= BLOCK_SIZE) {
            std::fwrite(bufInt8_.data(), sizeof(int8_t), BLOCK_SIZE, xmlFile_);
            bufferP_int8_ = 0;
            nrElements_ += BLOCK_SIZE;
        }
    }
}

void XmlFileWriter::add(const std::complex<float>* data, int count) {
    if (xmlFile_ == nullptr) return;
    for (int i = 0; i < count; i++) {
        bufFloat_[bufferP_float_++] = real(data[i]);
        bufFloat_[bufferP_float_++] = imag(data[i]);
        if (bufferP_float_ >= BLOCK_SIZE) {
            std::fwrite(bufFloat_.data(), sizeof(float), BLOCK_SIZE, xmlFile_);
            bufferP_float_ = 0;
            nrElements_ += BLOCK_SIZE;
        }
    }
}

// Entspricht v1 create_xmltree (QDomDocument::toString mit Einrueckung 1).
// Attributwerte enthalten nur Zahlen/Namen; Sonderzeichen werden maskiert.
static std::string xmlEscape(const std::string& s) {
    std::string r;
    for (char c : s) {
        switch (c) {
            case '&': r += "&amp;"; break;
            case '<': r += "&lt;"; break;
            case '>': r += "&gt;"; break;
            case '"': r += "&quot;"; break;
            default: r.push_back(c);
        }
    }
    return r;
}

std::string XmlFileWriter::createXmlTree() const {
    std::string s;
    s += "<SDR>\n";
    s += " <Recorder Name=\"DAB Classic\" Version=\"" + xmlEscape(recorderVersion_) + "\"/>\n";
    s += " <deviceGain Value=\"" + std::to_string(deviceGain_) + "\"/>\n";
    s += " <Device Name=\"" + xmlEscape(deviceName_) + "\" Model=\"" + xmlEscape(deviceModel_) + "\"/>\n";
    s += " <Time Unit=\"UTC\" Value=\"" + timeString_ + "\"/>\n";
    s += " <Sample>\n";
    s += "  <Samplerate Unit=\"Hz\" Value=\"" + std::to_string(sampleRate_) + "\"/>\n";
    s += "  <Channels Bits=\"" + std::to_string(nrBits_) + "\" Container=\"" + container_ +
         "\" Ordering=\"" + byteOrder_ + "\">\n";
    s += "   <Channel Value=\"I\"/>\n";
    s += "   <Channel Value=\"Q\"/>\n";
    s += "  </Channels>\n";
    s += " </Sample>\n";
    s += " <Datablocks>\n";
    s += "  <Datablock Count=\"" + std::to_string(nrElements_) + "\" Number=\"1\" Unit=\"Channel\">\n";
    s += "   <Frequency Value=\"" + std::to_string(frequency_ / 1000) + "\" Unit=\"kHz\"/>\n";
    s += "   <Modulation Value=\"DAB\"/>\n";
    s += "  </Datablock>\n";
    s += " </Datablocks>\n";
    s += "</SDR>\n";
    return s;
}
