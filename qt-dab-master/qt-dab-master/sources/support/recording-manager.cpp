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

#include	"recording-manager.h"
#include	"radio.h"
#include	"settingNames.h"
#include	<QDir>
#include	<QDateTime>
#include	<QRegularExpression>

	RecordingManager::RecordingManager (RadioInterface *radio,
	                                    QSettings *settings,
	                                    QObject *parent):
	                                    QObject (parent),
	                                    theRadio (radio),
	                                    theSettings (settings),
	                                    recording (false) {
	stopTimer.setSingleShot (true);
	connect (&stopTimer, &QTimer::timeout,
	         this, &RecordingManager::timedRecordingEnd);
}

	RecordingManager::~RecordingManager () {
	if (recording)
	   stopManualRecording ();
}

bool	RecordingManager::isRecording () const {
	return recording;
}

QString	RecordingManager::currentServiceName () const {
	return recordingService;
}

int	RecordingManager::remainingSeconds () const {
	if (!stopTimer.isActive ())
	   return 0;
	return stopTimer.remainingTime () / 1000;
}

void	RecordingManager::startManualRecording () {
	if (recording)
	   return;

	recording = true;
	recordingService = theRadio->channelOn () ?
	                   "recording" : "";

	// use existing RadioInterface audio dumping
	theRadio->handleAudiodumpButton ();
	emit recordingStarted (recordingService);
}

void	RecordingManager::startNamedRecording (const QString &serviceName,
	                                        const QString &title) {
	if (recording)
	   return;

	// generate filename: Datum_Uhrzeit_Sender_Titel.wav
	QDateTime now = QDateTime::currentDateTime ();
	QString cleanService = serviceName;
	cleanService.replace (QRegularExpression ("[^a-zA-Z0-9_-]"), "_");
	QString cleanTitle = title;
	cleanTitle.replace (QRegularExpression ("[^a-zA-Z0-9_-]"), "_");
	if (cleanTitle.length () > 40)
	   cleanTitle = cleanTitle.left (40);

	QString fileName = now.toString ("yyyyMMdd_HHmmss") + "_" +
	                   cleanService.trimmed () + "_" +
	                   cleanTitle.trimmed () + ".wav";

	QString path = theSettings->value (RECORDING_PATH,
	                  QDir::homePath () + "/Qt-DAB-recordings/").toString ();
	QDir dir (path);
	if (!dir.exists ())
	   dir.mkpath (".");

	QString fullPath = path + fileName;
	fprintf (stderr, "REC: starting recording to %s\n",
	         fullPath.toUtf8 ().data ());

	recording = true;
	recordingService = serviceName;
	theRadio->theAudioConverter.start_audioDump (fullPath);
	theRadio->audioDumping = true;
	emit recordingStarted (recordingService);
}

void	RecordingManager::stopManualRecording () {
	if (!recording)
	   return;

	stopTimer.stop ();
	recording = false;

	// stop via existing RadioInterface audio dumping
	theRadio->handleAudiodumpButton ();
	emit recordingStopped (recordingService);
	recordingService.clear ();
}

void	RecordingManager::startTimedRecording (int durationMinutes) {
	if (durationMinutes <= 0)
	   return;

	startManualRecording ();
	if (recording)
	   stopTimer.start (durationMinutes * 60 * 1000);
}

void	RecordingManager::handleRecordTimer (const QString &serviceName,
	                                     const QString &channelName,
	                                     int durationMinutes) {
	// signal to RadioInterface to switch service first, then record
	emit switchAndRecord (serviceName, channelName, durationMinutes);
}

void	RecordingManager::timedRecordingEnd () {
	stopManualRecording ();
}
