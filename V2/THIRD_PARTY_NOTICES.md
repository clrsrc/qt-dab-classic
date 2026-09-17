# Third-Party Notices / Danksagungen – DAB Classic v3.0

DAB Classic v3.0 steht unter der GNU General Public License v3 (siehe
`LICENSE`). Das Programm baut auf der Arbeit vieler Projekte auf. Dieses
Dokument nennt sie und ihre Lizenzen; die vollständigen Lizenztexte liegen
bei den jeweiligen Projekten.

## Basis-Software

### Qt-DAB (Jan van Katwijk, Lazy Chair Computing)

- https://github.com/JvanKatwijk/qt-dab
- Lizenz: GPL v2 „or (at your option) any later version“
- Der Empfangskern `core-cpp/libdabcore` (OFDM-Synchronisation, FIC/FIB-
  Dekodierung, MSC-Backend mit Faltungs- und Reed-Solomon-Decoder, DAB+-
  Superframes, PAD/DLS/DL+/MOT, EPG-Compiler, TII-Detektor, HackRF- und
  RTL-SDR-Anbindung, XML-Dateiformat `.uff`) ist eine Qt-freie Portierung des
  Qt-DAB-Empfangspfads. Alle Urheberrechte der Originalautoren bleiben
  anerkannt; die Portierung nutzt die in Qt-DAB vorgesehene Upgrade-Option auf
  GPLv3.

### In Qt-DAB enthaltene Fremdanteile, die übernommen wurden

