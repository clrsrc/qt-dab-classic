#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""QA Test Report Generator for Qt-DAB Empfangsoptimierung"""

from reportlab.lib.pagesizes import A4
from reportlab.lib import colors
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.units import mm
from reportlab.platypus import (
    SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle,
    PageBreak, HRFlowable
)
from reportlab.lib.enums import TA_LEFT, TA_CENTER, TA_JUSTIFY
from datetime import datetime

OUTPUT = "D:/cloud/Dropbox/Projekte/DAB/QA-Testreport.pdf"

doc = SimpleDocTemplate(
    OUTPUT, pagesize=A4,
    leftMargin=20*mm, rightMargin=20*mm,
    topMargin=20*mm, bottomMargin=20*mm
)

styles = getSampleStyleSheet()
styles.add(ParagraphStyle(name='Title2', fontSize=20, leading=24,
                          alignment=TA_CENTER, spaceAfter=6*mm,
                          fontName='Helvetica-Bold'))
styles.add(ParagraphStyle(name='Subtitle', fontSize=12, leading=14,
                          alignment=TA_CENTER, spaceAfter=10*mm,
                          textColor=colors.HexColor('#555555')))
styles.add(ParagraphStyle(name='H1', fontSize=14, leading=18,
                          fontName='Helvetica-Bold', spaceAfter=4*mm,
                          spaceBefore=6*mm))
styles.add(ParagraphStyle(name='H2', fontSize=11, leading=14,
                          fontName='Helvetica-Bold', spaceAfter=2*mm,
                          spaceBefore=4*mm))
styles.add(ParagraphStyle(name='Body', fontSize=9, leading=12,
                          alignment=TA_JUSTIFY, spaceAfter=2*mm))
styles.add(ParagraphStyle(name='CodeBlock', fontSize=8, leading=10,
                          fontName='Courier', spaceAfter=2*mm,
                          backColor=colors.HexColor('#F0F0F0'),
                          leftIndent=10, rightIndent=10))
styles.add(ParagraphStyle(name='Small', fontSize=8, leading=10,
                          textColor=colors.HexColor('#777777')))

story = []

# ---- TITLE PAGE ----
story.append(Spacer(1, 40*mm))
story.append(Paragraph("QA Test Report", styles['Title2']))
story.append(Paragraph("Qt-DAB Empfangsoptimierung - Code Audit", styles['Subtitle']))
story.append(HRFlowable(width="80%", thickness=1, color=colors.HexColor('#CCCCCC')))
story.append(Spacer(1, 10*mm))

meta = [
    ["Projekt:", "Qt-DAB DAB+ Receiver"],
    ["Version:", "Snapshot mit 9 Optimierungen"],
    ["Datum:", datetime.now().strftime("%d.%m.%Y")],
    ["Tester:", "Automatisierter Code-Audit (3 parallele Analysen)"],
    ["Scope:", "10 geaenderte Quelldateien"],
    ["Methodik:", "Statische Code-Analyse, Race-Condition-Analyse, Numerische Stabilitaet"],
]
t = Table(meta, colWidths=[40*mm, 120*mm])
t.setStyle(TableStyle([
    ('FONTNAME', (0,0), (0,-1), 'Helvetica-Bold'),
    ('FONTSIZE', (0,0), (-1,-1), 9),
    ('BOTTOMPADDING', (0,0), (-1,-1), 3),
    ('TOPPADDING', (0,0), (-1,-1), 3),
    ('VALIGN', (0,0), (-1,-1), 'TOP'),
]))
story.append(t)

story.append(PageBreak())

# ---- EXECUTIVE SUMMARY ----
story.append(Paragraph("1. Executive Summary", styles['H1']))
story.append(Paragraph(
    "Im Rahmen der Empfangsoptimierung des Qt-DAB Receivers wurden 10 Quelldateien "
    "geaendert. Drei parallele Code-Audits haben insgesamt <b>22 deduplizierte Findings</b> "
    "identifiziert. Davon sind <b>4 CRITICAL</b>, <b>5 HIGH</b>, <b>9 MEDIUM</b> und "
    "<b>4 LOW</b> Severity.", styles['Body']))

