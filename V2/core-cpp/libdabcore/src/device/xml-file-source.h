// DAB Classic v3: portiert aus Qt-DAB devices/filereaders/xml-filereader/
// (xml-filereader, xml-descriptor, xml-reader; Jan van Katwijk, GPLv2+),
// Qt entfernt: QDomDocument durch einen Mini-XML-Parser ersetzt, QThread
// durch FileSourceBase, kein Widget. Sample-Formate und Umrechnung
// (int8/uint8/int16/int24/int32/float32, IQ/QI/I/Q, Resampling auf
// 2,048 MS/s) wie v1.
#pragma once

#include "file-source.h"

#include <map>
#include <string>
#include <vector>

// Entspricht xmlDescriptor aus v1.
struct XmlDescriptor {
    struct Block {
        int      blockNumber = 0;
        uint64_t nrElements = 0;
        std::string typeofUnit;
        int      frequency = 0;
        std::string modType;
    };
    std::string deviceName, deviceModel, recorderName, recorderVersion, recordingTime;
    int sampleRate = 2048000;
    int nrChannels = 2;
    int bitsperChannel = 16;
    int deviceGain = -1;
    std::string container = "int16_t";
    std::string byteOrder = "MSB";
    std::string iqOrder = "IQ";
    int nrBlocks = 0;
    std::vector<Block> blockList;

    int sampleSize() const;
    // Liest den XML-Kopf (bis 500 Nullbytes) und fuellt die Felder; false bei Fehler.
    bool parse(FILE* f, std::string& error);
};

class XmlFileSource : public FileSourceBase {
public:
    XmlFileSource(const std::string& path, FileSourceOptions options);
    ~XmlFileSource() override = default;

    // false, wenn die Datei nicht lesbar oder kein XML-Kopf vorhanden ist.
    bool open(std::string& error);

    std::string name() const override { return "xml-file"; }
    int16_t bitDepth() const override { return static_cast<int16_t>(fd_.bitsperChannel); }
    int32_t vfoFrequency() const override { return frequency_; }
    const XmlDescriptor& descriptor() const { return fd_; }

protected:
    void seekStart() override;
    int32_t readChunk(std::complex<float>* out, int32_t maxSamples) override;

private:
    uint64_t computeNrSamples(int blockNumber) const;
    void readElements_IQ(std::complex<float>* buffer, int amount);
    void readElements_QI(std::complex<float>* buffer, int amount);
    void readElements_I(std::complex<float>* buffer, int amount);
    void readElements_Q(std::complex<float>* buffer, int amount);

    XmlDescriptor fd_;
    uint64_t dataStart_ = 0;
    int32_t  frequency_ = 0;
    float    mapTable_[256];
    // Resampling-Tabellen wie v1 xml_Reader
    int16_t  convBufferSize_ = 0;
    std::vector<std::complex<float>> convBuffer_;
    std::vector<int16_t> mapTable_int_;
    std::vector<float>   mapTable_float_;
    std::vector<uint8_t> lbuf_;
};
