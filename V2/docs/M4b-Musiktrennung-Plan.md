# M4b Musik-Trennung – Umsetzungsplan (Stand 12.09.2026)

Grundlage: Entscheidungen 6, 7 in `Entscheidungen.md`; Protokoll in `protocol.md`; Spike 3
(`Spike3-DLplus-2026-09-11.md`: 10/14 Dienste im Bundesmux liefern DL+ ITEM.TITLE(1)/ARTIST(4),
IR kippt zuverlässig bei Titelwechsel; DokDeb/ERF Plus/radio horeb nur DLS "A - B"); `M4-Timeshift-Plan.md`
Abschnitt 5 (Kurzfassung). Timeshift ist fertig (Commits 80e7895, f643ca4): `TimeshiftState`
(`frame_index`, `live_unix`, 2 Hz) und `export_timeshift_range` (`from_s`/`to_s` = Sekunden hinter live,
aktuell nur WAV) existieren bereits – **keine Änderung an diesen beiden Namen/Feldern**, nur additiv.

Kein Protokoll-Feld für „Rahmenindex an DL+/DLS geheftet" nötig: `TimeshiftState.frame_index` kommt mit
2 Hz; die App merkt sich den letzten Wert und heftet ihn an jede eingehende `dl_plus`/`dls`-Änderung
(Auflösung 0,5 s, für Vor-/Nachlauf von Sekunden ausreichend).

## 1. Kern (libdabcore, C++) – MP3/ID3 in `export_timeshift_range` und `start_recording`

### 1.1 Protokoll (additiv, `dab-api`)
`RecFormat::Mp3 { kbps: u16 }` bekommt ein neues optionales Feld `id3: Option<Id3Tags>`
(`#[serde(default)]`, alte Aufrufe ohne `id3` bleiben gültig):
```rust
pub struct Id3Tags {
    pub title: Option<String>,
    pub artist: Option<String>,
    pub album: Option<String>,   // z. B. Sendername ("Dlf")
    pub date: Option<String>,    // ISO yyyy-mm-dd, aus live_unix beim Start des Kandidaten
    pub cover_png_b64: Option<String>, // Slide oder Logo, von der App mitgeschickt
}
```
Kern bekommt keine eigene Cover-/Logo-Kenntnis – die App liefert `cover_png_b64` bereits fertig (Slide
aus `mot_slide`/Logo aus dem Cache), Kern schreibt nur das APIC-Frame.

### 1.2 MP3-Encoder
`pkg_check_modules(MP3LAME mp3lame)` in `CMakeLists.txt` (MSYS2-Paket `mingw-w64-ucrt-x86_64-lame`
prüfen, ggf. `libmp3lame`-pkgconfig-Name abweichend – erst `pkg-config --list-all | grep -i mp3` /
`| grep -i lame` laufen lassen). `Mp3Writer` (analog `WavWriter`, `audio/mp3-writer.{h,cpp}`):
`lame_init`, 48 kHz/Stereo/16 Bit → `kbps` (Default aus Entscheidung 6: 192), `lame_encode_buffer_interleaved`,
`lame_encode_flush`. Bei Encoder-Fehler: `log error` + Abbruch wie WAV-Pfad.

### 1.3 ID3v2.4 selbst schreiben (keine neue Abhängigkeit)
Minimaler Writer (`audio/id3-writer.{h,cpp}`): Header `ID3\x04\x00`, Frames `TIT2`/`TPE1`/`TALB`/`TDRC`
(UTF-8, Encoding-Byte 0x03) und `APIC` (MIME `image/png`, Picture-Type 0x03 "Cover (front)") nur wenn
das jeweilige Feld gesetzt ist; Tag-Größe als syncsafe Integer voranstellen, vor die MP3-Frames der Datei.

### 1.4 `exportTimeshiftRange` / `startRecording` erweitern
Aktuell (`core.cpp:1187`) wird bei `fmt != "wav"` nur gewarnt und trotzdem WAV geschrieben – das entfällt.
`format.value("format", "wav")` auswerten: `"mp3"` → `Mp3Writer` + optional `Id3Writer` (Tags aus
`format["id3"]`, falls vorhanden) statt `WavWriter`; `"aac_passthrough"` bleibt Archivoption (960-Sample-
Frames aus `mp4Processor` ohne Neucodierung in `.m4a`, wie in Entscheidung 6 vorgesehen – falls in der
verbleibenden Zeit zu aufwendig, mit `log warn "folgt später"` zurückstellen und Stefan informieren, WAV/
MP3 haben Vorrang). Gilt für **beide** Aufrufer (`exportTimeshiftRange` **und** `startRecording`, damit
manuelle Aufnahme ebenfalls MP3 kann – Entscheidung 6 Standard ist MP3 für Musikaufnahme allgemein).
`recording_state` am Ende meldet weiterhin nur `path/bytes/seconds` (Format ergibt sich aus der Endung).