- **Viterbi-Decoder** – Phil Karn, KA9Q (libfec, https://github.com/ka9q/libfec),
  LGPL; SIMD-Varianten (SSE/AVX2) und Umbau in Klassen von William Yang (2023).
- **Reed-Solomon-Decoder** – Phil Karn, KA9Q (2002), GPL.
- **librtlsdr-Header** – Osmocom rtl-sdr (Steve Markgraf, Dimitri Stolnikov,
  Hoernchen), GPL v2 oder später.
- **libhackrf-Header** – Great Scott Gadgets, GPL v2 oder später.
- **TII-Senderdatenbank** `tii\txdata.tii` – Datenbasis aus dem Qt-DAB-Umfeld.

## Mitgelieferte Bibliotheken (Ordner `core\`)

| Bibliothek | Zweck | Lizenz |
|---|---|---|
| FFTW 3 (`libfftw3f-3.dll`) | FFT der OFDM-Demodulation | GPL v2 oder später |
| FAAD2 (`libfaad-2.dll`) | AAC-Dekodierung (DAB+) | GPL v2 oder später |
| LAME (`libmp3lame-0.dll`) | MP3-Export (Musik, Durchsagen) | LGPL v2 |
| PortAudio (`libportaudio.dll`) | Audioausgabe (WASAPI) | MIT |
| libhackrf (`libhackrf.dll`) | HackRF One | GPL v2 oder später |
| librtlsdr (`librtlsdr.dll`) | RTL-SDR (Osmocom-Release) | GPL v2 oder später |
| libusb (`libusb-1.0.dll`) | USB-Zugriff | LGPL v2.1 oder später |
| zlib | TPEG-Dienstrahmen, statisch | Zlib |
| nlohmann/json | JSON-Protokoll des Kerns, statisch | MIT |
| GCC/MinGW-w64-Laufzeit (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`) | C/C++-Laufzeit (MSYS2 UCRT64) | GPL v3 mit GCC Runtime Library Exception; winpthread MIT/BSD |

FDK-AAC (Fraunhofer) wird nicht mitgeliefert. Liegt `libfdk-aac-2.dll` im
Ordner `core\`, lädt der Kern sie zur Laufzeit und dekodiert damit AAC
(Lizenz: Fraunhofer FDK AAC Codec Library for Android, siehe dort).

## Oberfläche und Anwendungslogik

### Tauri 2

- https://tauri.app – Lizenz MIT / Apache-2.0
- Plugins `tauri-plugin-dialog`, `tauri-plugin-opener` (MIT / Apache-2.0)

### Microsoft Edge WebView2

- `WebView2Loader.dll` (WebView2 SDK) und, nur im Vollpaket, die Fixed-Version-
  Laufzeit 153.0.4234.32 im Ordner `webview2\`
- Weitergabe gemäß den Lizenzbedingungen von Microsoft für das WebView2 SDK
  und die WebView2 Runtime (https://developer.microsoft.com/microsoft-edge/webview2/)
- Das Lite-Paket nutzt die auf dem System installierte Evergreen-Laufzeit.

### Rust-Crates

Der Rust-Workspace (`crates/`, `apps/desktop/src-tauri`) bindet u. a. serde,
serde_json, chrono, clap, crossbeam-channel, flate2, hickory-resolver, ureq,
base64, anyhow, thiserror, log, env_logger und windows-sys ein; insgesamt
rund 480 Crates. Alle stehen unter freizügigen Lizenzen (MIT, Apache-2.0,
BSD-2/3-Clause, Zlib, ISC, MPL-2.0, Unicode-3.0, CDLA-Permissive-2.0). Die
vollständige Liste liefert `cargo metadata` bzw. `cargo tree`.

### Svelte / SvelteKit / Vite / TypeScript

- Svelte 5, SvelteKit 2, Vite 8, TypeScript, svelte-check – Lizenz MIT
  (TypeScript: Apache-2.0)
- `@tauri-apps/api` und Plugin-Bindings – MIT / Apache-2.0

## Daten

### OpenStreetMap

- Autobahnnummern und Anschlussstellen der TPEG-Meldungen stammen aus einer
  eingebauten Tabelle (`crates/dab-app/data/autobahnen.bin`), erzeugt mit
  `tools/build-autobahnen.py` aus OpenStreetMap-Daten (Stand Mai 2026).
- © OpenStreetMap-Mitwirkende, Open Database License (ODbL) 1.0,
  https://www.openstreetmap.org/copyright

### TPEG

- Struktur der TPEG2-Dekodierung (ISO 21219, TEC/MMC/OpenLR) nach den
  öffentlichen Unterlagen von TISA (Traveller Information Services Association,
  https://github.com/tisa-asbl/TPEG2) und der Referenzimplementierung
  `tpeg-item` (https://github.com/fenghlkevin/tpeg-item, Apache-2.0), gegen die
  die eigene Implementierung abgeglichen wurde.

## Werkzeuge im Paket

### Zadig

- `zadig-2.9.exe` – Pete Batard / Akeo Consulting, https://zadig.akeo.ie
- Lizenz GPL v3; dient der einmaligen Einrichtung des WinUSB-Treibers für
  HackRF One und RTL-SDR.

## Entwicklung

### Claude Code (Anthropic)

DAB Classic v3.0 ist in enger Zusammenarbeit mit Claude Code entstanden, dem
KI-Pair-Programmer von Anthropic (u. a. Claude Fable 5.1). Claude Code hat die
Qt-freie Portierung des Empfangskerns, das Kommando/Ereignis-Protokoll, die
Rust-Anwendungslogik (Timer, Timeshift, Musik-Trennung, EWS-Ortsabgleich,
RadioDNS, Verkehrsfunk, TPEG-Dekoder), die Tauri/Svelte-Oberfläche, die
Tests, das Portable-Deployment und die Dokumentation maßgeblich erarbeitet;
Anforderungen, Tests am Empfänger, Entscheidungen und Abnahme lagen bei
Stefan Kammann (DM2TIM).

### Weitere Toolchain

MSYS2 UCRT64 (GCC, CMake, Ninja, pkg-config), Rust (MSVC-Toolchain), pnpm,
Node.js, PowerShell.

## Dank

- **Jan van Katwijk** für Qt-DAB – ohne diese Basis gäbe es keinen Empfangskern.
- **Michael Ossmann / Great Scott Gadgets** für HackRF One und libhackrf.
- **Osmocom** für rtl-sdr.
- Der **MSYS2**-Community für das Windows-Packaging der Toolchain.
- Den **OpenStreetMap**-Mitwirkenden für die Straßendaten.
- **Deutschlandradio** und der **ARD** für den Betrieb von EPG, EWS/ASA und
  TPEG im DAB+-Netz.

## Lizenzhinweis zu diesem Paket

DAB Classic v3.0 (Programm `dab-classic.exe`, Empfangskern `dabcored.exe`,
Quellcode im Ordner `V2/` des Repositorys
https://github.com/clrsrc/qt-dab-classic) steht unter der GNU General Public
License Version 3 (GPLv3), siehe `LICENSE`. Die mitgelieferten Bibliotheken
und Werkzeuge behalten ihre jeweiligen Lizenzen. Dieses Dokument ersetzt
keine rechtliche Prüfung; maßgeblich sind die Originallizenzen der einzelnen
Projekte.
