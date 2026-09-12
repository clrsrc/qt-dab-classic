// ctest timeshift_buffer: TimeshiftBuffer als reiner Speicher, ohne Thread
// und ohne Kern. Geprueft werden Packen/Entpacken, Schreib-/Lesezeiger,
// Ueberlauf, Suchen/Springen und das Kopieren fuer den Export.
#include "timeshift-buffer.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using dabcore::TimeshiftBuffer;
using dabcore::TimeshiftMode;

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FEHLER %s:%d: %s\n", __FILE__, __LINE__, msg); failures++; } } while (0)

static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

// Testrahmen: ein Bit je Byte, Muster haengt an der Rahmennummer.
static std::vector<uint8_t> makeFrame(uint32_t frameBits, uint64_t n) {
    std::vector<uint8_t> f(frameBits);
    for (uint32_t i = 0; i < frameBits; ++i)
        f[i] = static_cast<uint8_t>(((i + n) % 3) == 0 ? 1 : 0);
    return f;
}

// Dlf: 104 kbit/s -> 2496 Hardbits, 312 Byte gepackt je Rahmen.
static constexpr uint32_t kBitRate = 104;
static constexpr uint32_t kFrameBits = kBitRate * 24;

static void testGeometry() {
    TimeshiftBuffer b;
    CHECK(!b.active(), "ohne Rahmengroesse nicht aktiv");
    b.setCapacitySeconds(120);
    b.setFrameBits(kFrameBits);
    CHECK(b.active(), "mit Kapazitaet und Rahmengroesse aktiv");
    CHECK(b.capacityFrames() == 5000, "120 s = 5000 Rahmen");
    CHECK(b.packedBytes() == 312, "104 kbit/s = 312 Byte je Rahmen");
    CHECK(b.capacityBytes() == 5000ull * 312, "Speicherbedarf = Rahmen x Byte");
    CHECK(near(b.bufferedSeconds(), 0.0), "leerer Ring");
    CHECK(b.mode() == TimeshiftMode::Live, "Start live");
    // 3600 s bei 104 kbit/s: 150 000 Rahmen x 312 Byte = 46,8 MB (Plan 1.2)
    b.setCapacitySeconds(3600);
    CHECK(b.capacityFrames() == 150000, "3600 s = 150 000 Rahmen");
    CHECK(b.capacityBytes() == 150000ull * 312, "60 min ~ 46,8 MB");
    std::printf("Geometrie: 60 min = %llu Rahmen x %u Byte = %.1f MB\n",
                static_cast<unsigned long long>(b.capacityFrames()), b.packedBytes(),
                static_cast<double>(b.capacityBytes()) / (1024.0 * 1024.0));
}

static void testRoundTrip() {
    TimeshiftBuffer b;
    b.setCapacitySeconds(60);
    b.setFrameBits(kFrameBits);
    b.setMode(TimeshiftMode::Paused);   // live wuerde den Lesezeiger mitziehen
    for (uint64_t n = 0; n < 10; ++n) {
        auto f = makeFrame(kFrameBits, n);
        b.push(f.data(), kFrameBits, 1789194000 + static_cast<int64_t>(n));
    }
    CHECK(b.writeIndex() == 10, "10 Rahmen geschrieben");
    CHECK(near(b.offsetSeconds(), 0.24), "paused: der Lesezeiger bleibt stehen");
    CHECK(near(b.bufferedSeconds(), 0.24), "10 Rahmen = 0,24 s");
    CHECK(b.liveUnix() == 1789194009, "letzter Zeitstempel");
    // Lesezeiger steht noch auf 0 (nichts gelesen): alles lesbar
    std::vector<uint8_t> out;
    int64_t stamp = 0;
    for (uint64_t n = 0; n < 10; ++n) {
        CHECK(b.pop(out, &stamp), "Rahmen lesbar");
        CHECK(out == makeFrame(kFrameBits, n), "Rahmeninhalt unveraendert (Packen/Entpacken)");
        CHECK(stamp == 1789194000 + static_cast<int64_t>(n), "Zeitstempel je Rahmen");
    }
    CHECK(!b.pop(out), "am Schreibzeiger kommt nichts mehr");
    CHECK(near(b.offsetSeconds(), 0.0), "Versatz 0 am Schreibzeiger");
    // Rahmen mit falscher Groesse werden verworfen
    std::vector<uint8_t> wrong(kFrameBits - 8, 1);
    b.push(wrong.data(), static_cast<uint32_t>(wrong.size()), 0);
    CHECK(b.writeIndex() == 10, "falsche Rahmengroesse verworfen");
    std::printf("Roundtrip: ok\n");
}