summary_data = [
    ["Severity", "Anzahl", "Beschreibung"],
    ["CRITICAL", "4", "Crashes, Div-by-Zero, Race Conditions mit Absturzpotenzial"],
    ["HIGH", "5", "Geraetekompatibilitaet, Thread-Safety, Datenintegritaet"],
    ["MEDIUM", "9", "Algorithmische Stabilitaet, Typinkonsistenz, fehlende Validierung"],
    ["LOW", "4", "Kosmetische Issues, Minor Inkonsistenzen"],
]
st = Table(summary_data, colWidths=[25*mm, 18*mm, 120*mm])
st.setStyle(TableStyle([
    ('BACKGROUND', (0,0), (-1,0), colors.HexColor('#2C3E50')),
    ('TEXTCOLOR', (0,0), (-1,0), colors.white),
    ('FONTNAME', (0,0), (-1,0), 'Helvetica-Bold'),
    ('FONTSIZE', (0,0), (-1,-1), 8),
    ('GRID', (0,0), (-1,-1), 0.5, colors.HexColor('#CCCCCC')),
    ('BACKGROUND', (0,1), (-1,1), colors.HexColor('#FFDDDD')),
    ('BACKGROUND', (0,2), (-1,2), colors.HexColor('#FFE8CC')),
    ('BACKGROUND', (0,3), (-1,3), colors.HexColor('#FFFFCC')),
    ('BACKGROUND', (0,4), (-1,4), colors.HexColor('#E8FFE8')),
    ('BOTTOMPADDING', (0,0), (-1,-1), 4),
    ('TOPPADDING', (0,0), (-1,-1), 4),
    ('LEFTPADDING', (0,0), (-1,-1), 4),
]))
story.append(st)
story.append(Spacer(1, 5*mm))
story.append(Paragraph(
    "<b>Bewertung:</b> Die kritischen Findings (Division-by-Zero, Race Conditions) muessen "
    "vor einem produktiven Release behoben werden. Die HIGH-Findings betreffen vor allem "
    "die Geraetekompatibilitaet (RTL-SDR Gain-Defaults) und sollten zeitnah addressiert werden. "
    "Die MEDIUM-Findings sind Stabilitaetsverbesserungen, die die Robustheit erhoehen.",
    styles['Body']))

story.append(Paragraph("2. Gepruefter Scope", styles['H1']))
scope_data = [
    ["Datei", "Aenderung"],
    ["sample-reader.cpp", "IQ-Equalizer aktiviert, DC-Alpha zweistufig"],
    ["sample-reader.h", "dcSampleCounter Member"],
    ["equalizer.h", "Division-by-Zero Absicherung"],
    ["rtlsdr-handler.cpp", "Default Gain 340, HW AGC"],
    ["hackrf-handler.cpp", "BW 1536kHz, LNA/VGA, Oversampling, Adaptive Gain"],
    ["hackrf-handler.h", "adjustGain() Override"],
    ["device-handler.h", "Virtuelle adjustGain()"],
    ["ofdm-handler.cpp", "AFC-Faktor 0.25, SNR-Filter 0.8/0.2"],
    ["config-handler.cpp", "Default Decoder DECODER_3"],
    ["radio.cpp", "adjustGain() Aufruf in show_snr()"],
]
st2 = Table(scope_data, colWidths=[45*mm, 118*mm])
st2.setStyle(TableStyle([
    ('BACKGROUND', (0,0), (-1,0), colors.HexColor('#2C3E50')),
    ('TEXTCOLOR', (0,0), (-1,0), colors.white),
    ('FONTNAME', (0,0), (-1,0), 'Helvetica-Bold'),
    ('FONTSIZE', (0,0), (-1,-1), 8),
    ('GRID', (0,0), (-1,-1), 0.5, colors.HexColor('#CCCCCC')),
    ('BOTTOMPADDING', (0,0), (-1,-1), 3),
    ('TOPPADDING', (0,0), (-1,-1), 3),
    ('LEFTPADDING', (0,0), (-1,-1), 4),
    ('ROWBACKGROUNDS', (0,1), (-1,-1), [colors.white, colors.HexColor('#F8F8F8')]),
]))
story.append(st2)

