# Spike 3 – DL+-Verfügbarkeit im Bundesmux (5C, Warntag-Mitschnitt)

Werkzeug: `tools/dlplus-stats.py`, Kern `dabcored --fast --all-audio` (alle Audiodienste als
Background-Slots in einem Durchlauf).  
Datei: `P:\Projekte\DAB\Warntag-2026\Warntag-5C-20260910-105430-part1.uff`, Ausschnitt 300.0 s Dateizeit ab Dateianfang (10:54:30 Uhr), Laufzeit des Kerns 13.8 s mit 14 Backends gleichzeitig (Faktor 21.7x Echtzeit).  
Stand: 2026-09-11 21:57

## Tabelle

| Dienst | SId | kbit/s | Codec | Superframe-Fehler | dls (distinct) | dl_plus | Content-Types | IT-Wechsel | IR-Anteil | Beispiel ITEM.TITLE / ITEM.ARTIST |
|---|---|---|---|---|---|---|---|---|---|---|
| DRadio DokDeb | D240 | 48 | HE-AAC 48 k SBR | 1147 | 54 (7) | 0 | - | 0 | - | (DLS) „Bundestagsdebatte “ |
| SCHLAGERPARADIES | 10C3 | 64 | HE-AAC 48 k SBR | 4521 | 2 (2) | 1 | 1 ITEM.TITLE, 4 ITEM.ARTIST | 0 | 100 % | „WAHNSINN ODER LIEBE“ / „SEMINO ROSSI“ |
| ENERGY | 1A45 | 72 | HE-AAC 48 k SBR | 1004 | 14 (9) | 8 | 1 ITEM.TITLE, 4 ITEM.ARTIST | 0 | 100 % | „Broken Strings“ / „James Morrison feat. Nelly Furtado“ |
| ASA DE | 10C4 | 32 | HE-AAC 48 k SBR PS | 0 | 0 (0) | 0 | - | 0 | - |  |
| Schwarzwaldradio | 100D | 64 | HE-AAC 48 k SBR | 11663 | 4 (3) | 4 | 1 ITEM.TITLE, 4 ITEM.ARTIST | 1 | 100 % | „Sayonara“ / „Lee Marrow“ |
| Dlf Kultur | D220 | 104 | HE-AAC 48 k SBR | 2 | 12 (4) | 138 | 1 ITEM.TITLE, 4 ITEM.ARTIST, 33 PROGRAMME.NOW | 2 | 100 % | „Crooked teeth“ / „death cab for cutie“ |
| Dlf Nova | D230 | 104 | HE-AAC 48 k SBR | 4 | 9 (4) | 76 | 1 ITEM.TITLE, 4 ITEM.ARTIST, 36 PROGRAMME.HOST | 2 | 100 % | „Moment in the sun“ / „Sunflower Bean“ |
| ERF Plus | 1A64 | 64 | HE-AAC 48 k SBR | 1042 | 2 (2) | 0 | - | 0 | - | (DLS) „Timo Langner - Freudenöl statt Tränen“ |
| Dlf | D210 | 104 | HE-AAC 48 k SBR | 0 | 29 (6) | 31 | 1 ITEM.TITLE, 4 ITEM.ARTIST | 0 | 100 % | „Wege durch die neue Grundsicherung: Rechte, Pflichten und Unterstützung“ / „Sebastian Moritz“ |
| radio horeb | D01C | 48 | HE-AAC 48 k SBR | 1119 | 6 (3) | 0 | - | 0 | - | (DLS) „Suizidprävention - eine Aufgabe für alle! Prof. Dr. Barbara Schneider“ |
| SUNSHINE LIVE | 15DC | 72 | HE-AAC 48 k SBR | 2005 | 21 (4) | 30 | 1 ITEM.TITLE, 4 ITEM.ARTIST | 1 | 100 % | „Read My Mind (Every Time I Talk)“ / „STRAENGE“ |
| KLASSIK RADIO | D75B | 72 | HE-AAC 48 k SBR | 867 | 3 (3) | 40 | 1 ITEM.TITLE, 4 ITEM.ARTIST | 2 | 100 % | „Can you feel the love tonight (Der König der Löwen / Film)“ / „Elton John“ |
| RADIO BOB! | 15DD | 72 | HE-AAC 48 k SBR | 1372 | 8 (3) | 72 | 1 ITEM.TITLE, 4 ITEM.ARTIST | 0 | 100 % | „High On You“ / „Survivor“ |
| Absolut relax | 17FA | 72 | HE-AAC 48 k SBR | 1061 | 10 (4) | 90 | 1 ITEM.TITLE, 4 ITEM.ARTIST | 3 | 100 % | „FIRST TIME“ / „ROBIN BECK“ |

