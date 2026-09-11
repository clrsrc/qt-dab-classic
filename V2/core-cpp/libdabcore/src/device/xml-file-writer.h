// DAB Classic v3: portiert aus Qt-DAB devices/xml-filewriter.{h,cpp}
// (Jan van Katwijk, GPLv2+), Qt entfernt: QDomDocument durch handgebauten
// XML-Text (gleiche Elemente/Attribute wie v1, damit XmlFileSource ihn
// liest), QDateTime durch <ctime>, findfileNames entfaellt (der Aufrufer
// gibt den Pfad vor). Dateiaufbau wie v1: 5000 Nullbytes als Platzhalter,
// dann die Rohdaten in 8192er-Bloecken; computeHeader() schreibt den
// XML-Kopf beim Schliessen an den Anfang.
#
/*
 *    Copyright (C) 2014 .. 2024
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *    Lazy Chair Computing
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
#pragma once

#include <complex>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class XmlFileWriter {
public:
    // container: "int8" | "uint8" | "int16" | "float32" (wie v1)
    XmlFileWriter(const std::string& path, int nrBits, const std::string& container,
                  int sampleRate, int frequencyHz, int deviceGain,
                  const std::string& deviceName, const std::string& deviceModel,
                  const std::string& recorderVersion);
    ~XmlFileWriter();
    XmlFileWriter(const XmlFileWriter&) = delete;
    XmlFileWriter& operator=(const XmlFileWriter&) = delete;

    bool ok() const { return xmlFile_ != nullptr; }
    const std::string& path() const { return path_; }

    void add(const std::complex<int16_t>* data, int count);
    void add(const std::complex<uint8_t>* data, int count);
    void add(const std::complex<int8_t>* data, int count);
    void add(const std::complex<float>* data, int count);
    // Restpuffer schreiben und XML-Kopf an den Dateianfang setzen.
    void computeHeader();

private:
    std::string createXmlTree() const;

    std::string path_;
    int nrBits_;
    std::string container_;
    int sampleRate_;
    int frequency_;
    std::string deviceName_;
    int deviceGain_;
    std::string deviceModel_;
    std::string recorderVersion_;
    FILE* xmlFile_ = nullptr;
    std::string byteOrder_;
    uint64_t nrElements_ = 0;
    std::string timeString_;

    std::vector<int16_t> bufInt16_;
    std::vector<uint8_t> bufUint8_;
    std::vector<int8_t>  bufInt8_;
    std::vector<float>   bufFloat_;
    int bufferP_float_ = 0;
    int bufferP_int16_ = 0;
    int bufferP_uint8_ = 0;
    int bufferP_int8_ = 0;
};
