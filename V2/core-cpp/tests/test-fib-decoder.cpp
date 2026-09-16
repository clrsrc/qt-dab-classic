// DAB Classic v3 - fibDecoder ohne Empfang: synthetische FIBs (1 Bit je
// Byte, wie sie der ficHandler nach Viterbi/CRC liefert) durch processFIB.
// Prueft die Befunde des Reviews vom 16.09.2026:
//   M4  FIG 0/1: Subkanal mit startAddr + Length > 864 CU wird verworfen,
//       864 genau ist erlaubt
//   G1  FIG 0/9: negative LTO kommt vorzeichenrichtig an (-1, nicht 255)
//   G5  FIG 0/2 mit 0 Komponenten: getServiceComp/is_SPI liefern -1/false
//       statt auf einen leeren Vektor zuzugreifen
//   G9  FIG 0/1 mit zu kurzer Laenge legt keinen Eintrag an
//   Verkehrsfunk: announcement-Callback traegt SId, Flags (16 Bit),
//       Cluster und den Subkanal der Durchsage aus FIG 0/19
#include "fib-decoder.h"
#include "receiver-callbacks.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// FIB-Bauer: 30 Byte Nutzdaten (+ 2 Byte CRC, hier ungenutzt) als 256 Bits
struct FibBuilder {
    uint8_t bits[256];
    int pos = 0;   // Bit-Position
    FibBuilder() { std::memset(bits, 0, sizeof(bits)); }
    void put(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) bits[pos++] = (v >> i) & 1;
    }
    // FIG-Kopf: Typ (3), Laenge (5) = Bytes nach dem Kopfbyte
    void figHeader(int type, int length) { put(type, 3); put(length, 5); }
    // FIG 0 Byte 1: C/N, OE, P/D, Extension
    void fig0Byte1(int ext, int cn = 0, int oe = 0, int pd = 0) { put(cn, 1); put(oe, 1); put(pd, 1); put(ext, 5); }
    void endMarker() { put(7, 3); put(31, 5); }
};

struct Probe : public fibDecoder {
    explicit Probe(ReceiverCallbacks* cb) : fibDecoder(cb) {}
    void feed(FibBuilder& b) { processFIB(b.bits, 0); }
};

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++failures;
}

} // namespace

