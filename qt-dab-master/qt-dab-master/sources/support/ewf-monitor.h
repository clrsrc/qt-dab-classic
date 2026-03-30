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
#include	<QWidget>
#include	<QLabel>
#include	<QTimer>
#include	<QSettings>
#include	<QDialog>
#include	<QPushButton>

class EwfMonitor : public QObject {
Q_OBJECT
public:
		EwfMonitor	(QSettings *settings,
		                 QWidget *parentWidget = nullptr,
		                 QObject *parent = nullptr);
		~EwfMonitor	();

	bool	isAlarmActive	() const;
	bool	isEnabled	() const;
	void	setEnabled	(bool enabled);

	// call this to set the LED label in the main UI
	void	setIndicatorLabel	(QLabel *label);

public slots:
	void	handleAlarm	(bool active);
	void	setAlarmText	(const QString &text);

signals:
	void	alarmActivated	();
	void	alarmDeactivated ();

private slots:
	void	blinkTimer	();

private:
	void	showAlarmDialog	();
	void	hideAlarmDialog	();
	void	startBlinking	();
	void	stopBlinking	();

	QSettings	*theSettings;
	QWidget		*parentWidget;
	bool		alarmActive;
	bool		enabled;

	QLabel		*indicatorLabel;
	QTimer		blinkTimerObj;
	bool		blinkState;

	QDialog		*alarmDialog;
	QLabel		*alarmMessageLabel;
	QString		alarmText;
};
