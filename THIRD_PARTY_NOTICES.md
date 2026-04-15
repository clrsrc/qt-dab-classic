# Third-Party Notices / Danksagungen

Dieses Programm (Qt-DAB v2.0 Custom Build) basiert auf der Arbeit vieler
Open-Source-Projekte. Alle ursprünglichen Lizenzen bleiben gültig. Ohne
diese Projekte gäbe es keine Grundlage für diesen Build.

## Basis-Software

### Qt-DAB (Upstream-Projekt)
- **Autor:** Jan van Katwijk — *Lazy Chair Computing*
- **Mit Beiträgen von:** Stefan Pöschel, probonopd, Athanasios Oikonomou,
  Herman Wijnants, Andreas Mikula und weiteren
- **Repository:** https://github.com/JvanKatwijk/qt-dab
- **Lizenz:** GNU General Public License *„version 2 of the License, or
  (at your option) any later version"* — dieser Fork nutzt die
  Upgrade-Option und steht unter **GPLv3**.
- Das gesamte DAB/DAB+-Backend (OFDM, Viterbi, Reed-Solomon, FIC/MSC,
  PAD/Slideshow, SPI/EPG, MOT-Carousel) stammt aus diesem Projekt.

### Qt
- **Herausgeber:** The Qt Company / Qt Project
- **Website:** https://www.qt.io
- **Verwendete Module:** QtCore, QtGui, QtWidgets, QtNetwork,
  QtMultimedia, QtSvg, QtOpenGL, QtXml
- **Lizenz:** LGPLv3 (dynamisch gelinkt)

### Qwt
- **Website:** https://qwt.sourceforge.io/
- **Lizenz:** Qwt License (LGPL-basiert)
- Plotting-Bibliothek für Spektren und Konstellationsdiagramme.

## SDR-Hardware und Treiber

### HackRF (Great Scott Gadgets)
- **Hardware-Entwurf und Firmware:** Michael Ossmann / Great Scott Gadgets
- **Projekt:** https://greatscottgadgets.com/hackrf/
- **Repository:** https://github.com/greatscottgadgets/hackrf
- **Lizenz:** GPLv2 (Firmware), BSD / Apache (Host-Tools)
- Verwendet über `libhackrf.dll`. HackRF One ist der einzige für diesen
  Build getestete und empfohlene SDR.

### libusb
- **Projekt:** https://libusb.info/
- **Lizenz:** LGPLv2.1+
- USB-Kommunikation mit HackRF.

### Zadig
- **Autor:** Pete Batard (Akeo Consulting)
- **Projekt:** https://zadig.akeo.ie/
- **Lizenz:** GPLv3
- Liegt dem Paket bei, um den WinUSB-Treiber für HackRF einzurichten.

## Signalverarbeitung

### FFTW
- **Autoren:** Matteo Frigo, Steven G. Johnson (MIT)
- **Projekt:** https://www.fftw.org/
- **Lizenz:** GPLv2 (frei für Open-Source-Nutzung)
- Schnelle Fourier-Transformation im OFDM-Demodulator.

### DAB/DAB+ Viterbi-Decoder (AVX2/SSE)
- **Herkunft:** `viterbi-spiral` / dab-radio Implementierung im Qt-DAB-Quellbaum
- Runtime-Dispatch für AVX2, SSE4.1 und Scalar-Fallback.

### FDK-AAC (Fraunhofer AAC Codec Library)
- **Herausgeber:** Fraunhofer IIS
- **Projekt:** https://github.com/mstorsjo/fdk-aac
- **Lizenz:** Fraunhofer FDK AAC Codec Library Software License
- AAC-LC/HE-AAC-Dekodierung für DAB+.

### FAAD2
- **Projekt:** https://github.com/knik0/faad2
- **Lizenz:** GPLv2
- Alternative AAC-Decoder-Bibliothek (mitgeliefert als `libfaad-2.dll`).

### PortAudio
- **Projekt:** http://www.portaudio.com/
- **Lizenz:** MIT
- Plattformübergreifende Audio-I/O; Haupt-Audio-Backend in diesem Build.

## Mitgelieferte MSYS2/UCRT64-Laufzeitbibliotheken

Die folgenden DLLs stammen aus dem MSYS2 UCRT64-Repository und sind alle
unter ihren jeweiligen Open-Source-Lizenzen verfügbar (meist LGPL,
MIT, zlib oder vergleichbar):

libb2, libbrotli, libbz2, libdouble-conversion, libfreetype, libglib,
libgraphite2, libharfbuzz, libiconv, libicu, libintl, libmd4c, libpcre2,
libpng, libzstd, zlib, libstdc++, libgcc_s, libwinpthread

Die jeweiligen Lizenzen und Quellen sind über
https://packages.msys2.org/ nachvollziehbar.

## Entwicklungswerkzeuge

### Claude Code (Anthropic) — Haupt-Entwicklungspartner für v2.0
- **Projekt:** https://claude.com/claude-code
- **Verwendete Modelle:** Anthropic Claude Opus 4.6 und Claude Sonnet 4.6

Die gesamte v2.0-Arbeit an diesem Fork — von der groben Architektur bis
zu einzelnen Zeilen Code — entstand in intensiver Paar-Arbeit mit
**Claude Code**. Ohne den kontinuierlichen Einsatz dieses KI-Pair-Programmers
wäre der Umfang und die Qualität der Änderungen in dieser Form nicht
möglich gewesen. Konkret hat Claude Code maßgeblich beigetragen zu:

