// DAB Classic v3 – gemeinsame Schnittstelle der AAC-Decoder (faad2 fest
// gelinkt, FDK-AAC zur Laufzeit per LoadLibrary; Entscheidung 20).
// stream_parms wie in Qt-DAB faad-decoder.h / fdk-aac.h (Jan van Katwijk, GPLv2+).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dab-constants.h"

typedef struct {
        int	rfa;
        int	dacRate;
        int	sbrFlag;
        int	psFlag;
        int	aacChannelMode;
        int	mpegSurround;
	int	CoreChConfig;
	int	CoreSrIndex;
	int	ExtensionSrIndex;
} stream_parms;

enum class AacDecoderKind { Auto, Faad2, Fdk };

// PCM-Ausgabe eines Decoders: Paare (L, R) als complex16, Abtastrate,
// PS/SBR-Kennung wie vom Decoder gemeldet.
using PcmSink = std::function<void(const complex16* pcm, int nPairs, int rate, bool ps, bool sbr)>;

class IAacDecoder {
public:
    virtual ~IAacDecoder() = default;
    // Liefert die Kanalzahl (> 0) bei Erfolg, <= 0 bei Fehler (v1 MP42PCM).
    virtual int16_t MP42PCM(stream_parms* sp, uint8_t* buffer, int16_t bufferLength) = 0;
    // FDK erwartet LOAS/LATM-Rahmen (build_aacFile), faad2 die nackte AU.
    virtual bool takesLoas() const = 0;
    virtual const char* name() const = 0;
};

// Auto: FDK, wenn libfdk-aac-2.dll geladen werden kann, sonst faad2.
std::unique_ptr<IAacDecoder> createAacDecoder(AacDecoderKind kind, PcmSink sink);

// Namen der tatsaechlich verfuegbaren Decoder (fuer das ready-Ereignis).
std::vector<std::string> availableAacDecoders();
AacDecoderKind aacDecoderKindFromName(const std::string& name);   // "auto" | "faad2" | "fdk"