int main() {
    ReceiverCallbacks cb;
    int gotLto = 1000, gotEcc = -1;
    cb.ltoEcc = [&](int lto, int ecc) { gotLto = lto; gotEcc = ecc; };
    struct Ann { int sid, flags, cluster, subCh; };
    std::vector<Ann> anns;
    cb.announcement = [&](int sid, int flags, int cluster, int subCh) { anns.push_back({sid, flags, cluster, subCh}); };
    cb.log = [](const char* level, const std::string& t) { std::printf("  [%s] %s\n", level, t.c_str()); };

    Probe dec(&cb);
    dec.connectChannel();

    // --- FIB 1: FIG 0/1 mit drei Subkanaelen (EEP lange Form, Option A) ---
    {
        FibBuilder b;
        b.figHeader(0, 1 + 3 * 4);
        b.fig0Byte1(1);
        auto subCh = [&](int id, int start, int size) {
            b.put(id, 6); b.put(start, 10);
            b.put(1, 1);          // lange Form
            b.put(0, 3);          // Option A
            b.put(1, 2);          // Schutzstufe 1-A (Tabelle 12 CU je 8 kbit/s)
            b.put(size, 10);
        };
        subCh(1, 0, 96);          // gueltig
        subCh(2, 800, 100);       // 900 > 864 -> verwerfen (M4)
        subCh(3, 768, 96);        // 864 genau -> gueltig
        b.endMarker();
        dec.feed(b);
        check(dec.nrChannels() == 2, "M4: 2 von 3 Subkanaelen angelegt (CU 800+100 verworfen)");
        channel_data cd;
        dec.getChannelInfo(&cd, 0);
        check(cd.id == 1 && cd.start_cu == 0 && cd.size == 96 && cd.bitrate == 96, "M4: Subkanal 1 CU 0+96, 96 kbit/s");
        dec.getChannelInfo(&cd, 1);
        check(cd.id == 3 && cd.start_cu == 768 && cd.size == 96, "M4: Subkanal 3 CU 768+96 (Grenze 864 erlaubt)");
    }

    // --- FIB 2: FIG 0/1 zu kurz fuer einen Eintrag (G9) ---
    {
        FibBuilder b;
        b.figHeader(0, 3);        // 1 Byte Ext + 2 Byte "Eintrag" (Kurzform braucht 3)
        b.fig0Byte1(1);
        b.put(9, 6); b.put(100, 10);   // haette Subkanal 9 werden koennen
        b.endMarker();
        dec.feed(b);
        check(dec.nrChannels() == 2, "G9: abgeschnittener FIG-0/1-Eintrag legt keinen Subkanal an");
    }

    // --- FIB 3: FIG 0/9 mit LTO -1,5 h, ECC 0xE0 (G1) ---
    {
        FibBuilder b;
        b.figHeader(0, 4);
        b.fig0Byte1(9);
        b.put(0, 1);              // Ext flag
        b.put(0, 1);              // Rfa
        b.put(1, 1);              // LTO-Vorzeichen: negativ
        b.put(1, 4);              // 1 Stunde
        b.put(1, 1);              // halbe Stunde
        b.put(0xE0, 8);           // ECC
        b.put(0, 8);              // Inter. table id
        b.endMarker();
        dec.feed(b);
        check(gotLto == -1, "G1: LTO -1 h kommt als -1 an (nicht 255)");
        check(gotEcc == 0xE0, "G1: ECC 0xE0");
    }

    // --- FIB 4: FIG 0/2 ohne Komponenten (G5) und mit einer Komponente ---
    {
        FibBuilder b;
        b.figHeader(0, 1 + 3 + 5);
        b.fig0Byte1(2);
        b.put(0x1234, 16);        // SId ohne Komponenten
        b.put(0, 1); b.put(0, 3); b.put(0, 4);
        b.put(0xD210, 16);        // SId mit einer Audio-Komponente
        b.put(0, 1); b.put(0, 3); b.put(1, 4);
        b.put(0, 2);              // TMid Audio
        b.put(63, 6);             // ASCTy DAB+
        b.put(1, 6);              // Subkanal 1
        b.put(1, 1);              // primaer
        b.put(0, 1);              // CA
        b.endMarker();
        dec.feed(b);
        check(dec.getServiceComp(0x1234u, 0) == -1, "G5: SId ohne Komponenten -> getServiceComp -1");
        check(dec.is_SPI(0x1234u) == false, "G5: SId ohne Komponenten -> is_SPI false");
        check(dec.getServiceComp(0xD210u, 0) >= 0, "FIG 0/2: SId D210 hat eine Komponente");
        check(dec.getServiceComp(0xD210u, 5) == -1, "G5: Komponentennummer ausserhalb -> -1");
    }

    // --- FIB 5: FIG 0/18 (ASu) + FIG 0/19 (ASw) -> announcement-Callback ---
    {
        FibBuilder b;
        b.figHeader(0, 1 + 6);
        b.fig0Byte1(18);
        b.put(0xD210, 16);        // SId
        b.put(0x0003, 16);        // ASu: Alarm + Verkehr
        b.put(0, 5); b.put(1, 3); // Rfa, 1 Cluster
        b.put(7, 8);              // Cluster 7
        b.figHeader(0, 1 + 4);
        b.fig0Byte1(19);
        b.put(7, 8);              // Cluster 7
        b.put(0x0002, 16);        // ASw: Verkehr
        b.put(0, 1);              // New flag 0 (Wiederholung) - muss trotzdem melden
        b.put(0, 1);              // Region flag
        b.put(5, 6);              // Subkanal der Durchsage
        b.endMarker();
        dec.feed(b);
        check(anns.size() == 1, "Announcement: genau ein Ereignis");
        if (!anns.empty()) {
            check(anns[0].sid == 0xD210, "Announcement: sid = angekuendigter Dienst D210");
            check(anns[0].flags == 0x0002, "Announcement: kind = ASu & ASw = 0x0002 (Verkehr)");
            check(anns[0].cluster == 7, "Announcement: cluster 7");
            check(anns[0].subCh == 5, "Announcement: sub_ch 5 = Subkanal der Durchsage (FIG 0/19)");
        }
        // Wiederholung mit gleichen Flags: keine zweite Meldung; Ende (ASw 0): Meldung mit 0
        dec.feed(b);
        check(anns.size() == 1, "Announcement: Wiederholung wird nicht erneut gemeldet");
        FibBuilder e;
        e.figHeader(0, 1 + 4);
        e.fig0Byte1(19);
        e.put(7, 8); e.put(0x0000, 16); e.put(0, 1); e.put(0, 1); e.put(5, 6);
        e.endMarker();
        dec.feed(e);
        check(anns.size() == 2 && anns[1].flags == 0, "Announcement: Ende (ASw 0) wird gemeldet");
    }

    std::printf("%s (%d Fehler)\n", failures == 0 ? "ALLE TESTS OK" : "FEHLER", failures);
    return failures == 0 ? 0 : 1;
}