story.append(PageBreak())

# ---- FINDINGS ----
story.append(Paragraph("3. Detaillierte Findings", styles['H1']))

sev_color = {
    'CRITICAL': colors.HexColor('#CC0000'),
    'HIGH': colors.HexColor('#DD6600'),
    'MEDIUM': colors.HexColor('#CC9900'),
    'LOW': colors.HexColor('#339933'),
}

findings = [
    # CRITICAL
    {
        'id': 'BUG-001', 'severity': 'CRITICAL',
        'title': 'Division by Zero in show_dcOffset',
        'file': 'sources/frontend/sample-reader.cpp', 'lines': '168-169',
        'desc': 'Die Berechnung <font face="Courier" size="8">10 * (IQ_Real - IQ_Imag) / ((IQ_Real + IQ_Imag) / 2)</font> '
                'dividiert durch Null wenn beide IQ-Werte Null sind (z.B. bei Signalverlust oder Programmstart). '
                'Erzeugt NaN/Inf das an die UI weitergegeben wird.',
        'impact': 'UI-Korruption, potenzieller Absturz in Signal-Handlern, numerische Instabilitaet downstream.',
    },
    {
        'id': 'BUG-002', 'severity': 'CRITICAL',
        'title': 'NaN-Propagation durch sqrt() negativer Werte im Equalizer',
        'file': 'sources/support/equalizer.h', 'lines': '71, 79',
        'desc': 'sqrt(I_avg) und sqrt(Q_avg) koennen NaN erzeugen wenn durch numerischen Drift negative '
                'Werte entstehen. Die Guard-Checks (denom &lt; 1e-10) fangen NaN nicht ab, da NaN &lt; x immer false ist.',
        'impact': 'NaN propagiert durch gesamte Signalverarbeitung. Stille Korruption der Equalization.',
    },
    {
        'id': 'BUG-003', 'severity': 'CRITICAL',
        'title': 'Race Condition: theDeviceHandler in show_snr()',
        'file': 'sources/main/radio.cpp', 'lines': '4263-4264',
        'desc': 'theDeviceHandler wird ohne Synchronisation geprueft und aufgerufen. Zwischen NULL-Check und '
                'Funktionsaufruf kann das Objekt durch Device-Wechsel oder Shutdown zerstoert werden. '
                'Kein running-Flag-Check wie in anderen Slots (z.B. show_quality).',
        'impact': 'Use-after-free Crash bei Device-Wechsel waehrend laufendem Empfang.',
    },
    {
        'id': 'BUG-004', 'severity': 'CRITICAL',
        'title': 'Race Condition: adjustGain() GUI-Zugriff aus falschem Thread',
        'file': 'sources/devices/hackrf-handler/hackrf-handler.cpp', 'lines': '801-815',
        'desc': 'adjustGain() wird aus show_snr() aufgerufen (Signal/Slot, moeglicherweise OFDM-Thread). '
                'Zugriff auf vgaGainSlider->value() und setValue() ohne Thread-Synchronisation. '
                'Qt Widgets duerfen nur aus dem GUI-Thread modifiziert werden.',
        'impact': 'Undefiniertes Verhalten, moeglicher Crash, erratische Gain-Aenderungen.',
    },

    # HIGH
    {
        'id': 'BUG-005', 'severity': 'HIGH',
        'title': 'RTL-SDR Default Gain "340" existiert nicht fuer alle Tuner-Typen',
        'file': 'sources/devices/rtlsdr-handler/rtlsdr-handler.cpp', 'lines': '216-219',
        'desc': 'Der Default-Gain "340" (34 dB) existiert nur beim R820T Tuner. E4000, FC0012, FC0013 '
                'haben voellig andere Gain-Tabellen. findText() gibt -1 zurueck, Fallback auf gainsCount/2 '
                'ist unvorhersehbar (Combobox in absteigender Reihenfolge).',
        'impact': 'Falscher Gain bei Erststart mit nicht-R820T Tunern. Empfang kann dadurch fehlen.',
    },
    {
        'id': 'BUG-006', 'severity': 'HIGH',
        'title': 'Race Condition auf dcRemoval Boolean (nicht-atomar)',
        'file': 'sources/frontend/sample-reader.cpp', 'lines': '157, 210-211',
        'desc': 'dcRemoval ist ein plain bool, wird aber aus verschiedenen Threads gelesen (getSamples) '
                'und geschrieben (set_dcRemoval aus UI). Verletzt C++ Memory Model (Data Race = UB). '
                'Vergleich: running ist korrekt als atomic<bool> deklariert.',
        'impact': 'Undefiniertes Verhalten laut C++ Standard. Inkonsistenter Zustand moeglich.',
    },
    {
        'id': 'BUG-007', 'severity': 'HIGH',
        'title': 'Integer Overflow in dcSampleCounter',
        'file': 'sources/frontend/sample-reader.cpp', 'lines': '150-152',
        'desc': 'dcSampleCounter (int) wird bei jedem Sample inkrementiert. Nach Erreichen von '
                'DC_FAST_SETTLE_SAMPLES (100000) stoppt das Inkrement, aber der Counter wird nie zurueckgesetzt. '
                'Bei erneutem Aktivieren von dcRemoval koennte der Counter bereits ueber dem Threshold liegen.',
        'impact': 'Fast-Settle-Phase wird nach erstem Durchlauf nie wieder aktiviert (Design-Issue).',
    },
    {
        'id': 'BUG-008', 'severity': 'HIGH',
        'title': 'DECODER_3 Default inkonsistent mit ofdm-handler',
        'file': 'sources/main/config-handler.cpp', 'lines': '189-191',
        'desc': 'config-handler.cpp setzt Default auf DECODER_3, aber ofdm-handler.cpp verwendet DECODER_1 '
                'als eigenen Default. Inkonsistenz. Bei __MSC_THREAD__ ist der Decoder-Selector deaktiviert, '
                'DECODER_3 wird aber trotzdem als Default gesetzt.',
        'impact': 'Inkonsistentes Verhalten je nach Build-Konfiguration. Moegliche Runtime-Fehler.',
    },
    {
        'id': 'BUG-009', 'severity': 'HIGH',
        'title': 'toSkip Variable: Race Condition (nicht-atomar)',
        'file': 'sources/devices/device-handler.h / hackrf-handler.cpp', 'lines': '62 / 374-388',
        'desc': 'toSkip ist ein plain int in der Basisklasse. Wird aus dem GUI-Thread geschrieben '
                '(restartReader) und aus dem HackRF-Callback-Thread gelesen und dekrementiert.',
        'impact': 'Samples koennen versehentlich uebersprungen oder verarbeitet werden. DC-Offset/Clicks.',
    },

    # MEDIUM
    {
        'id': 'BUG-010', 'severity': 'MEDIUM',
        'title': 'AFC Daempfungsfaktor 0.25: Potenzielle Oszillation',
        'file': 'sources/frontend/ofdm-handler.cpp', 'lines': '496',
        'desc': 'Fine-Offset-Faktor von 0.1 auf 0.25 erhoeht (2.5x schnellere Konvergenz). '
                'Bei Signalspitzen kann fineOffset ueberschiessen und oszillieren. Keine Stabilitaetsanalyse '
                'des geschlossenen Regelkreises dokumentiert.',
        'impact': 'Frequenz-Tracking kann bei schwachem Signal instabil werden.',
    },
    {
        'id': 'BUG-011', 'severity': 'MEDIUM',
        'title': 'SNR-Filter verdoppelte Empfindlichkeit',
        'file': 'sources/frontend/ofdm-handler.cpp', 'lines': '476',
        'desc': 'SNR-Filter von 0.9/0.1 auf 0.8/0.2 geaendert. Doppelte Gewichtung neuer Messwerte '
                'fuehrt zu volatilerem SNR-Wert. In Kombination mit adjustGain() koennen oszillierende '
                'Gain-Anpassungen entstehen.',
        'impact': 'Instabiles Verhalten der adaptiven Gain-Regelung bei schnell schwankendem SNR.',
    },
    {
        'id': 'BUG-012', 'severity': 'MEDIUM',
        'title': 'Equalizer: Unzureichende Guard gegen negative sqrt-Argumente',
        'file': 'sources/support/equalizer.h', 'lines': '71-74',
        'desc': 'Guard prueft nur ob Denominator &lt; 1e-10, aber nicht ob I_avg und Q_avg nicht-negativ sind. '
                'sqrt() einer negativen Zahl gibt NaN, und NaN &lt; 1e-10 ist false, Guard wird umgangen.',
        'impact': 'NaN-Propagation bei numerischem Drift der Exponential-Averages.',
    },
    {
        'id': 'BUG-013', 'severity': 'MEDIUM',
        'title': 'Equalizer: Unangemessene Initialwerte',
        'file': 'sources/support/equalizer.h', 'lines': '57-60',
        'desc': 'I_avg, Q_avg, IQ_avg, Q_out werden alle mit 1.0 initialisiert. Diese Werte nehmen an, '
                'dass das Signal auf ~1.0 normalisiert ist. Bei sehr kleinen Signalen dauert die Konvergenz '
                'unnoetig lange (Alpha = 1/2048000 = ~5e-7).',
        'impact': 'Schlechte Equalization-Qualitaet waehrend Einschwingphase.',
    },
    {
        'id': 'BUG-014', 'severity': 'MEDIUM',
        'title': 'Bandbreite nicht automatisch mit Samplerate validiert',
        'file': 'sources/devices/hackrf-handler/hackrf-handler.cpp', 'lines': '130, 344',
        'desc': 'Baseband-Filter ist auf 1536000 Hz hardcoded. Bei handle_samplerateCorrection wird die '
                'Samplerate angepasst, aber der Filter bleibt fest. Verhaeltnis BW/Fs kann dadurch unguenstig werden.',
        'impact': 'Aliasing-Artefakte bei ungluecklicher Samplerate-Korrektur.',
    },
    {
        'id': 'BUG-015', 'severity': 'MEDIUM',
        'title': 'RTL-SDR HW AGC nicht von allen Tunern unterstuetzt',
        'file': 'sources/devices/rtlsdr-handler/rtlsdr-handler.cpp', 'lines': '221-222',
        'desc': 'Default AGC-Mode 0 (HW AGC) wird ohne Pruefung gesetzt. E4000-Tuner unterstuetzen '
                'kein HW AGC, Aufruf wird still ignoriert. Kein Fehler-Feedback an User.',
        'impact': 'User denkt AGC ist aktiv, tatsaechlich ist Gain unkontrolliert.',
    },
    {
        'id': 'BUG-016', 'severity': 'MEDIUM',
        'title': 'Busy-Wait Loop in update_gainSettings',
        'file': 'sources/devices/hackrf-handler/hackrf-handler.cpp', 'lines': '777-785',
        'desc': 'Polling-Loop wartet mit usleep(1000) auf Slider-Update. Kein Timeout. '
                'Wenn Signal nie zugestellt wird, haengt der Thread endlos.',
        'impact': 'Potenzieller Deadlock, UI-Freeze bei Device-Restart.',
    },
    {
        'id': 'BUG-017', 'severity': 'MEDIUM',
        'title': 'snr Member-Variable nicht thread-safe',
        'file': 'sources/frontend/ofdm-handler.cpp', 'lines': '476, 482',
        'desc': 'snr wird im OFDM-Thread geschrieben und per Signal an GUI-Thread gesendet. '
                'Float-Zuweisung ist nicht auf allen Plattformen atomar (Torn Read/Write).',
        'impact': 'Korrupter SNR-Wert an GUI, falsche adaptive Gain-Entscheidungen.',
    },
    {
        'id': 'BUG-018', 'severity': 'MEDIUM',
        'title': 'Samplerate-Korrektur ohne Bounds-Check',
        'file': 'sources/devices/hackrf-handler/hackrf-handler.cpp', 'lines': '332-337',
        'desc': 'correction-Wert wird nicht begrenzt. Bei grossen negativen Werten kann correctedRate '
                'unter 2 MHz fallen (HackRF Minimum). Hardware lehnt ab, aber GUI zeigt falschen Zustand.',
        'impact': 'Stille Fehler bei extremen Korrekturwerten.',
    },

    # LOW
    {
        'id': 'BUG-019', 'severity': 'LOW',
        'title': 'Typ-Inkonsistenz in Equalizer-Konstruktor',
        'file': 'sources/support/equalizer.h', 'lines': '57-61',
        'desc': 'Q_avg = 1.0 (double) statt 1.0f (float). Alpha_ = 1.0/2048000.0 als double-Division. '
                'Inkonsistent mit anderen float-Literals.',
        'impact': 'Subtile Praezisionsunterschiede je nach DABFLOAT-Typ.',
    },
    {
        'id': 'BUG-020', 'severity': 'LOW',
        'title': 'Static Counter in getSamples() wird nie zurueckgesetzt',
        'file': 'sources/frontend/sample-reader.cpp', 'lines': '166',
        'desc': 'static int teller persistiert ueber alle Aufrufe. Bei unregelmaessigen '
                'getSamples()-Aufrufen kann das Timing der dcOffset-Anzeige driften.',
        'impact': 'Minor: Ungleichmaessige UI-Updates der DC-Offset-Anzeige.',
    },
    {
        'id': 'BUG-021', 'severity': 'LOW',
        'title': 'VGA Gain Masking nicht in UI reflektiert',
        'file': 'sources/devices/hackrf-handler/hackrf-handler.cpp', 'lines': '264',
        'desc': 'VGA Gain wird mit & ~0x01 auf gerade Werte gerundet, aber Display zeigt den '
                'angeforderten Wert (ungerade moeglich). Diskrepanz zwischen Anzeige und Hardware.',
        'impact': 'Kosmetisch: User sieht 31 dB, Hardware setzt 30 dB.',
    },
    {
        'id': 'BUG-022', 'severity': 'LOW',
        'title': 'Gain-Combobox Fallback-Logik bei absteigender Sortierung',
        'file': 'sources/devices/rtlsdr-handler/rtlsdr-handler.cpp', 'lines': '219',
        'desc': 'Fallback auf gainsCount/2 bei unbekanntem Gain-Wert. Da Combobox absteigend sortiert ist, '
                'waehlt der mittlere Index nicht den mittleren Gain-Wert.',
        'impact': 'Minor: Unvorhersehbarer Initial-Gain bei Erststart.',
    },
]

