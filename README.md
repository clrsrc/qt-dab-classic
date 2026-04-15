# Qt-DAB v2.0 Custom Build — DAB+ Empfänger für HackRF One

Ein angepasster DAB+-Empfänger auf Basis des exzellenten
[Qt-DAB](https://github.com/JvanKatwijk/qt-dab) von **Jan van Katwijk**
(Lazy Chair Computing). Die DSP-Signalkette wurde optimiert, die Oberfläche
im Stil klassischer skinbarer Media-Player (Classic Compact UI) neu gestaltet und um mehrere praxisnahe Funktionen
erweitert.

> **Dieser Build ist kein Ersatz für Qt-DAB**, sondern eine Variante mit
> fokussiertem Feature-Set für den Heim-Empfang über HackRF One unter
> Windows. Für allen Code, der das eigentliche DAB-Empfangen ermöglicht,
> danke an Jan van Katwijk.

---

## Technische Änderungen gegenüber Upstream Qt-DAB 6.10

### GUI — Classic Compact UI (inspiriert von klassischen skinbaren Media-Playern)
- Neuer GUI-Modus (umschaltbar per `guiMode`, der klassische Qt-DAB-Modus bleibt erhalten)
- Frameless Shell, draggable, Dark-Theme, kompakte Hauptansicht
- Playlist-Panel mit Senderliste (Klick zum Umschalten)
- Sender-Logo (aus SPI-Cache, größtes PNG per SId) plus Live-MOT-Slide
  direkt neben dem Logo — nur echte Slides, keine Pause-/Werbebilder
- DLS-Ticker über eigenes `dlsText`-Signal
- Status-Leiste mit **EWF** (grün/rot-blinkend), **SYNC**, **MOT** per Polling
- Dunkle Titelleisten bei Sub-Fenstern (Windows DWM Dark-Mode-API)
- Sub-Fenster als `Qt::Tool` mit `WindowStaysOnTopHint`, nur ein
  Taskleisten-Eintrag
- Journaline-, HackRF-Control- und Device-Widget-Fenster beim Start
  unterdrückt
- Minimize/Close-Buttons mit ArrowCursor (kein Drag-Cursor)

### EPG-Browser
- Eigenständiges Fenster mit UTC→Lokalzeit-Konvertierung
- Sub-Einträge aus den Beschreibungen, Umlaut-Fix (Latin-1 / UTF-8)
- „Aktualisieren"-Button (lädt Daten ohne Fenster-Neustart)
- **Umschalten**-Button (sofort oder als Timer-Eintrag)
- **Aufnehmen**-Button (sofort bei laufender Sendung, Timer für zukünftige)
- **Bugfix:** Im Upstream-Parser (`xml-extractor.cpp`) wurden die Tags
  `shortDescription` und `longDescription` als `shortDescriptor` /
  `longDescriptor` gesucht — Beschreibungen kamen nie in der GUI an.
  Jetzt gefixt.

### Einheitliches Timer-System
- Vier Typen in einem Modell: `ManualSwitch`, `ManualRecord`,
  `EpgSwitch`, `EpgRecord`
- Alle Timer in einem Tab, sortiert nach Startzeit, Typ-Kennzeichnung,
  einzeln löschbar
- JSON-Persistenz (`Qt-DAB-timers.json` im portablen Datenordner)
- **Timer-Konflikt-Dialog:** wenn ein neuer Timer auf einen bereits
  belegten Zeitpunkt fällt, fragt ein Dialog welcher Timer genommen wird
- Timer-Status „Läuft" (rot) solange eine Aufnahme aktiv ist
- Service-Dropdown im Timer-Widget wird aus der Scanliste gefüllt

### Aufnahmefunktion
- Manuelle Aufnahme (REC-Button in der Shell)
- EPG-gesteuert (aus dem EPG-Browser)
- Timer-gesteuert
- **Aufnahme-Schutz:** während eine Aufnahme läuft, blockieren alle
  Umschalt-Aktionen (Senderwechsel würde die Aufnahme zerstören)
- REC-Button wird während der Aufnahme **rot** dargestellt

### EWF (Emergency Warning Functionality)
- Aktivierung des Alarm-Flags aus der FIB (im Upstream verworfen)
- Persistenter Alarm bis „Verstanden", akustisches Signal per Beep
- Status-Anzeige in der Hauptleiste

### RX-Empfangsoptimierungen
- IQ-Equalizer aktiv, mit Division-by-Zero-Guard, NaN-Guards,
  `isfinite`-Checks
- Zweistufige DC-Removal (1/8192 → 1/2048000), Reset bei Re-Enable
- HackRF-Bandbreite auf 1536 kHz (dynamisch an Samplerate gekoppelt)
- Default-Decoder: **DECODER_3** (synchronisiert zwischen `config-handler`
  und `ofdm-handler`)
- HackRF-Gain: LNA 40 / VGA 24, AMP OFF (8-bit ADC, LNA-betont)
- 2× Oversampling (HackRF 4.096 MHz → 2er-Mittelung → 2.048 MHz in den
  DAB-Decoder)
- AFC-Faktor 0.18 (konservativ, stabil)
- SNR-Filter 0.85 / 0.15 (stabiler als 0.8 / 0.2)
- Adaptive Gain: VGA automatisch per `adjustGain()` über
  `QueuedConnection` im GUI-Thread
- **Scan-AMP-Retry:** wenn beim Scan kein Signal gefunden wird, toggelt
  der Empfänger automatisch den HackRF-AMP und versucht erneut — Fix für
  Übersteuerung bei starken lokalen Muxen (z.B. Bundesmux 5C)
- Sämtliche 22 Findings des internen QA-Audits sind eingeflossen
  (`QA-Testreport.pdf`).

### v2.0 DSP-Signalketten-Optimierungen (Branch `v2.0-dev`)
- **FFT:** `FFTW_MEASURE` + Wisdom-Persistierung
  (`~/.qt-dab-fftw-wisdom`), `memcpy` statt Elementschleifen, Estimator als
  persistentes Member
- **Viterbi:** Umstellung von `viterbi-spiral/` (Scalar) auf `viterbi/`
  mit AVX2/SSE4.1-Runtime-Dispatch (`__ARCH_X86__` war im Upstream-Build
  nicht definiert — lief damit komplett Scalar; Faktor ~8–16× schneller)
- **AAC:** `FDK-AAC` (Fraunhofer) statt `FAAD2` aktiviert
- **OFDM decoder_3:** algebraisch vereinfacht (`normalize` und `jan_abs`
  heben sich auf), `conjVector` nur bei Display-Nutzung
- **Sample-Reader:** `sLevel`-Bug gefixt (nutzte `v_out[i]` statt
  `v_out[index+i]`), `dcRemoval` als `std::atomic<bool>`, IQ-Display aus
  dem Hot-Loop entfernt

### Portables Deployment
- `Qt-DAB-Start.bat` setzt `HOME` auf `data/` → EPG, Logos, Aufnahmen,
  Timer und INI bleiben alle im Portable-Ordner
- Zadig liegt zur einmaligen HackRF-Treiber-Einrichtung bei
- Kein Installer nötig, keine Registry-Einträge

---

## Installation

1. ZIP herunterladen und entpacken (beliebiger Ort, NTFS empfohlen)
2. HackRF One per USB anschließen
3. `zadig-2.9.exe` **als Administrator** starten, HackRF auf WinUSB
   umstellen (Details siehe `ANLEITUNG.txt`)
4. `Qt-DAB-Start.bat` ausführen

Die vollständige, bebilderte Einrichtung steht in `ANLEITUNG.txt`.

---

## Systemanforderungen

- Windows 10/11 (x64, AVX2 empfohlen für volle Viterbi-Performance)
- HackRF One (andere SDRs sind aus Upstream erreichbar, in diesem Build
  nicht getestet)
- Optional: DAB-Band-III-Bandpassfilter + moderate Vorverstärkung
  (Vorsicht: HackRF-ADC ist 8 Bit, zu viel Gain führt zu Übersteuerung)

---

## Danksagung / Third-Party

Eine vollständige Liste aller verwendeten Projekte, Bibliotheken und
Lizenzen steht in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

**Ohne die Arbeit anderer gäbe es diesen Fork nicht.** Das DAB-Know-how
stammt aus **Qt-DAB** von **Jan van Katwijk** (Lazy Chair Computing).
Die Hardware-Seite wäre ohne den offenen **HackRF** von
**Michael Ossmann / Great Scott Gadgets** und **libusb** nicht denkbar.
**Qt**, **Qwt**, **FFTW**, **FDK-AAC**, **FAAD2** und **PortAudio**
liefern den Rest der Laufzeit-Infrastruktur.

Die gesamte v2.0-Arbeit — GUI-Neugestaltung, Timer-System,
EPG-Browser inkl. Parser-Bugfix, Aufnahmefunktion, EWF-Monitor,
DSP-Optimierungen (FFTW-Wisdom, Viterbi-AVX2, FDK-AAC, OFDM-Decoder-3),
RX-Optimierungen mit 22-Findings-QA-Audit, Scan-AMP-Retry, portables
Deployment — ist in intensiver Paar-Programmierung mit **Claude Code**
(Anthropic, Modelle Opus 4.6 und Sonnet 4.6) entstanden. Der KI-Pair-
Programmer hat den Löwenanteil an Entwurf und Implementierung getragen,
inklusive Code-Review und Live-Empfangstests. Details dazu stehen in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

---

## Lizenz

**GNU General Public License v3 (GPLv3)** — siehe Datei `LICENSE`.
Das Upstream-Projekt Qt-DAB steht unter *„GPLv2 or any later version"*;
dieser Fork nutzt die Upgrade-Option. Die angepassten Quellen sind unter
https://github.com/clrsrc/qt-dab-classic verfügbar.
