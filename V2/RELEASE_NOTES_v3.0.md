# DAB Classic v3.0 – Release Notes

*September 2026. Neuer Unterbau: Qt-freier C++-Empfangskern als eigener Prozess,
Anwendungslogik in Rust, Oberfläche mit Tauri 2 und Svelte 5. Der Empfangspfad
geht auf Qt-DAB von Jan van Katwijk (Lazy Chair Computing) zurück. Lizenz GPLv3.*

## v3.0.2 – September 2026

- **Umschalten auf Stationsspeicher und Senderliste** – die Dienstauswahl
  wird im Empfangskern vorgemerkt und startet, sobald der Dienst in der FIC
  vollständig ist; der Kanalwechsel schickt die Auswahl sofort mit, der Ton
  beginnt damit häufig noch vor dem Sendernamen. Eine frühe Kein-Signal-
  Meldung beendet den Aufruf nicht mehr, es gilt allein die Wartezeit von
  8 s. Der gewählte Sender erscheint sofort in der Anzeige; verspätete
  Stop-Meldungen eines abgelösten Dienstes beeinflussen die Anzeige nicht
  mehr. Kann eine Auswahl nicht ausgeführt werden (Aufnahme, Scan), meldet
  die App das als „nicht gefunden".
- **AGC mit Übersteuerungsgrenze** – der Kern misst den Anteil der
  Rohsamples am Anschlag des ADC. Regelt ein starker Nachbarkanal den
  8-Bit-Wandler in die Begrenzung, senkt die AGC den Gain und merkt sich die
  Stufe bis zum nächsten Kanalwechsel als Obergrenze; der Verstärker (AMP)
  wird darüber nicht mehr versucht. Nach einer erschöpften Gain-Ramp startet
  ein Schein-Sync die Ramp nicht erneut. Neues Ereignis `adc_clip` im
  Protokoll (Anteil und Obergrenze, 1 Hz).
- **Halbband-FIR im HackRF-Pfad** – die 2:1-Dezimierung von 4,096 auf
  2,048 MS/s nutzt jetzt ein Halbband-FIR mit 47 Koeffizienten (Sperrbereich
  ab 1,25 MHz mit mehr als 70 dB) statt der Mittelung; der Nachbarkanal im
  Abstand von 1,712 MHz faltet sich nicht mehr in den Nutzkanal. Schwache
  Ensembles neben starken werden dadurch sichtbar.
- **Sync-Erkennung** – die AGC bewertet den SNR erst nach dekodierten FIBs;
  der Sync-Fortschritt zählt ab dem zweiten Rahmen in Folge, ein flatternder
  Sync auf Rauschen hält die Gain-Regelung nicht mehr an.

## v3.0.1 – September 2026

- **Antennenspeisung (Bias-T)** – neuer Schalter in den Einstellungen unter
  Gerät/Empfang: 3,3 V / max. 50 mA am Antennenanschluss des HackRF One für
  aktive Antennen. Die Speisung wird gespeichert, beim Start angewendet und
  nach jedem Neustart des Empfangs erneut gesetzt.
- **AGC bei hohem Eingangspegel** – niedriger SNR auf hoher Gain-Stufe gilt
  als Hinweis auf zu viel Pegel (aktive Antenne, starker Sender). Die AGC
  probiert dann zuerst nach unten und lässt nach einem Sync-Verlust die
  Akquisitions-Ramp abwärts laufen. Schwache Signale auf niedriger Stufe
  werden wie bisher nach oben geregelt.

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
| `DAB-Classic-v3.0.2-portable-win64.zip` | Vollpaket mit WebView2-Laufzeit (Fixed Version) im Ordner `webview2\`, läuft auf jedem Windows 10/11 x64 |
| `DAB-Classic-v3.0.2-portable-win64-lite.zip` | Ohne WebView2-Laufzeit, nutzt die vom System (Microsoft Edge) bereitgestellte Evergreen-Laufzeit |

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