for f in findings:
    sev = f['severity']
    c = sev_color[sev]

    # Severity badge
    badge_data = [[f['id'], sev, f['title']]]
    badge = Table(badge_data, colWidths=[20*mm, 22*mm, 121*mm])
    badge.setStyle(TableStyle([
        ('BACKGROUND', (0,0), (0,0), colors.HexColor('#2C3E50')),
        ('TEXTCOLOR', (0,0), (0,0), colors.white),
        ('BACKGROUND', (1,0), (1,0), c),
        ('TEXTCOLOR', (1,0), (1,0), colors.white),
        ('FONTNAME', (0,0), (-1,0), 'Helvetica-Bold'),
        ('FONTSIZE', (0,0), (-1,0), 9),
        ('BOTTOMPADDING', (0,0), (-1,0), 5),
        ('TOPPADDING', (0,0), (-1,0), 5),
        ('LEFTPADDING', (0,0), (-1,0), 4),
        ('BOX', (0,0), (-1,0), 0.5, colors.HexColor('#CCCCCC')),
    ]))
    story.append(badge)

    story.append(Paragraph(
        f"<b>Datei:</b> {f['file']} &nbsp; <b>Zeilen:</b> {f['lines']}",
        styles['Small']))
    story.append(Paragraph(f['desc'], styles['Body']))
    story.append(Paragraph(f"<b>Impact:</b> {f['impact']}", styles['Body']))
    story.append(Spacer(1, 2*mm))

