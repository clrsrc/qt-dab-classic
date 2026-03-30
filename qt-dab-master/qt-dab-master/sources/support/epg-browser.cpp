/*
 *    Copyright (C) 2025
 *    Qt-DAB Winamp Edition
 *
 *    This file is part of Qt-DAB
 *
 *    Qt-DAB is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 */

#include	"epg-browser.h"
#include	"unified-timer-model.h"
#include	<algorithm>
#include	<QFile>
#include	<QDir>
#include	<QDomDocument>
#include	<QDateTime>
#include	<QTimeZone>
#include	<QRegularExpression>
#include	<QHeaderView>
#include	<QGridLayout>
#include	<QMessageBox>

	EpgBrowser::EpgBrowser (const QString &epgFilePath,
	                         UnifiedTimerModel *timerModel,
	                         QSettings *settings,
	                         QWidget *parent):
	                         superFrame (parent),
	                         theTimerModel (timerModel),
	                         theSettings (settings),
	                         path_for_files (epgFilePath),
	                         currentEid (0),
	                         currentSid (0),
	                         dateOffset (0) {
	setWindowTitle ("EPG Browser");
	resize (600, 450);
	setupUi ();
}

	EpgBrowser::~EpgBrowser () {
}

void	EpgBrowser::setupUi () {
QVBoxLayout *mainLayout = new QVBoxLayout (this);

	// top bar: nav + service info
	QHBoxLayout *topBar = new QHBoxLayout ();

	prevButton = new QPushButton ("<", this);
	prevButton->setFixedWidth (30);
	prevButton->setToolTip (tr ("Vorheriger Tag"));
	connect (prevButton, &QPushButton::clicked,
	         this, &EpgBrowser::handlePrevDay);
	topBar->addWidget (prevButton);

	serviceLabel = new QLabel (this);
	topBar->addWidget (serviceLabel);

	serviceLogo = new QLabel (this);
	serviceLogo->setFixedSize (50, 50);
	topBar->addWidget (serviceLogo);

	dateLabel = new QLabel (this);
	topBar->addWidget (dateLabel);

	nextButton = new QPushButton (">", this);
	nextButton->setFixedWidth (30);
	nextButton->setToolTip (tr ("Naechster Tag"));
	connect (nextButton, &QPushButton::clicked,
	         this, &EpgBrowser::handleNextDay);
	topBar->addWidget (nextButton);

	mainLayout->addLayout (topBar);

	// time range filter
	QHBoxLayout *filterBar = new QHBoxLayout ();
	filterBar->addWidget (new QLabel (tr ("Zeitraum:"), this));
	timeRangeCombo = new QComboBox (this);
	timeRangeCombo->addItem (tr ("Alle"), 0);
	timeRangeCombo->addItem (tr ("Naechste 2h"), 120);
	timeRangeCombo->addItem (tr ("Naechste 6h"), 360);
	timeRangeCombo->addItem (tr ("Naechste 12h"), 720);
	connect (timeRangeCombo, QOverload<int>::of (&QComboBox::currentIndexChanged),
	         this, &EpgBrowser::handleTimeRangeChanged);
	filterBar->addWidget (timeRangeCombo);
	filterBar->addStretch ();
	mainLayout->addLayout (filterBar);

	// program table
	programTable = new QTableWidget (0, 5, this);
	programTable->setHorizontalHeaderLabels (
	   QStringList () << tr ("Zeit") << tr ("Dauer")
	                  << tr ("Titel") << tr ("Umschalten")
	                  << tr ("Aufnehmen"));
	programTable->horizontalHeader ()->setStretchLastSection (false);
	programTable->setColumnWidth (0, 70);
	programTable->setColumnWidth (1, 60);
	programTable->setColumnWidth (2, 250);
	programTable->setColumnWidth (3, 80);
	programTable->setColumnWidth (4, 80);
	programTable->setSelectionBehavior (QAbstractItemView::SelectRows);
	programTable->setEditTriggers (QAbstractItemView::NoEditTriggers);
	programTable->verticalHeader ()->hide ();
	mainLayout->addWidget (programTable);
}