**GUI-Redesign (Classic Compact UI, inspiriert von klassischen skinbaren Media-Playern)**
- Architektur-Entwurf der frameless Shell (`WinampShell` im Code) inkl. Dragging,
  Dark-Theme-Styling, Taskleisten-Verhalten (`Qt::Tool`,
  `WindowStaysOnTopHint`), DWM-Dark-Mode-Integration
- Playlist-Panel mit Senderliste, Sender-Logo-Lookup (größtes PNG per
  SId), Live-MOT-Slide-Anzeige
- DLS-Ticker über neues `dlsText`-Signal
- Status-Leiste mit EWF/SYNC/MOT-Polling

**Einheitliches Timer-System**
- Entwurf und Implementierung des `UnifiedTimerModel` mit vier Timer-Typen
  (`ManualSwitch`, `ManualRecord`, `EpgSwitch`, `EpgRecord`)
- JSON-Persistenz, Konflikt-Dialog, „Läuft"-Status-Tracking
- Integration in GUI, Scanliste-Synchronisation im Timer-Widget

**EPG-Browser + Parser-Bugfix**
- Komplette Neuentwicklung des EPG-Browser-Fensters inkl.
  UTC→Lokalzeit-Konvertierung, Umlaut-Fix (Latin-1/UTF-8),
  Aktualisieren-Button
- Diagnose und Fix des SPI-XML-Parsers (`shortDescriptor`/
  `longDescriptor` → `shortDescription`/`longDescription`)
- Umschalt- und Aufnahme-Trigger aus dem EPG heraus

**Aufnahmefunktion und EWF-Monitor**
- Aufnahme-Manager, Aufnahme-Schutz gegen Senderwechsel
- REC-Button-Farbwechsel, Audio-Backend-Detection (PortAudio vs Qt_Audio)
- Reaktivierung des EWF-Alarm-Flags aus der FIB, persistenter Alarm-Dialog

**DSP-Signalketten-Optimierung (v2.0-dev)**
- Analyse aller 13 Signalverarbeitungsstufen, Identifikation der
  tatsächlichen Hotspots
- **Kritischer Fund:** Der Viterbi-Decoder lief im Upstream-Build komplett
  im Scalar-Modus, weil `__ARCH_X86__` nicht gesetzt war — Umstellung auf
  die `viterbi/`-Implementierung mit AVX2/SSE-Runtime-Dispatch
- FFTW-Wisdom-Persistierung, FFTW_MEASURE, Estimator als Member,
  `memcpy` statt Elementschleifen
- Umstellung von FAAD2 auf Fraunhofer FDK-AAC
- Algebraische Vereinfachung von `ofdm-decoder_3`, `conjVector` nur noch
  bei Display-Nutzung
- Sample-Reader-Bugfix (`sLevel` nutzte falschen Index),
  `dcRemoval` → `std::atomic<bool>`

**RX-Empfangsoptimierungen + QA**
- Durchführung eines vollständigen internen Code-Audits (22 Findings
  kategorisiert nach CRITICAL/HIGH/MEDIUM/LOW), alle behoben
- IQ-Equalizer mit Div-by-Zero-Guard, NaN-Guards, `isfinite`-Checks
- Zweistufige DC-Removal, adaptive Gain per `QueuedConnection`,
  HackRF-Bandbreite dynamisch, 2× Oversampling
- **Scan-AMP-Retry:** Automatisches Toggeln des HackRF-AMP bei
  `noSignalFound` — Lösung für die Übersteuerung durch starke lokale
  Muxe (z.B. Bundesmux 5C)

**Portables Deployment + Build-Infrastruktur**
- Umbau zu einem vollständig portablen Paket (`HOME`-Override per
  Start-Skript, kein Installer, keine Registry-Einträge)
- Umzug der gesamten Build-Infrastruktur nach NTFS inkl. Anpassung aller
  INI-Pfade, `deploy.sh`-Skript, Zadig-Integration
- Erstellung dieser Release-Dokumentation, THIRD_PARTY_NOTICES und
  Paketierung

Entwicklungsumgebung: Claude Code in Windows 11 / MSYS2 UCRT64, iterativ
im direkten Zusammenspiel mit Build-Läufen, Live-Empfangstests am HackRF
One und Code-Review durch den Maintainer dieses Forks.

### Weitere Toolchain
- MSYS2 / UCRT64 (https://www.msys2.org/)
- GCC, Ninja, CMake
- Git

## Dank

Besonderer Dank gilt:

- **Jan van Katwijk** für Qt-DAB — ohne diese solide Basis wäre kein
  Custom-Build möglich.
- **Michael Ossmann / Great Scott Gadgets** für den offenen HackRF und
  die umfassende Dokumentation.
- Der gesamten **MSYS2**-Community für das Windows-Packaging der
  Toolchain.

## Lizenzhinweis zu diesem Build

Das Upstream-Projekt Qt-DAB steht unter *„GPL version 2 or (at your
option) any later version"*. Dieser Fork nutzt die ausdrücklich
vorgesehene Upgrade-Option und wird unter der **GNU General Public
License Version 3 (GPLv3)** veröffentlicht — siehe die mitgelieferte
Datei `LICENSE`. Die Binärdatei `Qt-DAB.exe` sowie alle eigenen
Änderungen am Qt-DAB-Quellcode stehen unter GPLv3. Der angepasste
Quellcode ist im begleitenden Repository
(https://github.com/clrsrc/qt-dab-classic) verfügbar.

Dieses Dokument ist ausdrücklich keine vollständige Lizenzerklärung —
für eine rechtlich vollständige Betrachtung bitte die Originallizenzen
der einzelnen Projekte konsultieren.