Spalten: *Superframe-Fehler* = Summe der service_stats.frame_errors (Superframes ohne Firecode-/RS-Erfolg, jeder kostet 120 ms Audio und PAD, daher abgeschnittene DLS-Fragmente); *dls* = Anzahl gemeldeter (geänderter) Labels, in Klammern verschiedene Texte; *dl_plus* = Anzahl DL+-Kommandos; *IT-Wechsel* = Wechsel des Item-Toggle-Bits (= Titelwechsel); *IR-Anteil* = Anteil der Kommandos mit Item-Running = 1.

## Kurzfazit

**Ja, die Musik-Trennung (Entscheidung 7) kann auf DL+ bauen – für die Musiksender des
Bundesmux.** Befund im 300-s-Ausschnitt (alle 14 Audiodienste in einem Durchlauf):

| Gruppe | Dienste | DL+ |
|---|---|---|
| DL+ mit ITEM.TITLE (1) + ITEM.ARTIST (4), IR = 1, IT wechselt beim Titelwechsel | Absolut relax, RADIO BOB!, KLASSIK RADIO, SUNSHINE LIVE, ENERGY, Schwarzwaldradio, SCHLAGERPARADIES, Dlf Kultur (+33 PROGRAMME.NOW), Dlf Nova (+36 PROGRAMME.HOST), Dlf | 10 von 14 |
| nur DLS-Text, kein DL+ | DRadio DokDeb, ERF Plus, radio horeb | 3 von 14 |
| weder DLS noch DL+ | ASA DE (Warndienst, 32 kbit/s, PS) | 1 |

- Bei allen DL+-Sendern kommen genau die beiden Tags 1 TITLE und 4 ARTIST (Dlf-Sender zusätzlich
  33 bzw. 36). ITEM.ALBUM (2), ITEM.GENRE (11), STATIONNAME (31/32) sendet im Ausschnitt niemand.
- IR (Item Running) ist bei allen DL+-Kommandos 1 (100 %); IT (Item Toggle) kippt beim Titelwechsel
  (Absolut relax 3, Dlf Kultur/Nova/KLASSIK RADIO 2, SUNSHINE LIVE/Schwarzwaldradio 1 Wechsel in 5 min).
  Ein IR-0-Zustand (Moderation ohne Titel) trat im Ausschnitt nicht auf; die Trennlogik muss deshalb
  primär auf IT-Wechsel und Tag-Textwechsel reagieren, IR = 0 nur als Zusatzsignal.
- Die Dlf-Sender setzen DL+ auch für Wortbeiträge ein (TITLE = Beitragstitel, ARTIST = Autor).
- ERF Plus und radio horeb senden „Interpret - Titel“ nur als DLS-Text; dafür braucht die Trennung
  einen Fallback (Textwechsel des DLS-Labels + Heuristik „A - B“). DRadio DokDeb liefert nur Programmtext.
- Wiederholrate: ein DL+-Kommando je Labelwiederholung (Dlf Kultur 138 in 5 min ≈ alle 2 s, Musiksender
  30–90 je 5 min).

**Hinweise zur Datenqualität:** Die Spalte *Superframe-Fehler* zeigt, dass im Mitschnitt (SNR ≈ 7 dB)
die EEP-5-Dienste (Schwarzwaldradio, SCHLAGERPARADIES) und einige EEP-2-Dienste Superframes verlieren
(Dlf-Dienste und ASA DE mit EEP 1 sind fehlerfrei). Verlorene Superframes kosten 120 ms Audio und PAD;
dadurch entstehen abgeschnittene DLS-Fragmente („oison Ivy“, „ertelefon 00800 …“) und DL+-Tags mit
verschobenem Text (der v1-padHandler prüft die DLS-Segment-CRC nicht und setzt nach Verlust fort). Für die
Musik-Trennung: Tags nur übernehmen, wenn Start+Länge innerhalb eines *vollständigen* Labels liegen bzw.
derselbe Tag-Text zweimal hintereinander kommt (Dedup/Bestätigung) – das ist Sache der App-Logik (dab-music).

## Beispiele je Dienst

### DRadio DokDeb (D240)

- DLS: Bundestagsdebatte 
- DLS: e 
- DLS: Bundestag Live
- DLS: Deutschlandradio Dokumente und Debatten
- DLS:  Dokumente und Debatten
- DLS:  Dokumente und DebioðEo

### SCHLAGERPARADIES (10C3)

- DLS: SEMINO ROSSI - WAHNSINN ODER LIEBE
- DLS: DEIN MUSIKWUNSCH WHATSAPP: 06805-699110
- DL+ 1 ITEM.TITLE: WAHNSINN ODER LIEBE
- DL+ 4 ITEM.ARTIST: SEMINO ROSSI

### ENERGY (1A45)

