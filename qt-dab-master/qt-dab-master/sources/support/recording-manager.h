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

#pragma once

#include	<QObject>
#include	<QTimer>
#include	<QString>
#include	<QSettings>

class	RadioInterface;

class RecordingManager : public QObject {
Q_OBJECT
public:
		RecordingManager	(RadioInterface *radio,
		                         QSettings *settings,
		                         QObject *parent = nullptr);
		~RecordingManager	();

	bool	isRecording		() const;
	QString	currentServiceName	() const;
	int	remainingSeconds	() const;

public slots:
	void	startManualRecording	();
	void	startNamedRecording	(const QString &serviceName,
	                                 const QString &title);
	void	stopManualRecording	();
	void	startTimedRecording	(int durationMinutes);
	void	handleRecordTimer	(const QString &serviceName,
	                                 const QString &channelName,
	                                 int durationMinutes);

signals:
	void	recordingStarted	(const QString &serviceName);
	void	recordingStopped	(const QString &serviceName);
	void	switchAndRecord		(const QString &serviceName,
	                                 const QString &channelName,
	                                 int durationMinutes);

private slots:
	void	timedRecordingEnd	();

private:
	RadioInterface		*theRadio;
	QSettings		*theSettings;
	QTimer			stopTimer;
	bool			recording;
	QString			recordingService;
};
