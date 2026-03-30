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

#include	"unified-timer-model.h"
#include	"settingNames.h"
#include	<QJsonDocument>
#include	<QJsonObject>
#include	<QJsonArray>
#include	<QFile>
#include	<QDir>
#include	<QColor>
#include	<algorithm>

#define	TIMER_POLL_INTERVAL_MS	30000	// check every 30 seconds
#define	TIMER_FIRE_LEAD_SECONDS	15	// fire 15s before start

	UnifiedTimerModel::UnifiedTimerModel (QSettings *s,
	                                      QObject *parent):
	                                      QAbstractTableModel (parent),
	                                      settings (s),
	                                      idCounter (0) {
	timerFilePath = settings->value (TIMER_FILE, "").toString ();
	if (timerFilePath.isEmpty ()) {
	   QString home = QDir::homePath ();
	   timerFilePath = home + "/.qt-dab-timers.json";
	   settings->setValue (TIMER_FILE, timerFilePath);
	}

	loadFromFile ();

	pollTimer.setInterval (TIMER_POLL_INTERVAL_MS);
	pollTimer.setSingleShot (false);
	connect (&pollTimer, &QTimer::timeout,
	         this, &UnifiedTimerModel::checkTimers);
	if (!timers.empty ())
	   pollTimer.start ();
}

	UnifiedTimerModel::~UnifiedTimerModel () {
	pollTimer.stop ();
	saveToFile ();
}

int	UnifiedTimerModel::rowCount (const QModelIndex &parent) const {
	if (parent.isValid ())
	   return 0;
	return (int)timers.size ();
}

int	UnifiedTimerModel::columnCount (const QModelIndex &parent) const {
	if (parent.isValid ())
	   return 0;
	return COL_COUNT;
}

QVariant UnifiedTimerModel::data (const QModelIndex &index, int role) const {
	if (!index.isValid () || index.row () >= (int)timers.size ())
	   return QVariant ();

	const TimerEntry &entry = timers [index.row ()];

	if (role == Qt::DisplayRole) {
	   switch (index.column ()) {
	      case COL_TYPE:
	         return timerTypeToString (entry.type);
	      case COL_SERVICE:
	         return entry.serviceName;
	      case COL_DATETIME:
	         return entry.startTime.toString ("dd.MM.yyyy HH:mm");
	      case COL_DURATION:
	         if (entry.durationMinutes > 0)
	            return QString::number (entry.durationMinutes) + " min";
	         return "-";
	      case COL_TITLE:
	         return entry.programTitle.isEmpty () ?
	                entry.serviceName : entry.programTitle;
	      case COL_STATUS:
	         if (entry.fired)
	            return tr ("Läuft");
	         return entry.active ? tr ("Wartend") : tr ("Inaktiv");
	      case COL_DELETE:
	         return tr ("X");
	      default:
	         return QVariant ();
	   }
	}

	if (role == Qt::ForegroundRole) {
	   // status column: red when recording is active
	   if (index.column () == COL_STATUS && entry.fired)
	      return QColor (0xFF, 0x00, 0x00);	// bright red

	   switch (entry.type) {
	      case TimerType::ManualSwitch:
	         return QColor (0x00, 0xCC, 0x00);	// green
	      case TimerType::ManualRecord:
	         return QColor (0xCC, 0x00, 0x00);	// red
	      case TimerType::EpgSwitch:
	         return QColor (0x00, 0x99, 0xCC);	// blue
	      case TimerType::EpgRecord:
	         return QColor (0xCC, 0x66, 0x00);	// orange
	   }
	}

	if (role == Qt::ToolTipRole) {
	   return QString ("%1: %2 @ %3 (%4)")
	          .arg (timerTypeToString (entry.type))
	          .arg (entry.serviceName)
	          .arg (entry.startTime.toString ("dd.MM.yyyy HH:mm"))
	          .arg (entry.channelName);
	}

	if (role == Qt::UserRole) {
	   return entry.id;
	}

	return QVariant ();
}

QVariant UnifiedTimerModel::headerData (int section,
	                                Qt::Orientation orientation,
	                                int role) const {
	if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
	   return QVariant ();

	switch (section) {
	   case COL_TYPE:	return tr ("Typ");
	   case COL_SERVICE:	return tr ("Service");
	   case COL_DATETIME:	return tr ("Datum/Zeit");
	   case COL_DURATION:	return tr ("Dauer");
	   case COL_TITLE:	return tr ("Titel");
	   case COL_STATUS:	return tr ("Status");
	   case COL_DELETE:	return "";
	   default:		return QVariant ();
	}
}

int	UnifiedTimerModel::addTimer (TimerType type,
	                             const QString &serviceName,
	                             const QString &channelName,
	                             const QDateTime &startTime,
	                             int durationMinutes,
	                             const QString &programTitle) {
	TimerEntry entry;
	entry.id		= nextId ();
	entry.type		= type;
	entry.serviceName	= serviceName;
	entry.channelName	= channelName;
	entry.startTime		= startTime;
	entry.durationMinutes	= durationMinutes;
	entry.active		= true;
	entry.fired		= false;
	entry.programTitle	= programTitle;

	beginResetModel ();
	timers.push_back (entry);
	sortByStartTime ();
	endResetModel ();

	saveToFile ();

	if (!pollTimer.isActive ())
	   pollTimer.start ();

	return entry.id;
}

