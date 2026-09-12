// DAB Classic v3 – Aufnahmeformat (Plan M4b 1.4): was `start_recording` und
// `export_timeshift_range` im Feld `format` bekommen, in einer Form, die
// AudioPipeline und Timeshift-Export gleichermassen benutzen.
#pragma once

#include "id3-writer.h"
#include "pcm-writer.h"

#include <memory>
#include <string>

struct RecFormat {
    std::string kind = "wav";   // "wav" | "mp3" (aac_passthrough folgt spaeter)
    uint16_t kbps = 192;        // nur MP3, Standard aus Entscheidung 6
    Id3Tags id3;                // nur MP3, leere Felder werden nicht geschrieben
};

// Passenden Schreiber anlegen und oeffnen; nullptr + error, wenn das Format
// unbekannt ist oder die Datei nicht angelegt werden kann. Bei MP3 steht der
// ID3v2-Tag danach bereits vor den Rahmen in der Datei.
std::unique_ptr<IPcmWriter> makeRecWriter(const RecFormat& fmt, const std::string& path,
                                          uint32_t rate, uint16_t channels, std::string& error);
