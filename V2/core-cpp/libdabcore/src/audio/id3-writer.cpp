// DAB Classic v3 – ID3v2.4-Schreiber, siehe id3-writer.h.
#include "id3-writer.h"

#include <cstdio>
#include <cstring>

namespace {

// ID3v2.4: alle Groessenfelder (Tag und Rahmen) sind syncsafe, also
// 4 x 7 Nutzbits, hoechstes Bit jedes Bytes 0.
void putSyncsafe(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 21) & 0x7F));
    out.push_back(static_cast<uint8_t>((v >> 14) & 0x7F));
    out.push_back(static_cast<uint8_t>((v >> 7) & 0x7F));
    out.push_back(static_cast<uint8_t>(v & 0x7F));
}

void appendFrame(std::vector<uint8_t>& out, const char id[4], const std::vector<uint8_t>& body) {
    out.insert(out.end(), id, id + 4);
    putSyncsafe(out, static_cast<uint32_t>(body.size()));
    out.push_back(0x00);   // Flags
    out.push_back(0x00);
    out.insert(out.end(), body.begin(), body.end());
}

void appendText(std::vector<uint8_t>& out, const char id[4], const std::string& text) {
    if (text.empty()) return;
    std::vector<uint8_t> body;
    body.reserve(text.size() + 1);
    body.push_back(0x03);   // UTF-8
    body.insert(body.end(), text.begin(), text.end());
    appendFrame(out, id, body);
}

int b64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;   // '-'/'_' = URL-Variante
    if (c == '/' || c == '_') return 63;
    return -1;
}

} // namespace

namespace Id3Writer {

std::vector<uint8_t> base64Decode(const std::string& in) {
    std::vector<uint8_t> out;
    size_t start = 0;
    // "data:image/png;base64,...." (die App liefert Logos als Data-URL)
    const size_t comma = in.find(',');
    if (in.compare(0, 5, "data:") == 0 && comma != std::string::npos) start = comma + 1;
    out.reserve((in.size() - start) * 3 / 4 + 3);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = start; i < in.size(); ++i) {
        const int v = b64Value(in[i]);
        if (v < 0) continue;   // Whitespace, '=' und alles Unbekannte
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

std::vector<uint8_t> build(const Id3Tags& tags) {
    std::vector<uint8_t> frames;
    appendText(frames, "TIT2", tags.title);
    appendText(frames, "TPE1", tags.artist);
    appendText(frames, "TALB", tags.album);
    appendText(frames, "TDRC", tags.date);
    appendText(frames, "TCON", tags.genre);
    if (!tags.coverPngB64.empty()) {
        const std::vector<uint8_t> png = base64Decode(tags.coverPngB64);
        if (!png.empty()) {
            static const char kMime[] = "image/png";
            std::vector<uint8_t> body;
            body.reserve(png.size() + 16);
            body.push_back(0x03);                                  // UTF-8 (gilt fuer die Beschreibung)
            body.insert(body.end(), kMime, kMime + sizeof kMime);  // inkl. abschliessender 0
            body.push_back(0x03);                                  // Cover (front)
            body.push_back(0x00);                                  // leere Beschreibung
            body.insert(body.end(), png.begin(), png.end());
            appendFrame(frames, "APIC", body);
        }
    }
    if (frames.empty()) return {};

    std::vector<uint8_t> tag;
    tag.reserve(frames.size() + 10);
    tag.push_back('I'); tag.push_back('D'); tag.push_back('3');
    tag.push_back(0x04); tag.push_back(0x00);   // Version 2.4.0
    tag.push_back(0x00);                        // Flags
    putSyncsafe(tag, static_cast<uint32_t>(frames.size()));
    tag.insert(tag.end(), frames.begin(), frames.end());
    return tag;
}

bool prependToFile(const std::string& path, const std::vector<uint8_t>& tag, std::string& error) {
    if (tag.empty()) return true;
    const std::string tmp = path + ".id3tmp";
    FILE* in = std::fopen(path.c_str(), "rb");
    if (!in) { error = "ID3: kann " + path + " nicht lesen"; return false; }
    FILE* out = std::fopen(tmp.c_str(), "wb");
    if (!out) { std::fclose(in); error = "ID3: kann " + tmp + " nicht schreiben"; return false; }
    bool ok = std::fwrite(tag.data(), 1, tag.size(), out) == tag.size();
    std::vector<uint8_t> buf(64 * 1024);
    while (ok) {
        const size_t n = std::fread(buf.data(), 1, buf.size(), in);
        if (n == 0) break;
        ok = std::fwrite(buf.data(), 1, n, out) == n;
    }
    std::fclose(in);
    std::fclose(out);
    if (!ok) { std::remove(tmp.c_str()); error = "ID3: Schreibfehler"; return false; }
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        error = "ID3: kann " + tmp + " nicht umbenennen";
        return false;
    }
    return true;
}

} // namespace Id3Writer
