# DAB Classic v3.0 – Release Notes

*September 2026. Neuer Unterbau: Qt-freier C++-Empfangskern als eigener Prozess,
Anwendungslogik in Rust, Oberfläche mit Tauri 2 und Svelte 5. Der Empfangspfad
geht auf Qt-DAB von Jan van Katwijk (Lazy Chair Computing) zurück. Lizenz GPLv3.*

## Highlights

- **Neue Architektur** – Empfangskern `dabcored.exe` (C++17, ohne Qt), Rust-
  Workspace mit Kommando/Ereignis-Protokoll (JSON-Zeilen), Oberfläche als
  Tauri-2-App. Stürzt der Kern, startet die App ihn neu; die Oberfläche bleibt.
- **Empfang** – HackRF One und RTL-SDR, SNR-gesteuerte AGC, Band-III-Scan mit
  Übersteuerungsschutz, PPM-Korrektur, HackRF-Referenztakt (CLKIN/GPSDO),
  Datei-Wiedergabe von `.uff`/`.iq`-Mitschnitten.
- **Classic Compact UI** – rahmenloses Fenster, zehn Stationsspeicher mit
  Kurzlabels, Tabs Ensemble/Senderliste/Scan, Display mit Logo, Slideshow
  (vergrößerbar) und Bilderstreifen, DLS/DL+ mit klickbaren Links, Programmtyp
  und Sprache aus dem FIC, Deutsch/Englisch.
- **Timeshift** – Puffer bis 240 Minuten im Arbeitsspeicher, Pause, ±30 s,
  zurück zu live; rückwirkende Aufnahme.
- **Aufnahme und Timer** – WAV-Aufnahme mit Umschaltsperre, Timer manuell oder
  aus dem EPG mit Vor-/Nachlauf, Sleep-Timer, Übernahme der Timer und
  Favoriten aus v2.
- **EPG und Logos** – Broadcast-EPG (SPI) mit Jetzt/Danach, Senderlogos; optional
  Hybrid Radio per RadioDNS/SPI über IP (Standard aus, die App spricht sonst
  nicht mit dem Internet).
- **Notfallwarnung (EWS)** – FIG 0/15 mit Alarmfenster, Warnstufe, Ortscodes in
  Klartext, Ortsabgleich gegen die Heimatkoordinaten (ETSI TS 104 089),
  automatisches Umschalten auf den Warndienst, EWF-Historie, Mitschnitt.
- **Verkehr** – Verkehrs- und Sonderdurchsagen (FIG 0/18, 0/19) mit Liste,
  Autoradio-Funktion „umschalten“ und Mitschnitt als MP3; TPEG-Verkehrs-
  meldungen (ISO 21219 TEC, ETSI TS 103 551) mit Wirkung, Ursache, Spuren,
  Länge, Verzögerung, Autobahn und Anschlussstellen aus einer eingebauten
  OpenStreetMap-Tabelle, Entfernung zum Heimatort, Filter, Karten-Link.
- **Musik-Trennung** (optional) – Titelerkennung aus DL+/DLS, Schnitt aus dem
  Timeshift-Puffer, Vorhören, verschiebbare Schnittmarken, MP3 mit ID3-Tags
  und Cover.
- **Audio** – Ausgabegerät per WASAPI-Kennung wählbar, „Standard“ folgt dem
  Windows-Standardgerät zur Laufzeit.
- **TII und Debug** – Sendestandorte mit Entfernung und Azimut, DX-Protokoll,
  Spektrum, Konstellation, SNR-Verlauf, Fehlerzähler.
- **Speicherplatz** – Obergrenze für Durchsage-Mitschnitte (Standard 50 Dateien
  oder 200 MB), Anzeige der Ordnerbelegung, EPG-Cache wird beim Start beschnitten.
- **Portabel** – `data\` neben der EXE, keine Einträge im Benutzerprofil, kein
  Installer, kein Registry-Eintrag.

## Pakete

| Datei | Inhalt |
|---|---|
| `DAB-Classic-v3.0.0-portable-win64.zip` | Vollpaket mit WebView2-Laufzeit (Fixed Version) im Ordner `webview2\`, läuft auf jedem Windows 10/11 x64 |
| `DAB-Classic-v3.0.0-portable-win64-lite.zip` | Ohne WebView2-Laufzeit, nutzt die vom System (Microsoft Edge) bereitgestellte Evergreen-Laufzeit |

Beide Pakete enthalten `dab-classic.exe`, den Empfangskern `core\` mit
Bibliotheken, die TII-Senderdatenbank `tii\`, `zadig-2.9.exe` für die
USB-Treiber, `ANLEITUNG.txt`, `LICENSE`, `THIRD_PARTY_NOTICES.md` und diese
Release Notes. Entpacken, `dab-classic.exe` starten.

## Systemanforderungen

- Windows 10/11, 64 Bit; CPU mit AVX2 empfohlen (Viterbi-Decoder mit
  SSE/AVX2-Laufzeitauswahl)
- HackRF One oder RTL-SDR (R820T/R828D) mit WinUSB-Treiber (Zadig liegt bei)
- WebView2-Laufzeit: im Vollpaket enthalten, beim Lite-Paket vom System.
  Windows 10 mit dem Vollpaket: einmalig `webview2-rechte-win10.cmd` ausführen
  (Leserechte für App-Container auf `webview2\`, Vorgabe von Microsoft für
  Fixed-Version-Laufzeiten ab Version 120; Windows 11 braucht das nicht)

## Umstieg von v2.0

- Die Timer-Datei der v2 (`.qt-dab-timers.json`) wird beim ersten Start
  übernommen, wenn noch keine `data\timers.json` vorhanden ist; Favoriten
  lassen sich über „Importieren“ im Speicher-Panel übernehmen.
- Der Datenordner heißt `data\` neben der EXE; Aufnahmen liegen in
  `data\recordings\` oder im gewählten Aufnahmeordner.

## Hinweise

- Im Bundesmux (5C) senden die meisten Programme kein Broadcast-EPG; die
  Internet-Ergänzung per RadioDNS ist optional zuschaltbar.
- TPEG-Verkehrsmeldungen sendet in NRW der WDR (11D „WDR NRW“, 9A „WDR
  Regional“); der Bundesmux hat keinen TPEG-Dienst.
- AAC-Dekodierung mit FAAD2; liegt `libfdk-aac-2.dll` im Ordner `core\`, wird
  FDK-AAC zur Laufzeit verwendet.
- Die Musik-Trennung ist standardmäßig aus und wird in den Einstellungen
  eingeschaltet.