int	UnifiedTimerModel::findConflict (const QDateTime &startTime,
	                                  int durationMinutes) const {
	int durSecs = durationMinutes > 0 ? durationMinutes * 60 : 300;
	QDateTime newEnd = startTime.addSecs (durSecs);

	for (int i = 0; i < (int)timers.size (); i++) {
	   if (!timers [i].active)
	      continue;
	   int existDur = timers [i].durationMinutes > 0
	                  ? timers [i].durationMinutes * 60 : 300;
	   QDateTime existEnd = timers [i].startTime.addSecs (existDur);
	   // overlap: new starts before existing ends AND new ends after existing starts
	   if (startTime < existEnd && newEnd > timers [i].startTime)
	      return i;
	}
	return -1;
}

void	UnifiedTimerModel::markFired (int id) {
	for (int i = 0; i < (int)timers.size (); i++) {
	   if (timers [i].id == id) {
	      timers [i].fired = true;
	      emit dataChanged (index (i, 0), index (i, COL_COUNT - 1));
	      saveToFile ();
	      return;
	   }
	}
}

void	UnifiedTimerModel::removeTimer (int row) {
	if (row < 0 || row >= (int)timers.size ())
	   return;

	beginRemoveRows (QModelIndex (), row, row);
	timers.erase (timers.begin () + row);
	endRemoveRows ();

	saveToFile ();

	if (timers.empty ())
	   pollTimer.stop ();
}

void	UnifiedTimerModel::removeTimerById (int id) {
	for (int i = 0; i < (int)timers.size (); i++) {
	   if (timers [i].id == id) {
	      removeTimer (i);
	      return;
	   }
	}
}

void	UnifiedTimerModel::clearAll () {
	beginResetModel ();
	timers.clear ();
	endResetModel ();
	saveToFile ();
	pollTimer.stop ();
}

int	UnifiedTimerModel::timerCount () const {
	return (int)timers.size ();
}

const TimerEntry &UnifiedTimerModel::timerAt (int row) const {
	return timers [row];
}

void	UnifiedTimerModel::checkTimers () {
QDateTime now = QDateTime::currentDateTime ();
std::vector<int> toRemove;
bool changed = false;

	for (int i = 0; i < (int)timers.size (); i++) {
	   TimerEntry &entry = timers [i];
	   if (!entry.active)
	      continue;

	   // check if a fired timer has completed (duration elapsed)
	   if (entry.fired) {
	      int durSecs = entry.durationMinutes > 0
	                    ? entry.durationMinutes * 60 : 0;
	      QDateTime endTime = entry.startTime.addSecs (durSecs);
	      if (now >= endTime) {
	         toRemove.push_back (i);
	      }
	      continue;
	   }

	   int secsUntil = now.secsTo (entry.startTime);

	   // timer is due (within lead time) or overdue (e.g. after restart)
	   if (secsUntil <= TIMER_FIRE_LEAD_SECONDS) {
	      // check if timer is already past its end time (missed)
	      int durSecs = entry.durationMinutes > 0
	                    ? entry.durationMinutes * 60 : 0;
	      QDateTime endTime = entry.startTime.addSecs (durSecs);
	      if (durSecs > 0 && now >= endTime) {
	         // timer completely missed, remove it
	         toRemove.push_back (i);
	         continue;
	      }

	      switch (entry.type) {
	         case TimerType::ManualSwitch:
	         case TimerType::EpgSwitch:
	            emit switchTimerFired (entry.serviceName,
	                                  entry.channelName);
	            toRemove.push_back (i);  // switch timers remove immediately
	            break;
	         case TimerType::ManualRecord:
	         case TimerType::EpgRecord:
	            emit recordTimerFired (entry.serviceName,
	                                  entry.channelName,
	                                  entry.durationMinutes,
	                                  entry.programTitle);
	            entry.fired = true;  // keep until duration elapsed
	            changed = true;
	            break;
	      }
	   }
	}

	// remove completed/missed timers (reverse order to preserve indices)
	if (!toRemove.empty ()) {
	   beginResetModel ();
	   for (int i = (int)toRemove.size () - 1; i >= 0; i--)
	      timers.erase (timers.begin () + toRemove [i]);
	   endResetModel ();
	   saveToFile ();
	}
	else if (changed) {
	   emit dataChanged (index (0, 0),
	                     index (rowCount () - 1, COL_COUNT - 1));
	   saveToFile ();
	}

	if (timers.empty ())
	   pollTimer.stop ();
}

void	UnifiedTimerModel::sortByStartTime () {
	std::sort (timers.begin (), timers.end (),
	   [] (const TimerEntry &a, const TimerEntry &b) {
	      return a.startTime < b.startTime;
	   });
}

int	UnifiedTimerModel::nextId () {
	return ++idCounter;
}