- DLS: Broken Strings von James Morrison feat. Nelly Furtado
- DLS: Ihr hört ENERGY um 10:54 Uhr
- DLS: ENERGY - Deutschlands Hitradio Nr. 1
- DLS: HIT MUSIC ONLY ! Am Donnerstag, dem 10.09.2026
- DLS: ENERGY im Internet: energy.de
- DLS: ENERGY - HIT MUSIC ONLY !
- DL+ 1 ITEM.TITLE: Broken Strings
- DL+ 4 ITEM.ARTIST: James Morrison feat. Nelly Furtado
- weitere Titel: SWIM

### Schwarzwaldradio (100D)

- DLS: Lee Marrow - Sayonara
- DLS: The Coasters - Poison Ivy
- DLS: oison Ivy
- DL+ 1 ITEM.TITLE: Sayonara
- DL+ 4 ITEM.ARTIST: Lee Marrow
- weitere Titel: Poison Ivy

### Dlf Kultur (D220)

- DLS: Lesart 
- DLS: Crooked teeth von death cab for cutie
- DLS: Sugar Clouds von Ásgeir
- DLS: Deutschlandfunk Kultur
- DL+ 1 ITEM.TITLE: Crooked teeth
- DL+ 4 ITEM.ARTIST: death cab for cutie
- DL+ 33 PROGRAMME.NOW:  
- weitere Titel: Sugar Clouds;  

### Dlf Nova (D230)

- DLS: Deutschlandfunk Nova - Entdecke neue Songs. Dazu Musik, die schon immer bei dir war!
- DLS: "Moment in the sun" von Sunflower Bean
- DLS: Deutschlandfunk Nova mit Christoph Sterz
- DLS: "Camera" von Charli xcx
- DL+ 1 ITEM.TITLE: Moment in the sun
- DL+ 4 ITEM.ARTIST: Sunflower Bean
- DL+ 36 PROGRAMME.HOST: Christoph Sterz
- weitere Titel:  ; Camera

### ERF Plus (1A64)

- DLS: Timo Langner - Freudenöl statt Tränen
- DLS: Johannes Begemann - Da berühren sich Himmel und Erde

### Dlf (D210)

- DLS: Marktplatz - Hörertelefon 00800 44644464
- DLS: Deutschlandfunk - Alles von Relevanz
- DLS: Wege durch die neue Grundsicherung: Rechte, Pflichten und Unterstützung, Sebastian Moritz
- DLS: eue Grundsicherung: Rechte, Pflichten und Unterstützung, Sebastian Moritz
- DLS: - Alles von Relevanz
- DLS: ertelefon 00800 44644464
- DL+ 1 ITEM.TITLE: Wege durch die neue Grundsicherung: Rechte, Pflichten und Unterstützung
- DL+ 4 ITEM.ARTIST: Sebastian Moritz

### radio horeb (D01C)

- DLS: Suizidprävention - eine Aufgabe für alle! Prof. Dr. Barbara Schneider
- DLS: Suizidprävention - efür alle! Prof. Dr. Barbara Schneider
- DLS: Suizidprävention - eine Aufgabe für Dr. Barbara Schneider

### SUNSHINE LIVE (15DC)

- DLS: STRAENGE mit Read My Mind (Every Time I Talk) auf SUNSHINE LIVE
- DLS: Calvin Harris mit My Way auf SUNSHINE LIVE
- DLS: t My Way auf SUNSHINE LIVE
- DLS: Calvin Harris mit My Way auf SUNSHINE LIVEf SUNSHINE LIVE
- DL+ 1 ITEM.TITLE: Read My Mind (Every Time I Talk)
- DL+ 4 ITEM.ARTIST: STRAENGE
- weitere Titel: My Way; INE LI

### KLASSIK RADIO (D75B)

- DLS: Elton John - Can you feel the love tonight (Der König der Löwen / Film)
- DLS: Charles Gounod - 5.Satz Les Troyennes (Faust / Ballettmusik)
- DLS: Klassik Radio - Klassische Musik zum Wohlfühlen und Entspannen
- DL+ 1 ITEM.TITLE: Can you feel the love tonight (Der König der Löwen / Film)
- DL+ 4 ITEM.ARTIST: Elton John
- weitere Titel: you feel the love tonight (Der König der Lö; 5.Satz Les Troyennes (Faust / Ballettmusik);  

### RADIO BOB! (15DD)

- DLS: Survivor - High On You
- DLS: Linkin Park - Somewhere I Belong
- DLS: mewhere I Belong
- DL+ 1 ITEM.TITLE: High On You
- DL+ 4 ITEM.ARTIST: Survivor
- weitere Titel: Somewhere I Belong

### Absolut relax (17FA)

- DLS: ROBIN BECK - FIRST TIME
- DLS: TAYLOR SWIFT - ANTI-HERO
- DLS: Absolut relax - Entspannt durch den Tag
- DLS: NTI-HERO
- DL+ 1 ITEM.TITLE: FIRST TIME
- DL+ 4 ITEM.ARTIST: ROBIN BECK
- weitere Titel: ANTI-HERO;  Entspann