static void testLiveFollows() {
    TimeshiftBuffer b;
    b.setCapacitySeconds(60);
    b.setFrameBits(kFrameBits);
    for (uint64_t n = 0; n < 100; ++n) {
        auto f = makeFrame(kFrameBits, n);
        b.push(f.data(), kFrameBits, 0);
    }
    CHECK(near(b.offsetSeconds(), 0.0), "live: offset_s bleibt 0");
    CHECK(near(b.bufferedSeconds(), 2.4), "live: der Ring fuellt sich trotzdem");
    std::vector<uint8_t> out;
    CHECK(!b.pop(out), "live: nichts zu lesen (Lesezeiger = Schreibzeiger)");
    std::printf("live folgt: ok\n");
}

static void testOverflow() {
    TimeshiftBuffer b;
    b.setCapacitySeconds(60);        // 2500 Rahmen
    b.setFrameBits(kFrameBits);
    b.setMode(TimeshiftMode::Paused);
    const uint32_t cap = b.capacityFrames();
    for (uint64_t n = 0; n < cap + 500; ++n) {
        auto f = makeFrame(kFrameBits, n);
        b.push(f.data(), kFrameBits, 0);
    }
    CHECK(b.size() == cap, "Ring haelt genau die Kapazitaet");
    CHECK(near(b.bufferedSeconds(), 60.0), "buffered_s = Kapazitaet");
    // Der Lesezeiger stand auf 0 und ist mitgerueckt -> Versatz = Kapazitaet
    CHECK(near(b.offsetSeconds(), 60.0), "Lesezeiger rueckt bei Ueberlauf mit");
    std::vector<uint8_t> out;
    CHECK(b.pop(out), "aeltester Rahmen lesbar");
    CHECK(out == makeFrame(kFrameBits, 500), "aeltester Rahmen ist Nr. 500");
    std::printf("Ueberlauf: ok\n");
}

static void testSeekSkip() {
    TimeshiftBuffer b;
    b.setCapacitySeconds(120);
    b.setFrameBits(kFrameBits);
    // 40 s Inhalt
    const uint64_t n40 = 40000 / 24;
    for (uint64_t n = 0; n < n40; ++n) {
        auto f = makeFrame(kFrameBits, n);
        b.push(f.data(), kFrameBits, 0);
    }
    b.toLive();
    CHECK(near(b.offsetSeconds(), 0.0), "toLive: Versatz 0");
    double off = b.seekSeconds(10.0);
    CHECK(near(off, 10.008, 0.03), "seek 10 s");
    CHECK(near(b.offsetSeconds(), off), "offsetSeconds passt zum seek");
    off = b.skipSeconds(-3.0);       // weiter zurueck
    CHECK(near(off, 13.0, 0.03), "skip -3 -> 13 s");
    off = b.skipSeconds(5.0);        // Richtung live
    CHECK(near(off, 8.0, 0.03), "skip +5 -> 8 s");
    off = b.skipSeconds(100.0);      // ueber live hinaus
    CHECK(near(off, 0.0), "skip ueber live -> 0");
    off = b.seekSeconds(999.0);      // mehr als der Inhalt
    CHECK(near(off, b.bufferedSeconds()), "seek wird auf buffered_s geklemmt");
    off = b.seekSeconds(-5.0);
    CHECK(near(off, 0.0), "negativer seek -> live");
    std::printf("Seek/Skip: ok\n");
}