void	EpgBrowser::setUp (const QDate &date, uint32_t Eid,
	                    uint16_t SId, const QString &serviceName,
	                    const QString &channelName) {
	startDate		= date;
	currentEid		= Eid;
	currentSid		= SId;
	currentServiceName	= serviceName;
	currentChannelName	= channelName;
	dateOffset		= 0;

	serviceLabel->setText (serviceName);
	serviceLogo->setPixmap (QPixmap ());
	loadAndDisplay (0);
}

void	EpgBrowser::addLogo (const QPixmap &p) {
	serviceLogo->setPixmap (p.scaled (50, 50, Qt::KeepAspectRatio));
}

void	EpgBrowser::clear () {
	int rows = programTable->rowCount ();
	for (int i = rows - 1; i >= 0; i--)
	   programTable->removeRow (i);
	displayedPrograms.clear ();
}

void	EpgBrowser::handlePrevDay () {
	dateOffset--;
	loadAndDisplay (dateOffset);
}

void	EpgBrowser::handleNextDay () {
	dateOffset++;
	loadAndDisplay (dateOffset);
}

void	EpgBrowser::handleTimeRangeChanged (int index) {
	(void)index;
	loadAndDisplay (dateOffset);
}

static bool programStartsOnDate (const programDescriptor &prog, const QDate &date) {
	return prog.startTime.date () == date;
}

static bool programCarriesIntoDate (const programDescriptor &prog, const QDate &date) {
	// program started before this date but runs past midnight into it
	if (prog.startTime.date () >= date)
	   return false;
	int durSecs = prog.duration > 0 ? prog.duration * 60 : 3600;
	QDateTime progEnd = prog.startTime.addSecs (durSecs);
	QDateTime dayStart (date, QTime (0, 0, 0));
	return progEnd > dayStart;
}

// Extract sub-entries from shortDescription like "06:30-06:35 Nachrichten06:50-06:59 Interview..."
// These times are in local time (as broadcast by DLF)
static void extractSubEntries (const programDescriptor &parent,
                               std::vector<programDescriptor> &out) {
	QString desc = parent.shortDescriptor;
	if (desc.isEmpty ())
	   return;

	// match patterns like "06:30-06:35 Title" or "12:45-12:48 Sport"
	static QRegularExpression rx ("(\\d{2}:\\d{2})-(\\d{2}:\\d{2})\\s*");
	QRegularExpressionMatchIterator it = rx.globalMatch (desc);

	QDate parentDate = parent.startTime.date ();

	while (it.hasNext ()) {
	   QRegularExpressionMatch m = it.next ();
	   QTime startT = QTime::fromString (m.captured (1), "HH:mm");
	   QTime endT = QTime::fromString (m.captured (2), "HH:mm");
	   if (!startT.isValid () || !endT.isValid ())
	      continue;

	   // extract title: text between end of this match and start of next time pattern
	   int titleStart = m.capturedEnd ();
	   int titleEnd = desc.length ();
	   if (it.hasNext ()) {
	      // peek: find next digit sequence that looks like HH:MM
	      int nextMatch = desc.indexOf (QRegularExpression ("\\d{2}:\\d{2}-"), titleStart);
	      if (nextMatch > 0)
	         titleEnd = nextMatch;
	   }
	   QString title = desc.mid (titleStart, titleEnd - titleStart).trimmed ();
	   if (title.isEmpty ())
	      continue;
	   // truncate at reasonable length
	   if (title.length () > 60)
	      title = title.left (60) + "...";

	   // compute duration in minutes
	   int durMins = startT.secsTo (endT) / 60;
	   if (durMins <= 0)
	      durMins += 24 * 60;  // across midnight

	   // build sub-entry with local time
	   programDescriptor sub;
	   sub.valid = true;
	   sub.mediumName = title;
	   sub.startTime = QDateTime (parentDate, startT);
	   sub.duration = durMins;
	   sub.shortDescriptor = title;
	   out.push_back (sub);
	}
}

