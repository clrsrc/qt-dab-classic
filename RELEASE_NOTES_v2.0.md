# Qt-DAB Custom Build v2.0 — Release Notes

*Basis:* Qt-DAB 6.10 von Jan van Katwijk (Lazy Chair Computing), GPLv2.

## Highlights

- **Classic Compact UI** (inspiriert von klassischen skinbaren Media-Playern) — frameless Shell, Dark-Theme, Playlist-Panel,
  Sender-Logo + Live-MOT-Slide, DLS-Ticker, EWF/SYNC/MOT-Statusleiste
- **Einheitliches Timer-System** — 4 Typen in einem Modell, JSON-Persistenz,
  Konflikt-Dialog, „Läuft"-Status
- **EPG-Browser** mit Umschalt- und Aufnahme-Trigger, UTC→Lokalzeit,
  `shortDescription`/`longDescription`-Bugfix im XML-Parser
- **Aufnahmefunktion** manuell / EPG / Timer, Aufnahme-Schutz gegen
  Senderwechsel, REC-Button rot während Aufnahme
- **EWF (Emergency Warning)** aus der FIB, persistenter Alarm bis
  bestätigt, akustisches Signal
- **v2.0 DSP-Optimierungen** — FFTW-Wisdom, Viterbi AVX2/SSE
  Runtime-Dispatch (Upstream lief komplett Scalar), FDK-AAC statt FAAD2,
  OFDM-Decoder-3 algebraisch vereinfacht, `sLevel`-Bug gefixt
- **RX-Optimierungen** — IQ-Equalizer, zweistufige DC-Removal,
  2× Oversampling, AFC 0.18, SNR-Filter 0.85/0.15, adaptive Gain,
  Scan-AMP-Retry gegen Übersteuerung
- **Portables Deployment** — `Qt-DAB-Start.bat` mit `HOME`-Override,
  Zadig beiliegend, kein Installer, keine Registry-Einträge

## Enthaltene Binärdatei

`Qt-DAB.exe` — Windows x64, UCRT64/MinGW-Build, dynamisch gegen Qt 6, FFTW,
FDK-AAC, PortAudio, libhackrf und libusb gelinkt (alle DLLs im Paket).

## Getestete Hardware

- HackRF One (Firmware aktuell)
- Windows 11 Home, CPU mit AVX2

## Bekannte Einschränkungen

- Im Bundesmux 5C/9B/11B senden 39 von 42 Programmen kein EPG
- Deutschlandradio sendet über den DAB-Äther aktuell keine
  `longDescription` (Internet-PI hat sie; Thema läuft mit DR)
- FFTW-Wisdom wird nur bei sauberem Beenden gespeichert (nicht bei
  `taskkill /F`)

## Danksagung

Dieser Build ist nur deshalb möglich, weil andere die fundamentale Arbeit
gemacht haben:

- **Jan van Katwijk** (Qt-DAB-Upstream, GPLv2)
- **Michael Ossmann / Great Scott Gadgets** (HackRF-Hardware, Firmware,
  libhackrf)
- **libusb, Zadig, Qt, Qwt, FFTW, FDK-AAC, FAAD2, PortAudio,** MSYS2/UCRT64
- **Anthropic Claude Code** als AI-Pair-Programmer für GUI-Redesign,
  Timer/EPG/Aufnahme/EWF, DSP-Optimierungen und Bugfixes

Vollständige Liste mit Lizenzen: siehe `THIRD_PARTY_NOTICES.md` im Paket.

## Lizenz

GPLv2 (wie Upstream).