### 1.5 Tests (ctest)
Neuer Test `dabcored_music_export`: Export eines Warntag-Ausschnitts (oder eines kurzen Musik-Segments
aus einem Live-Mitschnitt, falls vorhanden – sonst Referenzdatei mit `dab-cli replay --fast`) mit
`format: {format: mp3, kbps: 192, id3: {title: "Test", artist: "X"}}`; Prüfung: Datei beginnt mit `ID3`,
enthält `mp3lame`-Frame-Sync (0xFF 0xFB/0xFA) nach dem Tag, Dauer plausibel (± 1 s wie beim WAV-Test).

## 2. App (`dab-music` + `dab-app`, Rust)

### 2.1 `dab-music` erweitern (Datenmodell existiert: `TrackCandidate`, `SplitConfig`)
`TrackCandidate` bekommt `start_frame: u64`, `end_frame: Option<u64>` (statt/zusätzlich zu `start_s`/
`end_s`, die weiterhin nur zur Anzeige in der Vorschlagsliste dienen – **maßgeblich für den Export sind
die Frame-Indizes**, weil `from_s`/`to_s` sich auf „Sekunden hinter **jetzt**" beziehen und sich mit jeder
Sekunde ändern, während der Kandidat im Puffer wartet). Neue Funktion `to_export_range(current_frame:
u64) -> (f64, f64)`: `from_s = (current_frame - start_frame) as f64 * 0.024 + pre_roll_s`,
`to_s = (current_frame - end_frame.unwrap_or(current_frame)) as f64 * 0.024 - post_roll_s` (auf `>= 0`
klammen, `from_s > to_s` sonst verwerfen/loggen).

### 2.2 `dab-app/src/music.rs` (neu)
Zustand `MusicDetector`: hört auf `Event::DlPlus`/`Event::Dls` (Primary) und `Event::TimeshiftState`
(merkt sich `frame_index`); Erkennung:
- **DL+-Dienste** (Entscheidung/Spike 3: item_toggle-Flanke ODER Wechsel von `tags[TITLE]`): bei
  `item_running == true` und neuem/geändertem `ITEM.TITLE` → neuer offener Kandidat (`start_frame` =
  aktueller `frame_index`, `title`/`artist` aus den Tags 1/4); bei `item_running == false` oder
  nächstem Toggle → Kandidat schließen (`end_frame` = aktueller `frame_index`), verwerfen wenn
  `duration_s < min_len_s` oder `item_running` beim Öffnen bereits `false` war (Moderation/Nachrichten,
  siehe Doc-Kommentar in `dab-music`).
- **Nur-DLS-Dienste** (DokDeb, ERF Plus, radio horeb laut Spike 3 – Liste nicht hart codieren, sondern
  einfach: Dienst hat nach 30 s noch nie `dl_plus` gesendet → Fallback aktiv): Regex/Split an " - " in
  `Event::Dls.text`; Text-Wechsel = neuer Kandidat, vorherigen schließen; kein `item_running`-Signal
  vorhanden → `min_len_s`/`max_len_s` als einzige Plausibilitätsprüfung, keine harte Titel/Artist-Trennung
  garantiert (linker Teil = Artist, rechter = Title, wie v1-Konvention, falls dort vorhanden – sonst
  beides in `title`).
- Sender wechselt (`service_started` auf Primary) oder Timeshift-Puffer leert (`buffered_s` sinkt auf 0,
  z. B. nach EWS-Umschaltung) → offenen Kandidaten verwerfen (Ring enthält ihn nicht mehr zuverlässig).
- Ergebnisliste (max. 20 Einträge, älteste zuerst verwerfen) in `AppState` (neues Feld `music_candidates:
  Vec<TrackCandidate>`), Persistenz nicht nötig (Puffer ist ohnehin flüchtig).