void	EpgBrowser::loadAndDisplay (int offset) {
	QDate targetDate = startDate.addDays (offset);
	dateLabel->setText (targetDate.toString ("dd.MM.yyyy - dddd"));
	clear ();

	// collect programs for targetDate from multiple files
	std::vector<programDescriptor> allPrograms;
	bool isToday = (targetDate == QDate::currentDate ());

	// Step 1: carry-over from previous day (NOT for today - rule 1)
	if (!isToday) {
	   scheduleDescriptor prevSched = loadSchedule (targetDate.addDays (-1),
	                                                 currentEid, currentSid);
	   if (prevSched.valid)
	      for (auto &p : prevSched.thePrograms)
	         if (p.valid && programCarriesIntoDate (p, targetDate))
	            allPrograms.push_back (p);

	   // also check the target day file for carry-overs
	   scheduleDescriptor sched = loadSchedule (targetDate,
	                                             currentEid, currentSid);
	   if (sched.valid)
	      for (auto &p : sched.thePrograms)
	         if (p.valid && programCarriesIntoDate (p, targetDate))
	            allPrograms.push_back (p);
	}

	// Step 2: programs starting on targetDate
	scheduleDescriptor sched = loadSchedule (targetDate,
	                                          currentEid, currentSid);
	if (sched.valid)
	   for (auto &p : sched.thePrograms)
	      if (p.valid && programStartsOnDate (p, targetDate))
	         allPrograms.push_back (p);

	scheduleDescriptor nextSched = loadSchedule (targetDate.addDays (1),
	                                              currentEid, currentSid);
	if (nextSched.valid)
	   for (auto &p : nextSched.thePrograms)
	      if (p.valid && programStartsOnDate (p, targetDate))
	         allPrograms.push_back (p);

	// extract sub-entries from descriptions (e.g. "06:30-06:35 Nachrichten")
	std::vector<programDescriptor> subEntries;
	for (const auto &p : allPrograms)
	   if (p.duration > 30)
	      extractSubEntries (p, subEntries);
	for (auto &sub : subEntries)
	   allPrograms.push_back (sub);

	// sort by start time
	std::sort (allPrograms.begin (), allPrograms.end (),
	   [] (const programDescriptor &a, const programDescriptor &b) {
	      return a.startTime < b.startTime;
	   });

	// remove duplicates (same start time + name)
	auto last = std::unique (allPrograms.begin (), allPrograms.end (),
	   [] (const programDescriptor &a, const programDescriptor &b) {
	      return a.startTime == b.startTime && a.mediumName == b.mediumName;
	   });
	allPrograms.erase (last, allPrograms.end ());

	// display
	displayedPrograms.clear ();
	int row = 0;
	int filterMinutes = timeRangeCombo->currentData ().toInt ();
	QDateTime now = QDateTime::currentDateTime ();

	for (const auto &prog : allPrograms) {
	   int durSecs = prog.duration > 0 ? prog.duration * 60 : 3600;
	   QDateTime progEnd = prog.startTime.addSecs (durSecs);

	   if (filterMinutes > 0) {
	      // time range filter: running + starting within range
	      bool isRunning = (prog.startTime <= now && progEnd > now);
	      bool startsInRange = (prog.startTime > now &&
	                            now.secsTo (prog.startTime) <= filterMinutes * 60);
	      if (!isRunning && !startsInRange)
	         continue;
	   }
	   else if (isToday) {
	      // "Alle" for today: only running + future programs
	      if (progEnd <= now)
	         continue;
	   }

	   displayedPrograms.push_back (prog);
	   addProgramRow (prog, row);
	   row++;
	}

	if (row == 0)
	   serviceLabel->setText (currentServiceName + " - " +
	                          tr ("Keine EPG-Daten fuer diesen Tag"));
	else
	   serviceLabel->setText (currentServiceName +
	                          " (" + QString::number (row) + " Sendungen)");

	fprintf (stderr, "EPG: %d programs for %s (from %d total)\n",
	         row, targetDate.toString ("dd.MM.yyyy").toUtf8 ().data (),
	         (int)allPrograms.size ());
	show ();
}

static QDateTime parseDateTime (const QString &s) {
	// parse "2026-3-27T21:41" or "2026-3-28T05:01" format
	// DAB EPG times are in UTC, convert to local time
	QDateTime dt = QDateTime::fromString (s, "yyyy-M-dTHH:mm");
	if (!dt.isValid ())
	   dt = QDateTime::fromString (s, "yyyy-M-dTH:mm");
	if (!dt.isValid ())
	   dt = QDateTime::fromString (s, "yyyy-MM-ddTHH:mm");
	if (!dt.isValid ())
	   dt = QDateTime::fromString (s, "yyyy-MM-ddTHH:mm:ss");
	if (!dt.isValid ())
	   dt = QDateTime::fromString (s, Qt::ISODate);
	if (!dt.isValid ()) {
	   fprintf (stderr, "EPG: FAILED to parse '%s'\n", s.toUtf8 ().data ());
	   return dt;
	}
	dt.setTimeZone (QTimeZone::utc ());
	return dt.toLocalTime ();
}