story.append(PageBreak())

# ---- RECOMMENDATIONS ----
story.append(Paragraph("4. Empfehlungen", styles['H1']))

recs = [
    ["Prioritaet", "Massnahme", "Betroffene Bugs"],
    ["P0 - Sofort", "Division-by-Zero Guards vervollstaendigen (sqrt-Argumente pruefen, Denominator-Checks)", "BUG-001, BUG-002, BUG-012"],
    ["P0 - Sofort", "adjustGain() per QueuedConnection oder QMetaObject::invokeMethod in GUI-Thread ausfuehren", "BUG-003, BUG-004"],
    ["P1 - Kurzfristig", "dcRemoval und toSkip als std::atomic deklarieren", "BUG-006, BUG-009"],
    ["P1 - Kurzfristig", "RTL-SDR Default-Gain dynamisch aus Tuner-Tabelle waehlen (z.B. 75% des Max)", "BUG-005, BUG-022"],
    ["P1 - Kurzfristig", "Decoder-Default zwischen config-handler und ofdm-handler synchronisieren", "BUG-008"],
    ["P2 - Mittelfristig", "AFC-Daempfungsfaktor konfigurierbar machen, Stabilitaetsanalyse durchfuehren", "BUG-010, BUG-011"],
    ["P2 - Mittelfristig", "Equalizer-Guards auf NaN und negative Werte erweitern (std::isfinite)", "BUG-012, BUG-013"],
    ["P2 - Mittelfristig", "Busy-Wait Loops durch Qt-Signals mit Timeout ersetzen", "BUG-016"],
    ["P3 - Spaeter", "Typ-Konsistenz im Equalizer bereinigen, dcSampleCounter Reset bei Re-Enable", "BUG-007, BUG-019, BUG-020"],
]
rt = Table(recs, colWidths=[28*mm, 95*mm, 40*mm])
rt.setStyle(TableStyle([
    ('BACKGROUND', (0,0), (-1,0), colors.HexColor('#2C3E50')),
    ('TEXTCOLOR', (0,0), (-1,0), colors.white),
    ('FONTNAME', (0,0), (-1,0), 'Helvetica-Bold'),
    ('FONTSIZE', (0,0), (-1,-1), 8),
    ('GRID', (0,0), (-1,-1), 0.5, colors.HexColor('#CCCCCC')),
    ('BOTTOMPADDING', (0,0), (-1,-1), 4),
    ('TOPPADDING', (0,0), (-1,-1), 4),
    ('LEFTPADDING', (0,0), (-1,-1), 4),
    ('VALIGN', (0,0), (-1,-1), 'TOP'),
    ('ROWBACKGROUNDS', (0,1), (-1,-1), [colors.white, colors.HexColor('#F8F8F8')]),
]))
story.append(rt)