### 2.3 Export-Aktionen
Tauri-Kommando `music_export(candidate_index: usize, auto_save: bool) -> Result<(), String>`: baut
`export_timeshift_range` mit `to_export_range(current_frame)`, `format: mp3` (Standard, Entscheidung 6),
`id3` aus Kandidat + `station` (aktueller Dienstname) + Cover (Slide falls vorhanden, sonst Logo aus dem
Cache, `logos.rs` liefert das schon als Data-URL – PNG-Bytes daraus zurückgewinnen); Dateiname wie v1-
Konvention (`recording.rs` hat das Muster schon für Aufnahmen, wiederverwenden). Schalter „automatisch
speichern" (`Settings.music_auto_save`, Default aus): wenn an, `music_export` sofort beim Schließen eines
Kandidaten aufrufen statt in die Vorschlagsliste zu legen (Entscheidung 7).

### 2.4 Vertrag zwischen Kern-Agent und App-Agent
`RecFormat::Mp3{kbps,id3}` (Abschnitt 1.1) ist der einzige geteilte Berührungspunkt – **Feldnamen exakt
wie in 1.1**, damit beide Seiten unabhängig arbeiten können (Kern-Agent kennt `dab-app`/`dab-music`
nicht, App-Agent muss nicht auf core-cpp warten, `RecFormat` in `dab-api` ändert der App-Agent selbst
additiv, Kern-Agent implementiert nur das Verhalten dazu).

## 3. Shell (Svelte) – Panel „Musik"

`components/MusicPanel.svelte` (Hotkey frei wählen, z. B. `Umschalt+M`, da `M` = Mute schon belegt):
Liste `music_candidates` (Titel, Artist, Dauer, Sender, Start-Zeit relativ „vor 3:20"), pro Zeile
„übernehmen" (ruft `music_export`) und optional Wellenform – **kein neuer Kern-Kanal für PCM-Vorschau
nötig**, Balken aus dem vorhandenen `audio_level`-Verlauf (falls dieser schon gepuffert wird; sonst
einfache Platzhalter-Leiste ohne Wellenform in dieser ersten Ausbaustufe, echte Wellenform zurückstellen
statt einen weiteren Ring im Kern zu bauen). Button „alle übernehmen" (alle offenen Kandidaten exportieren,
sequenziell wegen „höchstens ein Export gleichzeitig"). Schalter „automatisch speichern" oben im Panel,
gespiegelt in `SettingsPanel`. Verschiebbare Schnittmarken (Pre-/Post-Roll pro Kandidat vor dem Export
anpassen) als einfache +/- Sekunden-Stepper statt Drag-Wellenform, wenn die Zeit knapp wird – Stefan bei
Abnahme fragen, ob das für den ersten Wurf reicht.

## 4. Abnahme (live, HackRF, und Referenzdatei)

- ctest `dabcored_music_export` grün, bestehende 10 ctests weiter grün.
- Live: Musiksender (z. B. WDR 2/Antenne DE) länger laufen lassen, Panel zeigt Kandidaten mit Titel/
  Artist, „übernehmen" erzeugt MP3 mit lesbaren Tags (in einem Player/Explorer prüfen, nicht nur Bytes).
- Nur-DLS-Dienst (radio horeb o. ä., falls empfangbar) prüfen: Kandidaten ohne DL+ entstehen trotzdem.
- „Automatisch speichern" an: Kandidat verschwindet aus der Liste, Datei liegt im Aufnahmeordner.
- Senderwechsel während offenem Kandidat: kein Absturz, Kandidat verworfen (Log-Zeile reicht als Beleg).

## 5. Offen / bewusst zurückgestellt

AAC-Passthrough-Export (960-Sample-Frames) nur falls Zeit reicht, sonst separater Nachtrag; echte
Wellenform-Darstellung; Cover-Auswahl durch den Nutzer (nur automatisch Slide/Logo); Umschaltsperre
während eines Musik-Exports (Export läuft parallel zur Wiedergabe, betrifft nur den Recorder-Thread –
prüfen ob bestehende Aufnahme-Sperre das schon abdeckt oder ob „Export" als eigener Zustand ergänzt
werden muss, das ist der einzige Punkt, den der App-Agent mit dem bestehenden `recording.rs`
(Umschaltsperre) abgleichen muss, nicht mit dem Kern-Agenten).