static int parseDuration (const QString &s) {
	// parse ISO 8601 duration like "PT05M", "PT04H55M", "PT01H55M"
	int hours = 0, minutes = 0;
	QString d = s.toUpper ();
	if (d.startsWith ("PT"))
	   d = d.mid (2);
	int hPos = d.indexOf ('H');
	if (hPos >= 0) {
	   hours = d.left (hPos).toInt ();
	   d = d.mid (hPos + 1);
	}
	int mPos = d.indexOf ('M');
	if (mPos >= 0)
	   minutes = d.left (mPos).toInt ();
	return hours * 60 + minutes;
}

scheduleDescriptor EpgBrowser::loadSchedule (const QDate &date,
	                                      uint32_t Eid, uint32_t SId) {
	// build filename: path/EID/YYYYMMDD_SID_SI.xml
	char temp [80];
	sprintf (temp, "%X/%4d%02d%02d_%4X_SI.xml",
	         Eid, date.year (), date.month (), date.day (), SId);
	QString fileName = path_for_files + QString (temp);
	fprintf (stderr, "EPG: looking for %s\n", fileName.toUtf8 ().data ());

	QFile f (QDir::toNativeSeparators (fileName));
	if (!f.open (QIODevice::ReadOnly)) {
	   fprintf (stderr, "EPG: file not found\n");
	   scheduleDescriptor empty;
	   return empty;
	}

	// read and clean XML - remove control characters that break parsing
	QByteArray rawData = f.readAll ();
	f.close ();
	for (int i = 0; i < rawData.size (); i++) {
	   char c = rawData [i];
	   if (c < 0x20 && c != '\n' && c != '\r' && c != '\t')
	      rawData [i] = ' ';
	}

	QDomDocument doc;
	QString errorMsg;
	int errorLine = 0;
	if (!doc.setContent (rawData, &errorMsg, &errorLine)) {
	   fprintf (stderr, "EPG: XML parse error at line %d: %s\n",
	            errorLine, errorMsg.toUtf8 ().data ());
	   scheduleDescriptor empty;
	   return empty;
	}

	QDomElement root = doc.firstChildElement ("epg");
	if (root.isNull ()) {
	   scheduleDescriptor empty;
	   return empty;
	}

	QDomElement theSchedule = root.firstChildElement ("schedule");
	if (theSchedule.isNull ()) {
	   scheduleDescriptor empty;
	   return empty;
	}

	// parse directly without xmlExtractor filtering
	scheduleDescriptor theDescriptor;
	theDescriptor.valid = true;
	theDescriptor.name = currentServiceName;
	theDescriptor.Eid = Eid;
	theDescriptor.Sid = SId;

	int count = 0;
	for (QDomElement prog = theSchedule.firstChildElement ("programme");
	     !prog.isNull ();
	     prog = prog.nextSiblingElement ("programme")) {

	   programDescriptor pd;
	   pd.valid = false;

	   // get names
	   QDomElement medName = prog.firstChildElement ("mediumName");
	   QDomElement longName = prog.firstChildElement ("longName");
	   QDomElement shortName = prog.firstChildElement ("shortName");
	   pd.mediumName = medName.isNull () ? "" : medName.text ();
	   pd.longName = longName.isNull () ? "" : longName.text ();
	   pd.shortName = shortName.isNull () ? "" : shortName.text ();

	   // get time from location/time
	   QDomElement location = prog.firstChildElement ("location");
	   if (!location.isNull ()) {
	      QDomElement timeEl = location.firstChildElement ("time");
	      if (!timeEl.isNull ()) {
	         QString timeStr = timeEl.attribute ("time", "");
	         QString durStr = timeEl.attribute ("duration", "");
	         pd.startTime = parseDateTime (timeStr);
	         pd.duration = parseDuration (durStr);
	         if (pd.startTime.isValid ())
	            pd.valid = true;
	      }
	   }

	   // get description
	   QDomElement mediaDesc = prog.firstChildElement ("mediaDescription");
	   if (!mediaDesc.isNull ()) {
	      QDomElement shortDesc = mediaDesc.firstChildElement ("shortDescription");
	      QDomElement longDesc = mediaDesc.firstChildElement ("longDescription");
	      pd.shortDescriptor = shortDesc.isNull () ? "" : shortDesc.text ();
	      pd.longDescriptor = longDesc.isNull () ? "" : longDesc.text ();
	   }

	   if (pd.valid) {
	      theDescriptor.thePrograms.push_back (pd);
	      count++;
	   }
	   else {
	      fprintf (stderr, "EPG: invalid entry (name='%s')\n",
	               pd.mediumName.toUtf8 ().data ());
	   }
	}

	fprintf (stderr, "EPG: parsed %d valid programmes from %s\n",
	         count, fileName.toUtf8 ().data ());
	return theDescriptor;
}