void	UnifiedTimerModel::saveToFile () {
QJsonArray arr;

	for (const auto &entry : timers) {
	   QJsonObject obj;
	   obj ["id"]		= entry.id;
	   obj ["type"]		= (int)entry.type;
	   obj ["service"]	= entry.serviceName;
	   obj ["channel"]	= entry.channelName;
	   obj ["startTime"]	= entry.startTime.toString (Qt::ISODate);
	   obj ["duration"]	= entry.durationMinutes;
	   obj ["active"]	= entry.active;
	   obj ["fired"]	= entry.fired;
	   obj ["title"]	= entry.programTitle;
	   arr.append (obj);
	}

	QJsonObject root;
	root ["version"]	= 1;
	root ["idCounter"]	= idCounter;
	root ["timers"]		= arr;

	QJsonDocument doc (root);
	QFile file (timerFilePath);
	if (file.open (QIODevice::WriteOnly)) {
	   file.write (doc.toJson ());
	   file.close ();
	}
}

void	UnifiedTimerModel::loadFromFile () {
QFile file (timerFilePath);

	if (!file.exists () || !file.open (QIODevice::ReadOnly))
	   return;

	QJsonDocument doc = QJsonDocument::fromJson (file.readAll ());
	file.close ();

	if (!doc.isObject ())
	   return;

	QJsonObject root = doc.object ();
	idCounter = root ["idCounter"].toInt (0);

	QJsonArray arr = root ["timers"].toArray ();
	QDateTime now = QDateTime::currentDateTime ();

	beginResetModel ();
	timers.clear ();
	for (const auto &val : arr) {
	   QJsonObject obj = val.toObject ();
	   TimerEntry entry;
	   entry.id		= obj ["id"].toInt ();
	   entry.type		= (TimerType)obj ["type"].toInt ();
	   entry.serviceName	= obj ["service"].toString ();
	   entry.channelName	= obj ["channel"].toString ();
	   entry.startTime	= QDateTime::fromString (
	                            obj ["startTime"].toString (), Qt::ISODate);
	   entry.durationMinutes = obj ["duration"].toInt ();
	   entry.active		= obj ["active"].toBool (true);
	   entry.fired		= obj ["fired"].toBool (false);
	   entry.programTitle	= obj ["title"].toString ();

	   // keep timer if: not yet started, or fired but not yet finished
	   int durSecs = entry.durationMinutes > 0
	                 ? entry.durationMinutes * 60 : 0;
	   QDateTime endTime = entry.startTime.addSecs (durSecs);
	   if (entry.startTime > now || (entry.fired && now < endTime))
	      timers.push_back (entry);
	}
	sortByStartTime ();
	endResetModel ();
}

void	UnifiedTimerModel::importOldScheduler (const QString &schedulerFile) {
QFile file (schedulerFile);

	if (!file.exists () || !file.open (QIODevice::ReadOnly))
	   return;

	QTextStream in (&file);
	QDate referenceDate;
	QDateTime now = QDateTime::currentDateTime ();

	while (!in.atEnd ()) {
	   QString line = in.readLine ().trimmed ();

	   if (line.startsWith ("//") || line.length () < 10)
	      continue;

	   if (line.startsWith ("reference:")) {
	      QStringList parts = line.split (" ", Qt::SkipEmptyParts);
	      if (parts.size () >= 4) {
	         int year   = parts [1].toInt ();
	         int month  = parts [2].toInt ();
	         int day    = parts [3].toInt ();
	         referenceDate = QDate (year, month, day);
	      }
	      continue;
	   }

	   if (line.startsWith ("element:")) {
	      QStringList parts = line.split ('|');
	      if (parts.size () != 3)
	         continue;

	      QString service = parts [1].trimmed ();
	      QStringList nums = parts [2].trimmed ().split (' ',
	                                       Qt::SkipEmptyParts);
	      if (nums.size () < 3)
	         continue;

	      int delayDays = nums [0].toInt ();
	      int hours     = nums [1].toInt ();
	      int minutes   = nums [2].toInt ();

	      QDate wakeupDate = referenceDate.addDays (delayDays);
	      QTime wakeupTime (hours, minutes, 0);
	      QDateTime dt (wakeupDate, wakeupTime);

	      if (dt > now) {
	         addTimer (TimerType::ManualSwitch, service, "", dt);
	      }
	   }
	}
	file.close ();
}

QString	UnifiedTimerModel::timerTypeToString (TimerType t) {
	switch (t) {
	   case TimerType::ManualSwitch:	return "Umschalten";
	   case TimerType::ManualRecord:	return "Aufnahme";
	   case TimerType::EpgSwitch:		return "EPG-Umschalten";
	   case TimerType::EpgRecord:		return "EPG-Aufnahme";
	}
	return "?";
}

QString	UnifiedTimerModel::timerTypeToIcon (TimerType t) {
	switch (t) {
	   case TimerType::ManualSwitch:	return "SW";
	   case TimerType::ManualRecord:	return "REC";
	   case TimerType::EpgSwitch:		return "EPG-SW";
	   case TimerType::EpgRecord:		return "EPG-REC";
	}
	return "?";
}