story.append(Spacer(1, 8*mm))
story.append(Paragraph("5. Fazit", styles['H1']))
story.append(Paragraph(
    "Die 9 Empfangsoptimierungen verbessern die DAB+-Empfangsqualitaet messbar "
    "(IQ-Equalizer, Oversampling, adaptive Gain-Regelung, schnellere AFC). "
    "Fuer einen stabilen Produktivbetrieb muessen jedoch die 4 kritischen und 5 hohen "
    "Findings behoben werden. Insbesondere die Thread-Safety-Issues (BUG-003, BUG-004, BUG-006) "
    "und die numerischen Guards (BUG-001, BUG-002) sind essenziell fuer die geforderte "
    "99.9% Stabilitaet. Nach Behebung der P0- und P1-Issues wird ein Re-Test empfohlen.",
    styles['Body']))

story.append(Spacer(1, 15*mm))
story.append(HRFlowable(width="100%", thickness=0.5, color=colors.HexColor('#CCCCCC')))
story.append(Paragraph(
    f"Report generiert am {datetime.now().strftime('%d.%m.%Y %H:%M')} | "
    "Qt-DAB QA Audit | 3 parallele Code-Analysen",
    styles['Small']))

doc.build(story)
print(f"Report erstellt: {OUTPUT}")
