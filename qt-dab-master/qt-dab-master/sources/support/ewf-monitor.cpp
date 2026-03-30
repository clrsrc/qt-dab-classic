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

#include	"ewf-monitor.h"
#include	"settingNames.h"
#include	<QVBoxLayout>
#include	<QApplication>

	EwfMonitor::EwfMonitor (QSettings *settings,
	                         QWidget *parentWidget,
	                         QObject *parent):
	                         QObject (parent),
	                         theSettings (settings),
	                         parentWidget (parentWidget),
	                         alarmActive (false),
	                         indicatorLabel (nullptr),
	                         blinkState (false),
	                         alarmDialog (nullptr),
	                         alarmMessageLabel (nullptr) {
	enabled = theSettings->value (EWF_ENABLED, true).toBool ();

	blinkTimerObj.setInterval (500);
	connect (&blinkTimerObj, &QTimer::timeout,
	         this, &EwfMonitor::blinkTimer);
}

	EwfMonitor::~EwfMonitor () {
	stopBlinking ();
	hideAlarmDialog ();
}

bool	EwfMonitor::isAlarmActive () const {
	return alarmActive;
}

bool	EwfMonitor::isEnabled () const {
	return enabled;
}

void	EwfMonitor::setEnabled (bool e) {
	enabled = e;
	theSettings->setValue (EWF_ENABLED, enabled);
	if (!enabled && alarmActive) {
	   stopBlinking ();
	   hideAlarmDialog ();
	   alarmActive = false;
	}
}

void	EwfMonitor::setIndicatorLabel (QLabel *label) {
	indicatorLabel = label;
	if (indicatorLabel != nullptr) {
	   indicatorLabel->setText ("EWF");
	   indicatorLabel->setStyleSheet (
	      "QLabel { color: #666666; font-weight: bold; }");
	}
}

void	EwfMonitor::handleAlarm (bool active) {
	if (!enabled)
	   return;

	if (active == alarmActive)
	   return;

	if (active) {
	   // activate alarm - stays until user dismisses
	   if (!alarmActive) {
	      alarmActive = true;
	      startBlinking ();
	      showAlarmDialog ();
	      emit alarmActivated ();
	   }
	}
	// don't auto-dismiss: alarm stays until user clicks "Verstanden"
	// the flag may pulse on/off in DAB, but alarm should persist
}

void	EwfMonitor::blinkTimer () {
	blinkState = !blinkState;
	if (indicatorLabel != nullptr) {
	   if (blinkState) {
	      indicatorLabel->setStyleSheet (
	         "QLabel { color: #FF0000; font-weight: bold; "
	         "background-color: #440000; }");
	      // acoustic alert every other blink (every 1 second)
	      QApplication::beep ();
	   }
	   else
	      indicatorLabel->setStyleSheet (
	         "QLabel { color: #FFFF00; font-weight: bold; "
	         "background-color: #FF0000; }");
	}
}

void	EwfMonitor::startBlinking () {
	blinkState = false;
	blinkTimerObj.start ();
}

void	EwfMonitor::stopBlinking () {
	blinkTimerObj.stop ();
	if (indicatorLabel != nullptr)
	   indicatorLabel->setStyleSheet (
	      "QLabel { color: #666666; font-weight: bold; }");
}

void	EwfMonitor::showAlarmDialog () {
	if (alarmDialog != nullptr)
	   return;

	alarmDialog = new QDialog (parentWidget);
	alarmDialog->setWindowTitle (tr ("NOTFALLWARNUNG - Emergency Warning"));
	alarmDialog->setMinimumSize (400, 200);

	QVBoxLayout *layout = new QVBoxLayout (alarmDialog);

	QLabel *iconLabel = new QLabel (alarmDialog);
	iconLabel->setText ("\xe2\x9a\xa0");	// warning triangle
	iconLabel->setStyleSheet (
	   "QLabel { font-size: 48px; color: #FF0000; }");
	iconLabel->setAlignment (Qt::AlignCenter);
	layout->addWidget (iconLabel);

	QLabel *titleLabel = new QLabel (alarmDialog);
	titleLabel->setText (tr ("NOTFALLWARNUNG"));
	titleLabel->setStyleSheet (
	   "QLabel { font-size: 24px; font-weight: bold; color: #FF0000; }");
	titleLabel->setAlignment (Qt::AlignCenter);
	layout->addWidget (titleLabel);

	alarmMessageLabel = new QLabel (alarmDialog);
	QString displayText = alarmText.isEmpty () ?
	   tr ("Ein Notfallwarnsignal (EWF) wurde empfangen.\n\n"
	       "Emergency Warning Flag ist aktiv.\n"
	       "Bitte beachten Sie offizielle Durchsagen.") :
	   alarmText;
	alarmMessageLabel->setText (displayText);
	alarmMessageLabel->setWordWrap (true);
	alarmMessageLabel->setAlignment (Qt::AlignCenter);
	layout->addWidget (alarmMessageLabel);

	QPushButton *okButton = new QPushButton (tr ("Verstanden"), alarmDialog);
	connect (okButton, &QPushButton::clicked,
	         this, [this] () {
	            alarmActive = false;
	            stopBlinking ();
	            hideAlarmDialog ();
	            emit alarmDeactivated ();
	         });
	layout->addWidget (okButton);

	alarmDialog->setStyleSheet (
	   "QDialog { background-color: #1a0000; }"
	   "QLabel { color: #FFFFFF; }"
	   "QPushButton { background-color: #CC0000; color: white; "
	   "padding: 8px 20px; font-weight: bold; }");

	alarmDialog->show ();
	alarmDialog->raise ();
	alarmDialog->activateWindow ();
}

void	EwfMonitor::setAlarmText (const QString &text) {
	alarmText = text;
	if (alarmMessageLabel != nullptr)
	   alarmMessageLabel->setText (text);
}

void	EwfMonitor::hideAlarmDialog () {
	if (alarmDialog != nullptr) {
	   alarmDialog->close ();
	   delete alarmDialog;
	   alarmDialog = nullptr;
	}
}