static void testCopyRange() {
    TimeshiftBuffer b;
    b.setCapacitySeconds(120);
    b.setFrameBits(kFrameBits);
    const uint64_t total = 40000 / 24;
    for (uint64_t n = 0; n < total; ++n) {
        auto f = makeFrame(kFrameBits, n);
        b.push(f.data(), kFrameBits, 1000 + static_cast<int64_t>(n));
    }
    std::vector<uint8_t> packed;
    int64_t firstUnix = 0;
    uint64_t frames = b.copyRange(8.0, 2.0, packed, &firstUnix);
    CHECK(frames == 250, "8..2 s = 6 s = 250 Rahmen");
    CHECK(packed.size() == frames * b.packedBytes(), "Groesse der Kopie");
    // Der erste kopierte Rahmen ist total - 8 s
    const uint64_t firstIdx = total - 8000 / 24;
    CHECK(firstUnix == 1000 + static_cast<int64_t>(firstIdx), "Zeitstempel des ersten Rahmens");
    std::vector<uint8_t> frame(kFrameBits);
    TimeshiftBuffer::unpack(packed.data(), kFrameBits, frame.data());
    CHECK(frame == makeFrame(kFrameBits, firstIdx), "erster kopierter Rahmen stimmt");
    TimeshiftBuffer::unpack(packed.data() + (frames - 1) * b.packedBytes(), kFrameBits, frame.data());
    CHECK(frame == makeFrame(kFrameBits, firstIdx + frames - 1), "letzter kopierter Rahmen stimmt");
    CHECK(b.copyRange(2.0, 8.0, packed) == 0, "from_s <= to_s liefert nichts");
    CHECK(b.copyRange(5.0, 5.0, packed) == 0, "leerer Bereich liefert nichts");
    // Bereich groesser als der Inhalt wird geklemmt
    frames = b.copyRange(999.0, 0.0, packed);
    CHECK(frames == total, "geklemmt auf den Inhalt");
    std::printf("copyRange: ok\n");
}

static void testClearOnChange() {
    TimeshiftBuffer b;
    b.setCapacitySeconds(120);
    b.setFrameBits(kFrameBits);
    for (uint64_t n = 0; n < 100; ++n) {
        auto f = makeFrame(kFrameBits, n);
        b.push(f.data(), kFrameBits, 0);
    }
    b.setMode(TimeshiftMode::Paused);
    // Kapazitaetswechsel legt den Ring neu an
    CHECK(b.setCapacitySeconds(240), "geaenderte Kapazitaet meldet true");
    CHECK(b.size() == 0 && b.writeIndex() == 0, "neue Kapazitaet leert den Ring");
    CHECK(b.mode() == TimeshiftMode::Live, "nach dem Leeren wieder live");
    CHECK(!b.setCapacitySeconds(240), "gleiche Kapazitaet meldet false");
    // Dienstwechsel (andere Bitrate) leert ebenfalls
    for (uint64_t n = 0; n < 100; ++n) {
        auto f = makeFrame(kFrameBits, n);
        b.push(f.data(), kFrameBits, 0);
    }
    b.setFrameBits(96 * 24);
    CHECK(b.size() == 0, "Dienstwechsel leert den Ring");
    CHECK(b.packedBytes() == 288, "96 kbit/s = 288 Byte je Rahmen");
    // clear() ohne Groessenwechsel
    for (uint64_t n = 0; n < 10; ++n) {
        auto f = makeFrame(96 * 24, n);
        b.push(f.data(), 96 * 24, 0);
    }
    b.clear();
    CHECK(b.size() == 0 && b.writeIndex() == 0 && b.liveUnix() == 0, "clear leert alles");
    // Ohne Dienst tut der Ring nichts
    b.setFrameBits(0);
    CHECK(!b.active(), "ohne Dienst inaktiv");
    std::vector<uint8_t> out;
    CHECK(!b.pop(out), "ohne Dienst kein pop");
    std::printf("Leeren: ok\n");
}

int main() {
    testGeometry();
    testRoundTrip();
    testLiveFollows();
    testOverflow();
    testSeekSkip();
    testCopyRange();
    testClearOnChange();
    if (failures) { std::printf("%d Fehler\n", failures); return 1; }
    std::printf("OK\n");
    return 0;
}
