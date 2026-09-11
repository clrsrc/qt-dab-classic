// DAB Classic v3: portiert aus Qt-DAB devices/filereaders/xml-filereader/
// (Jan van Katwijk, GPLv2+), Qt entfernt – siehe xml-file-source.h.
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
 *    along with Qt-DAB; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "xml-file-source.h"
#include "dab-constants.h"

#include <cmath>
#include <cstring>

#define INPUT_FRAMEBUFFERSIZE (8 * 32768)

// ---------------------------------------------------------------------------
// Mini-XML-Parser: reicht fuer den festen Kopf der .uff-Dateien
// (Elemente mit Attributen, Kommentare, <?xml ?>-Prolog). Ersatz fuer
// QDomDocument.
// ---------------------------------------------------------------------------
namespace {

struct XmlNode {
    std::string name;
    std::map<std::string, std::string> attrs;
    std::vector<XmlNode> children;

    std::string attr(const char* key, const std::string& def) const {
        auto it = attrs.find(key);
        return it == attrs.end() ? def : it->second;
    }
};

class MiniXml {
public:
    explicit MiniXml(const std::string& text) : s_(text) {}

    bool parse(XmlNode& root) {
        std::vector<XmlNode*> stack;
        XmlNode doc;
        stack.push_back(&doc);
        while (pos_ < s_.size()) {
            size_t lt = s_.find('<', pos_);
            if (lt == std::string::npos) break;
            pos_ = lt + 1;
            if (s_.compare(pos_, 3, "!--") == 0) {                 // Kommentar
                size_t e = s_.find("-->", pos_);
                if (e == std::string::npos) return false;
                pos_ = e + 3;
                continue;
            }
            if (s_[pos_] == '?') {                                 // Prolog
                size_t e = s_.find("?>", pos_);
                if (e == std::string::npos) return false;
                pos_ = e + 2;
                continue;
            }
            if (s_[pos_] == '/') {                                 // Ende-Tag
                size_t e = s_.find('>', pos_);
                if (e == std::string::npos) return false;
                pos_ = e + 1;
                if (stack.size() > 1) stack.pop_back();
                continue;
            }
            XmlNode node;
            skipWs();
            while (pos_ < s_.size() && !isspace((unsigned char)s_[pos_]) &&
                   s_[pos_] != '>' && s_[pos_] != '/')
                node.name.push_back(s_[pos_++]);
            bool selfClosing = false;
            for (;;) {
                skipWs();
                if (pos_ >= s_.size()) return false;
                if (s_[pos_] == '/') { selfClosing = true; ++pos_; continue; }
                if (s_[pos_] == '>') { ++pos_; break; }
                std::string key;
                while (pos_ < s_.size() && s_[pos_] != '=' && !isspace((unsigned char)s_[pos_]) &&
                       s_[pos_] != '>' && s_[pos_] != '/')
                    key.push_back(s_[pos_++]);
                skipWs();
                if (pos_ < s_.size() && s_[pos_] == '=') {
                    ++pos_;
                    skipWs();
                    char q = s_[pos_];
                    if (q != '"' && q != '\'') return false;
                    size_t e = s_.find(q, pos_ + 1);
                    if (e == std::string::npos) return false;
                    node.attrs[key] = s_.substr(pos_ + 1, e - pos_ - 1);
                    pos_ = e + 1;
                } else {
                    node.attrs[key] = "";
                }
            }
            XmlNode* parent = stack.back();
            parent->children.push_back(std::move(node));
            if (!selfClosing) stack.push_back(&parent->children.back());
        }
        if (doc.children.empty()) return false;
        root = std::move(doc.children.front());
        return true;
    }

private:
    void skipWs() { while (pos_ < s_.size() && isspace((unsigned char)s_[pos_])) ++pos_; }
    const std::string& s_;
    size_t pos_ = 0;
};

int shift(int a) {
    int r = 1;
    while (--a > 0) r <<= 1;
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// XmlDescriptor (v1 xml-descriptor.cpp)
// ---------------------------------------------------------------------------
int XmlDescriptor::sampleSize() const {
    int bytes = bitsperChannel <= 8 ? 1 : bitsperChannel <= 16 ? 2 : bitsperChannel <= 24 ? 3 : 4;
    return nrChannels * bytes;
}

bool XmlDescriptor::parse(FILE* f, std::string& error) {
    std::string xmlText;
    int zeroCount = 0;
    while (zeroCount < 500) {
        int c = fgetc(f);
        if (c == EOF) { error = "Dateiende im XML-Kopf"; return false; }
        if (c == 0) zeroCount++; else zeroCount = 0;
        xmlText.push_back(static_cast<char>(c));
    }
    // Nullbytes vor dem Parsen entfernen
    std::string clean;
    clean.reserve(xmlText.size());
    for (char c : xmlText) if (c != 0) clean.push_back(c);

    XmlNode root;
    MiniXml parser(clean);
    if (!parser.parse(root)) { error = "XML-Kopf nicht lesbar"; return false; }

    for (auto& component : root.children) {
        if (component.name == "Recorder") {
            recorderName = component.attr("Name", "??");
            recorderVersion = component.attr("Version", "??");
        }
        if (component.name == "Device") {
            deviceName = component.attr("Name", "unknown");
            deviceModel = component.attr("Model", "???");
        }
        if (component.name == "Time")
            recordingTime = component.attr("Value", "???");
        if (component.name == "Sample") {
            for (auto& child : component.children) {
                if (child.name == "Samplerate") {
                    std::string SR = child.attr("Value", std::to_string(SAMPLERATE));
                    std::string Hz = child.attr("Unit", "Hz");
                    int factor = Hz == "Hz" ? 1 : (Hz == "KHz") || (Hz == "Khz") ? 1000 : 1000000;
                    sampleRate = atoi(SR.c_str()) * factor;
                }
                if (child.name == "Channels") {
                    nrChannels = atoi(child.attr("Amount", "2").c_str());
                    bitsperChannel = atoi(child.attr("Bits", "8").c_str());
                    container = child.attr("Container", "uint8_t");
                    byteOrder = child.attr("Ordering", "N/A");
                    int channelOrder = 0;
                    for (auto& sub : child.children) {
                        if (sub.name != "Channel") continue;
                        std::string Value = sub.attr("Value", "I");
                        // addChannelOrder
                        if (channelOrder <= 1) {
                            if (channelOrder == 0)
                                iqOrder = Value == "I" ? "I_ONLY" : "Q_ONLY";
                            else if (iqOrder == "I_ONLY" && Value == "Q")
                                iqOrder = "IQ";
                            else if (iqOrder == "Q_ONLY" && Value == "I")
                                iqOrder = "QI";
                        }
                        channelOrder++;
                    }
                }
            }
        }
        if (component.name == "deviceGain")
            deviceGain = atoi(component.attr("Value", "-1").c_str());
        if (component.name == "Datablocks") {
            nrBlocks = 0;
            int currBlock = 0;
            for (auto& child : component.children) {
                if (child.name != "Datablock") continue;
                Block b;
                b.nrElements = strtoull(child.attr("Count", "100").c_str(), nullptr, 10);
                b.blockNumber = atoi(child.attr("Number", "10").c_str());
                b.typeofUnit = child.attr("Channel", "Channel");
                blockList.push_back(b);
                for (auto& sub : child.children) {
                    if (sub.name == "Frequency") {
                        std::string Unit = sub.attr("Unit", "Hz");
                        int Value = atoi(sub.attr("Value", "200").c_str());
                        int Frequency = Unit == "Hz" ? Value :
                            ((Unit == "KHz") || (Unit == "kHz")) ? Value * 1000 : Value * 1000000;
                        blockList.at(currBlock).frequency = Frequency;
                    }
                    if (sub.name == "Modulation")
                        blockList.at(currBlock).modType = sub.attr("Value", "DAB");
                }
                currBlock++;
            }
            nrBlocks = currBlock;
        }
    }
    if (nrBlocks <= 0) { error = "kein Datablock im XML-Kopf"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// XmlFileSource (v1 xml_fileReader + xml_Reader)
// ---------------------------------------------------------------------------
XmlFileSource::XmlFileSource(const std::string& path, FileSourceOptions options)
    : FileSourceBase(path, INPUT_FRAMEBUFFERSIZE, options) {
    for (int i = 0; i < 256; i++)
        // the offset 127.38f is due to the input data comes usually from
        // an SDR stick which has its DC offset a bit shifted from ideal (from old-dab)
        mapTable_[i] = ((float)i - 127.38) / 128.0;
}

bool XmlFileSource::open(std::string& error) {
    file_ = std::fopen(path_.c_str(), "rb");
    if (file_ == nullptr) { error = "kann " + path_ + " nicht oeffnen"; return false; }
    if (!fd_.parse(file_, error)) {
        std::fclose(file_); file_ = nullptr;
        return false;
    }
    sampleRate_ = fd_.sampleRate;
    frequency_ = fd_.blockList[0].frequency;

    uint64_t nrElements = 0;
    for (int i = 0; i < fd_.nrBlocks; i++) nrElements += fd_.blockList[i].nrElements;
    uint16_t sampleSize = fd_.sampleSize();
    fileSeek(file_, 0, SEEK_END);
    uint64_t fileLength = static_cast<uint64_t>(fileTell(file_));
    dataStart_ = fileLength - (uint64_t)(nrElements * (sampleSize / 2));
    if (dataStart_ <= 1000)   // as with DABstar
        dataStart_ = 5000;

    // Resampling-Tabellen (v1 xml_Reader-Konstruktor); v1 liest immer nur
    // den ersten Datablock.
    convBufferSize_ = fd_.sampleRate / 1000;
    float samplesPerMsec = SAMPLERATE / 1000.0;
    mapTable_int_.resize(SAMPLERATE / 1000);
    mapTable_float_.resize(SAMPLERATE / 1000);
    for (int i = 0; i < SAMPLERATE / 1000; i++) {
        float inVal = float(fd_.sampleRate / 1000);
        mapTable_int_[i] = (int)(floor(i * (inVal / samplesPerMsec)));
        mapTable_float_[i] = i * (inVal / samplesPerMsec) - mapTable_int_[i];
    }
    convBuffer_.assign(convBufferSize_ + 1, std::complex<float>(0, 0));
    lbuf_.resize(8 * convBufferSize_);
    chunkSamples_ = SAMPLERATE / 1000;
    totalSamples_ = computeNrSamples(0);
    sampleRate_ = SAMPLERATE;    // nach dem Resampling
    return true;
}

uint64_t XmlFileSource::computeNrSamples(int blockNumber) const {
    uint64_t nrElements = fd_.blockList.at(blockNumber).nrElements;
    if (fd_.blockList.at(blockNumber).typeofUnit == "Channel") {
        if ((fd_.iqOrder == "IQ") || (fd_.iqOrder == "QI"))
            return nrElements / 2;
        return nrElements;
    }
    return nrElements;   // typeofUnit = "sample"
}

void XmlFileSource::seekStart() {
    fileSeek(file_, static_cast<int64_t>(dataStart_), SEEK_SET);   // 64 Bit: Dateien > 2 GB
    // v1: convBuffer wird nicht zurueckgesetzt; convBuffer [0] traegt das
    // letzte Sample des vorigen Blocks (ein Sample Verzoegerung).
}

// v1 xml_Reader::readSamples: 1 ms lesen, auf 2,048 MS/s interpolieren
int32_t XmlFileSource::readChunk(std::complex<float>* out, int32_t maxSamples) {
    (void)maxSamples;
    if (fd_.iqOrder == "IQ")
        readElements_IQ(&convBuffer_[1], convBufferSize_);
    else if (fd_.iqOrder == "QI")
        readElements_QI(&convBuffer_[1], convBufferSize_);
    else if (fd_.iqOrder == "I_Only")
        readElements_I(&convBuffer_[1], convBufferSize_);
    else
        readElements_Q(&convBuffer_[1], convBufferSize_);
    if (feof(file_)) return 0;
    for (int i = 0; i < SAMPLERATE / 1000; i++) {
        int16_t inpBase = mapTable_int_[i];
        float inpRatio = mapTable_float_[i];
        out[i] = convBuffer_[inpBase + 1] * inpRatio +
                 convBuffer_[inpBase] * (1.0f - inpRatio);
    }
    convBuffer_[0] = convBuffer_[convBufferSize_];
    return SAMPLERATE / 1000;
}

// Die Leser (v1 xml-reader.cpp); Container-Namen und Umrechnung wie v1.
void XmlFileSource::readElements_IQ(std::complex<float>* buffer, int amount) {
    int nrBits = fd_.bitsperChannel;
    float scaler = float(shift(nrBits));
    uint8_t* lbuf = lbuf_.data();

    if (fd_.container == "int8") {
        size_t objectsRead = fread(lbuf, 1, 2 * amount, file_);
        for (size_t i = 0; i < objectsRead / 2; i++)
            buffer[i] = std::complex<float>(((int8_t)lbuf[2 * i]) / 127.0,
                                            ((int8_t)lbuf[2 * i + 1]) / 127.0);
        return;
    }
    if (fd_.container == "uint8") {
        size_t objectsRead = fread(lbuf, 1, 2 * amount, file_);
        for (size_t i = 0; i < objectsRead / 2; i++)
            buffer[i] = std::complex<float>(mapTable_[lbuf[2 * i]], mapTable_[lbuf[2 * i + 1]]);
        return;
    }
    if (fd_.container == "int16") {
        size_t objectsRead = fread(lbuf, 2, 2 * amount, file_);
        if (fd_.byteOrder == "MSB") {
            for (size_t i = 0; i < objectsRead / 2; i++) {
                int16_t t1 = (lbuf[4 * i] << 8) | lbuf[4 * i + 1];
                int16_t t2 = (lbuf[4 * i + 2] << 8) | lbuf[4 * i + 3];
                buffer[i] = std::complex<float>((float)t1 / scaler, (float)t2 / scaler);
            }
        } else {
            for (size_t i = 0; i < objectsRead / 2; i++) {
                int16_t t1 = (lbuf[4 * i + 1] << 8) | lbuf[4 * i];
                int16_t t2 = (lbuf[4 * i + 3] << 8) | lbuf[4 * i + 2];
                buffer[i] = std::complex<float>((float)t1 / scaler, (float)t2 / scaler);
            }
        }
        return;
    }
    if (fd_.container == "int24") {
        size_t objectsRead = fread(lbuf, 3, 2 * amount, file_);
        for (size_t i = 0; i < objectsRead / 2; i++) {
            int32_t t1, t2;
            if (fd_.byteOrder == "MSB") {
                t1 = (lbuf[6 * i] << 16) | (lbuf[6 * i + 1] << 8) | lbuf[6 * i + 2];
                t2 = (lbuf[6 * i + 3] << 16) | (lbuf[6 * i + 4] << 8) | lbuf[6 * i + 5];
            } else {
                t1 = (lbuf[6 * i + 2] << 16) | (lbuf[6 * i + 1] << 8) | lbuf[6 * i];
                t2 = (lbuf[6 * i + 5] << 16) | (lbuf[6 * i + 4] << 8) | lbuf[6 * i + 3];
            }
            if (t1 & 0x800000) t1 |= 0xFF000000;
            if (t2 & 0x800000) t2 |= 0xFF000000;
            buffer[i] = std::complex<float>((float)t1 / scaler, (float)t2 / scaler);
        }
        return;
    }
    if (fd_.container == "int32") {
        size_t objectsRead = fread(lbuf, 4, 2 * amount, file_);
        for (size_t i = 0; i < objectsRead / 2; i++) {
            int32_t t1, t2;
            if (fd_.byteOrder == "MSB") {
                t1 = (lbuf[8 * i] << 24) | (lbuf[8 * i + 1] << 16) | (lbuf[8 * i + 2] << 8) | lbuf[8 * i + 3];
                t2 = (lbuf[8 * i + 4] << 24) | (lbuf[8 * i + 5] << 16) | (lbuf[8 * i + 6] << 8) | lbuf[8 * i + 7];
            } else {
                t1 = (lbuf[8 * i + 3] << 24) | (lbuf[8 * i + 2] << 16) | (lbuf[8 * i + 1] << 8) | lbuf[8 * i];
                t2 = (lbuf[8 * i + 7] << 24) | (lbuf[8 * i + 6] << 16) | (lbuf[8 * i + 5] << 8) | lbuf[8 * i + 4];
            }
            buffer[i] = std::complex<float>((float)t1 / scaler, (float)t2 / scaler);
        }
        return;
    }
    if (fd_.container == "float32") {
        size_t objectsRead = fread(lbuf, 4, 2 * amount, file_);
        for (size_t i = 0; i < objectsRead / 2; i++) {
            uint32_t r, im;
            if (fd_.byteOrder == "MSB") {
                r  = ((uint32_t)lbuf[8 * i] << 24) | (lbuf[8 * i + 1] << 16) | (lbuf[8 * i + 2] << 8) | lbuf[8 * i + 3];
                im = ((uint32_t)lbuf[8 * i + 4] << 24) | (lbuf[8 * i + 5] << 16) | (lbuf[8 * i + 6] << 8) | lbuf[8 * i + 7];
            } else {
                r  = ((uint32_t)lbuf[8 * i + 3] << 24) | (lbuf[8 * i + 2] << 16) | (lbuf[8 * i + 1] << 8) | lbuf[8 * i];
                im = ((uint32_t)lbuf[8 * i + 7] << 24) | (lbuf[8 * i + 6] << 16) | (lbuf[8 * i + 5] << 8) | lbuf[8 * i + 4];
            }
            float fr, fi;
            memcpy(&fr, &r, 4); memcpy(&fi, &im, 4);
            buffer[i] = std::complex<float>(fr, fi);
        }
        return;
    }
}

void XmlFileSource::readElements_QI(std::complex<float>* buffer, int amount) {
    // wie IQ, Real- und Imaginaerteil vertauscht (v1 readElements_QI)
    readElements_IQ(buffer, amount);
    for (int i = 0; i < amount; i++)
        buffer[i] = std::complex<float>(imag(buffer[i]), real(buffer[i]));
}

void XmlFileSource::readElements_I(std::complex<float>* buffer, int amount) {
    int nrBits = fd_.bitsperChannel;
    float scaler = float(shift(nrBits));
    uint8_t* lbuf = lbuf_.data();
    if (fd_.container == "int8") {
        size_t n = fread(lbuf, 1, amount, file_);
        for (size_t i = 0; i < n; i++) buffer[i] = std::complex<float>((int8_t)lbuf[i] / 127.0, 0);
        return;
    }
    if (fd_.container == "uint8") {
        size_t n = fread(lbuf, 1, amount, file_);
        for (size_t i = 0; i < n; i++) buffer[i] = std::complex<float>(mapTable_[lbuf[i]], 0);
        return;
    }
    if (fd_.container == "int16") {
        size_t n = fread(lbuf, 2, amount, file_);
        for (size_t i = 0; i < n; i++) {
            int16_t t = fd_.byteOrder == "MSB" ? (int16_t)((lbuf[2 * i] << 8) | lbuf[2 * i + 1])
                                               : (int16_t)((lbuf[2 * i + 1] << 8) | lbuf[2 * i]);
            buffer[i] = std::complex<float>((float)t / scaler, 0);
        }
        return;
    }
    if (fd_.container == "int24") {
        size_t n = fread(lbuf, 3, amount, file_);
        for (size_t i = 0; i < n; i++) {
            int32_t t = fd_.byteOrder == "MSB" ? ((lbuf[3 * i] << 16) | (lbuf[3 * i + 1] << 8) | lbuf[3 * i + 2])
                                               : ((lbuf[3 * i + 2] << 16) | (lbuf[3 * i + 1] << 8) | lbuf[3 * i]);
            if (t & 0x800000) t |= 0xFF000000;
            buffer[i] = std::complex<float>((float)t / scaler, 0);
        }
        return;
    }
    if (fd_.container == "int32") {
        size_t n = fread(lbuf, 4, amount, file_);
        for (size_t i = 0; i < n; i++) {
            int32_t t = fd_.byteOrder == "MSB"
                ? ((lbuf[4 * i] << 24) | (lbuf[4 * i + 1] << 16) | (lbuf[4 * i + 2] << 8) | lbuf[4 * i + 3])
                : ((lbuf[4 * i + 3] << 24) | (lbuf[4 * i + 2] << 16) | (lbuf[4 * i + 1] << 8) | lbuf[4 * i]);
            buffer[i] = std::complex<float>((float)t / scaler, 0);
        }
        return;
    }
    if (fd_.container == "float32") {
        size_t n = fread(lbuf, 4, amount, file_);
        for (size_t i = 0; i < n; i++) {
            uint32_t r = fd_.byteOrder == "MSB"
                ? (((uint32_t)lbuf[4 * i] << 24) | (lbuf[4 * i + 1] << 16) | (lbuf[4 * i + 2] << 8) | lbuf[4 * i + 3])
                : (((uint32_t)lbuf[4 * i + 3] << 24) | (lbuf[4 * i + 2] << 16) | (lbuf[4 * i + 1] << 8) | lbuf[4 * i]);
            float f; memcpy(&f, &r, 4);
            buffer[i] = std::complex<float>(f, 0);
        }
        return;
    }
}

void XmlFileSource::readElements_Q(std::complex<float>* buffer, int amount) {
    readElements_I(buffer, amount);
    for (int i = 0; i < amount; i++)
        buffer[i] = std::complex<float>(0, real(buffer[i]));
}