void	EpgBrowser::displaySchedule (const scheduleDescriptor &sched) {
	(void)sched;  // no longer used, logic moved to loadAndDisplay
}

void	EpgBrowser::addProgramRow (const programDescriptor &p, int row) {
	programTable->insertRow (row);

	// time - show 00:00 for carry-over programs from previous day
	QTableWidgetItem *timeItem = new QTableWidgetItem;
	QDate viewDate = startDate.addDays (dateOffset);
	if (p.startTime.date () < viewDate)
	   timeItem->setText ("00:00");
	else
	   timeItem->setText (p.startTime.time ().toString ("HH:mm"));
	programTable->setItem (row, 0, timeItem);

	// duration - for carry-overs, show remaining time on this day
	QTableWidgetItem *durItem = new QTableWidgetItem;
	int showDuration = p.duration;
	if (p.startTime.date () < viewDate) {
	   // carry-over: remaining = endTime - midnight
	   QDateTime dayStart (viewDate, QTime (0, 0, 0));
	   int durSecs = p.duration > 0 ? p.duration * 60 : 3600;
	   QDateTime progEnd = p.startTime.addSecs (durSecs);
	   showDuration = dayStart.secsTo (progEnd) / 60;
	   if (showDuration < 1) showDuration = 1;
	}
	if (showDuration < 60)
	   durItem->setText (QString::number (showDuration) + "m");
	else
	   durItem->setText (QString::number (showDuration / 60) + "h " +
	                     QString::number (showDuration % 60) + "m");
	programTable->setItem (row, 1, durItem);

	// title
	QTableWidgetItem *titleItem = new QTableWidgetItem;
	titleItem->setText (bestProgramName (p));
	titleItem->setToolTip (p.longDescriptor.isEmpty () ?
	                        p.shortDescriptor : p.longDescriptor);
	programTable->setItem (row, 2, titleItem);

	// switch button
	QPushButton *switchBtn = new QPushButton (tr ("Umschalten"), this);
	switchBtn->setProperty ("programRow", row);
	connect (switchBtn, &QPushButton::clicked,
	         this, &EpgBrowser::handleSwitchClicked);
	programTable->setCellWidget (row, 3, switchBtn);

	// record button
	QPushButton *recBtn = new QPushButton (tr ("Aufnehmen"), this);
	recBtn->setProperty ("programRow", row);
	connect (recBtn, &QPushButton::clicked,
	         this, &EpgBrowser::handleRecordClicked);
	programTable->setCellWidget (row, 4, recBtn);
}

QString	EpgBrowser::bestProgramName (const programDescriptor &p) {
	if (!p.longName.isEmpty ())
	   return p.longName;
	if (!p.mediumName.isEmpty ())
	   return p.mediumName;
	if (!p.shortName.isEmpty ())
	   return p.shortName;
	if (!p.longDescriptor.isEmpty ())
	   return p.longDescriptor.left (50);
	if (!p.shortDescriptor.isEmpty ())
	   return p.shortDescriptor.left (50);
	return tr ("Kein Titel");
}

void	EpgBrowser::handleSwitchClicked () {
	QPushButton *btn = qobject_cast<QPushButton *>(sender ());
	if (btn == nullptr)
	   return;

	int row = btn->property ("programRow").toInt ();
	if (row < 0 || row >= (int)displayedPrograms.size ())
	   return;

	const programDescriptor &prog = displayedPrograms [row];
	QString title = bestProgramName (prog);

	// always create a timer entry so user can see it in timer list
	if (prog.startTime > QDateTime::currentDateTime ()) {
	   int conflict = theTimerModel->findConflict (prog.startTime, 0);
	   if (conflict >= 0) {
	      const TimerEntry &existing = theTimerModel->timerAt (conflict);
	      QString msg = tr ("Timer-Konflikt!\n\n"
	         "Bestehender Timer:\n  %1 um %2\n\n"
	         "Neuer Timer:\n  %3 um %4\n\n"
	         "Bestehenden Timer ersetzen?")
	         .arg (existing.programTitle.isEmpty () ?
	               existing.serviceName : existing.programTitle)
	         .arg (existing.startTime.toString ("dd.MM. HH:mm"))
	         .arg (title)
	         .arg (prog.startTime.toString ("dd.MM. HH:mm"));
	      int ret = QMessageBox::question (this, tr ("Timer-Konflikt"),
	                   msg, QMessageBox::Yes | QMessageBox::No);
	      if (ret == QMessageBox::No)
	         return;
	      theTimerModel->removeTimer (conflict);
	   }
	   theTimerModel->addTimer (TimerType::EpgSwitch,
	                            currentServiceName,
	                            currentChannelName,
	                            prog.startTime,
	                            0,
	                            title);
	   fprintf (stderr, "EPG: switch timer created for '%s' at %s\n",
	            title.toUtf8 ().data (),
	            prog.startTime.toString ().toUtf8 ().data ());
	}
	else {
	   // currently running -> switch now
	   emit switchRequested (currentServiceName, currentChannelName);
	   fprintf (stderr, "EPG: immediate switch to '%s'\n",
	            currentServiceName.toUtf8 ().data ());
	}
}

void	EpgBrowser::handleRecordClicked () {
	QPushButton *btn = qobject_cast<QPushButton *>(sender ());
	if (btn == nullptr)
	   return;

	int row = btn->property ("programRow").toInt ();
	if (row < 0 || row >= (int)displayedPrograms.size ())
	   return;

	const programDescriptor &prog = displayedPrograms [row];
	QString title = bestProgramName (prog);
	int duration = prog.duration > 0 ? prog.duration : 60;

	QDateTime startTime = prog.startTime;
	int recDuration = duration;

	bool startNow = (prog.startTime <= QDateTime::currentDateTime ());
	if (startNow) {
	   // program already running -> start now, record remaining time
	   startTime = QDateTime::currentDateTime ();
	   int elapsed = prog.startTime.secsTo (
	                    QDateTime::currentDateTime ()) / 60;
	   recDuration = duration - elapsed;
	   if (recDuration < 1)
	      recDuration = 1;
	}

	// check for timer conflicts
	int conflict = theTimerModel->findConflict (startTime, recDuration);
	if (conflict >= 0) {
	   const TimerEntry &existing = theTimerModel->timerAt (conflict);
	   QString msg = tr ("Timer-Konflikt!\n\n"
	      "Bestehender Timer:\n  %1 um %2\n\n"
	      "Neue Aufnahme:\n  %3 um %4 (%5 min)\n\n"
	      "Bestehenden Timer ersetzen?")
	      .arg (existing.programTitle.isEmpty () ?
	            existing.serviceName : existing.programTitle)
	      .arg (existing.startTime.toString ("dd.MM. HH:mm"))
	      .arg (title)
	      .arg (startTime.toString ("dd.MM. HH:mm"))
	      .arg (recDuration);
	   int ret = QMessageBox::question (this, tr ("Timer-Konflikt"),
	                msg, QMessageBox::Yes | QMessageBox::No);
	   if (ret == QMessageBox::No)
	      return;
	   theTimerModel->removeTimer (conflict);
	}

	int timerId = theTimerModel->addTimer (TimerType::EpgRecord,
	                         currentServiceName,
	                         currentChannelName,
	                         startTime,
	                         recDuration,
	                         title);

	if (startNow) {
	   // start recording immediately and mark timer as fired
	   theTimerModel->markFired (timerId);
	   emit recordRequested (currentServiceName, title, recDuration);
	   fprintf (stderr, "EPG: immediate recording '%s' (%d min)\n",
	            title.toUtf8 ().data (), recDuration);
	}
	else {
	   fprintf (stderr, "EPG: record timer for '%s' at %s (%d min)\n",
	            title.toUtf8 ().data (),
	            startTime.toString ("dd.MM.yyyy HH:mm").toUtf8 ().data (),
	            recDuration);
	}
}
