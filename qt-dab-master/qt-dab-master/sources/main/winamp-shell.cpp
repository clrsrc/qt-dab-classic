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

#include	"winamp-shell.h"
#include	"winamp-transport.h"
#include	"radio.h"
#include	"dock-manager.h"
#include	"unified-timer-model.h"
#include	"unified-timer-widget.h"
#include	"epg-browser.h"
#include	"ewf-monitor.h"
#include	"recording-manager.h"
#include	"settingNames.h"
#include	"findfilenames.h"
#include	<QListWidget>
#include	<QEvent>
#include	<QMoveEvent>
#include	<QCloseEvent>
#include	<QMouseEvent>
#include	<QDateTime>
#include	<QApplication>
#ifdef Q_OS_WIN
#include	<dwmapi.h>
#endif

	WinampShell::WinampShell (RadioInterface *radio,
	                           QSettings *settings,
	                           QWidget *parent):
	                           QWidget (parent),
	                           theRadio (radio),
	                           theSettings (settings) {
	setWindowTitle ("Qt-DAB Winamp Edition");
	setObjectName ("winampMain");
	setFixedWidth (275);
	setWindowFlags (Qt::Window | Qt::FramelessWindowHint);
	dragging = false;

	fprintf (stderr, "WinampShell: constructor start\n");

	setupUi ();
	fprintf (stderr, "WinampShell: UI setup done\n");
	connectRadio ();
	fprintf (stderr, "WinampShell: radio connected\n");

	// populate channel selector from RadioInterface
	if (theRadio->channelSelector != nullptr) {
	   channelSelector->blockSignals (true);
	   for (int i = 0; i < theRadio->channelSelector->count (); i++)
	      channelSelector->addItem (theRadio->channelSelector->itemText (i));
	   QString currentChannel = theRadio->channelSelector->currentText ();
	   int idx = channelSelector->findText (currentChannel);
	   if (idx >= 0)
	      channelSelector->setCurrentIndex (idx);
	   channelSelector->blockSignals (false);
	   connect (channelSelector, &QComboBox::textActivated,
	            theRadio, &RadioInterface::handle_channelSelector);
	   fprintf (stderr, "WinampShell: channels populated (%d)\n",
	            channelSelector->count ());
	}

	// create managers
	theTimerModel = new UnifiedTimerModel (theSettings, this);
	theRecordingManager = new RecordingManager (theRadio, theSettings, this);
	connect (theRecordingManager, &RecordingManager::recordingStarted,
	         this, [this] (const QString &) {
	            transportBar->recButton->setStyleSheet ("color: red; font-weight: bold;");
	         });
	connect (theRecordingManager, &RecordingManager::recordingStopped,
	         this, [this] (const QString &) {
	            transportBar->recButton->setStyleSheet ("");
	         });
	theEwfMonitor = new EwfMonitor (theSettings, this, this);
	theEwfMonitor->setIndicatorLabel (ewfIndicator);

	// create dock manager
	theDockManager = new DockManager (this, theSettings, this);

	// enable dark title bars on Windows 10/11
#ifdef Q_OS_WIN
	auto setDarkTitleBar = [] (QWidget *w) {
	   BOOL useDark = TRUE;
	   DwmSetWindowAttribute ((HWND)w->winId (),
	      20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDark, sizeof (useDark));
	};
	setDarkTitleBar (this);
#endif

	// create sub-windows (Tool windows - small title bar, near main)
	theTimerWidget = new UnifiedTimerWidget (theTimerModel, theSettings);
	theTimerWidget->setWindowFlags (Qt::Tool | Qt::WindowStaysOnTopHint);
	theTimerWidget->setWindowTitle ("Timer");
	theTimerWidget->hide ();
#ifdef Q_OS_WIN
	setDarkTitleBar (theTimerWidget);
#endif
	theDockManager->registerPanel (theTimerWidget, "timerPanel",
	                               DockPosition::Right);

	findfileNames fileFinder (theSettings);
	QString epgPath = theSettings->value (S_FILE_PATH,
	                     fileFinder.basicPath ()).toString ();
	theEpgBrowser = new EpgBrowser (epgPath, theTimerModel, theSettings);
	theEpgBrowser->setWindowFlags (Qt::Tool | Qt::WindowStaysOnTopHint);
	theEpgBrowser->setWindowTitle ("EPG Browser");
	theEpgBrowser->hide ();
#ifdef Q_OS_WIN
	setDarkTitleBar (theEpgBrowser);
#endif
	theDockManager->registerPanel (theEpgBrowser, "epgPanel",
	                               DockPosition::Right);

	// connect timer signals
	connect (theTimerModel, &UnifiedTimerModel::switchTimerFired,
	         this, [this] (const QString &service, const QString &channel) {
	            theRadio->scheduleSelect (channel + ":" + service);
	         });
	connect (theTimerModel, &UnifiedTimerModel::recordTimerFired,
	         this, [this] (const QString &service, const QString &channel,
	                        int duration, const QString &title) {
	            // switch to service if needed, then start named recording
	            if (!channel.isEmpty ())
	               theRadio->scheduleSelect (channel + ":" + service);
	            QTimer::singleShot (3000, this,
	               [this, service, title, duration] () {
	                  theRecordingManager->startNamedRecording (service, title);
	                  if (duration > 0)
	                     theRecordingManager->startTimedRecording (duration);
	               });
	         });

	// connect EPG switch
	connect (theEpgBrowser, &EpgBrowser::switchRequested,
	         this, [this] (const QString &service, const QString &channel) {
	            theRadio->scheduleSelect (channel + ":" + service);
	         });

	// connect EPG immediate recording (no dialog)
	connect (theEpgBrowser, &EpgBrowser::recordRequested,
	         this, [this] (const QString &service, const QString &title,
	                        int duration) {
	            theRecordingManager->startNamedRecording (service, title);
	            if (duration > 0)
	               theRecordingManager->startTimedRecording (duration);
	         });

	// create playlist panel (service list)
	playlistPanel = new QWidget (nullptr);
	playlistPanel->setWindowTitle ("Playlist");
	playlistPanel->setObjectName ("winampMain");
	playlistPanel->setFixedWidth (275);
	QVBoxLayout *plLayout = new QVBoxLayout (playlistPanel);
	plLayout->setContentsMargins (2, 2, 2, 2);
	plLayout->setSpacing (0);

	serviceListWidget = new QListWidget (playlistPanel);
	serviceListWidget->setStyleSheet (
	   "QListWidget { background: #0a0e14; color: #00c040; "
	   "border: none; font-size: 10px; } "
	   "QListWidget::item { padding: 1px 4px; } "
	   "QListWidget::item:selected { background: #002a10; color: #00ff50; } "
	   "QListWidget::item:hover { background: #0e1a0e; }");
	serviceListWidget->setMinimumHeight (150);
	connect (serviceListWidget, &QListWidget::itemClicked,
	         this, [this] (QListWidgetItem *item) {
	            serviceClicked (item->text ());
	         });
	plLayout->addWidget (serviceListWidget);
	playlistPanel->hide ();

	theDockManager->registerPanel (playlistPanel, "playlist",
	                               DockPosition::Below);

	// connect service list signals from RadioInterface
	connect (theRadio, &RadioInterface::addToEnsemble,
	         this, &WinampShell::addServiceToList);
	connect (theRadio, &RadioInterface::changeinConfiguration,
	         this, &WinampShell::clearServiceList);

	// display refresh - update time, date, sync, service info
	displayRefreshTimer.setInterval (1000);
	connect (&displayRefreshTimer, &QTimer::timeout,
	         this, &WinampShell::updateTimeDisplay);
	displayRefreshTimer.start ();

	// connect sync indicator
	connect (theRadio, &RadioInterface::set_synced,
	         this, &WinampShell::updateSyncState);

	// try import old scheduler
	QString schedFile = theSettings->value ("schedFile", "").toString ();
	if (!schedFile.isEmpty ())
	   theTimerModel->importOldScheduler (schedFile);

	// restore dock layout
	theDockManager->restoreLayout ();
}

	WinampShell::~WinampShell () {
	theDockManager->saveLayout ();
}

DockManager *WinampShell::getDockManager () {
	return theDockManager;
}

void	WinampShell::setupUi () {
QVBoxLayout *mainLayout = new QVBoxLayout (this);
	mainLayout->setContentsMargins (2, 2, 2, 2);
	mainLayout->setSpacing (1);

	// === Title Bar ===
	titleBar = new QWidget (this);
	titleBar->setObjectName ("titleBar");
	titleBar->setFixedHeight (14);
	QHBoxLayout *titleLayout = new QHBoxLayout (titleBar);
	titleLayout->setContentsMargins (4, 0, 2, 0);
	titleLayout->setSpacing (2);
	titleLabel = new QLabel ("Qt-DAB Winamp", titleBar);
	titleLabel->setObjectName ("titleLabel");
	titleLayout->addWidget (titleLabel);
	titleLayout->addStretch ();

	QPushButton *minimizeBtn = new QPushButton ("_", titleBar);
	minimizeBtn->setFixedSize (12, 12);
	minimizeBtn->setStyleSheet (
	   "QPushButton { background: #3A3A3A; border: 1px outset #555; "
	   "color: #CCC; font-size: 8px; padding: 0; }");
	minimizeBtn->setCursor (Qt::ArrowCursor);
	connect (minimizeBtn, &QPushButton::clicked,
	         this, &QWidget::showMinimized);
	titleLayout->addWidget (minimizeBtn);

	QPushButton *closeBtn = new QPushButton ("X", titleBar);
	closeBtn->setFixedSize (12, 12);
	closeBtn->setStyleSheet (
	   "QPushButton { background: #3A3A3A; border: 1px outset #555; "
	   "color: #CC0000; font-size: 8px; padding: 0; font-weight: bold; }");
	closeBtn->setCursor (Qt::ArrowCursor);
	connect (closeBtn, &QPushButton::clicked,
	         this, &QWidget::close);
	titleLayout->addWidget (closeBtn);

	titleBar->installEventFilter (this);
	titleLabel->installEventFilter (this);
	titleBar->setCursor (Qt::SizeAllCursor);
	mainLayout->addWidget (titleBar);

	// === Display Area ===
	displayArea = new QWidget (this);
	displayArea->setObjectName ("displayArea");
	displayArea->setMinimumHeight (40);
	QGridLayout *displayLayout = new QGridLayout (displayArea);
	displayLayout->setContentsMargins (4, 2, 4, 2);

	bitrateLabel = new QLabel ("--- kbps", displayArea);
	bitrateLabel->setObjectName ("bitrateLabel");
	bitrateLabel->installEventFilter (this);
	displayLayout->addWidget (bitrateLabel, 0, 0);

	freqLabel = new QLabel ("--- MHz", displayArea);
	freqLabel->setObjectName ("freqLabel");
	freqLabel->installEventFilter (this);
	displayLayout->addWidget (freqLabel, 0, 1);

	stereoIndicator = new QLabel ("mono", displayArea);
	stereoIndicator->setObjectName ("stereoIndicator");
	stereoIndicator->installEventFilter (this);
	displayLayout->addWidget (stereoIndicator, 0, 2);

	displayArea->installEventFilter (this);
	displayArea->setCursor (Qt::SizeAllCursor);
	mainLayout->addWidget (displayArea);

	// === Service Name ===
	serviceNameLabel = new QLabel ("No Service", this);
	serviceNameLabel->setObjectName ("serviceLabel");
	serviceNameLabel->setAlignment (Qt::AlignCenter);
	serviceNameLabel->installEventFilter (this);
	serviceNameLabel->setCursor (Qt::SizeAllCursor);
	mainLayout->addWidget (serviceNameLabel);

	// === Ticker (DLS) ===
	tickerLabel = new QLabel ("", this);
	tickerLabel->setObjectName ("tickerLabel");
	tickerLabel->setAlignment (Qt::AlignLeft | Qt::AlignVCenter);
	tickerLabel->setMinimumHeight (18);
	mainLayout->addWidget (tickerLabel);

	// === Transport Bar ===
	transportBar = new WinampTransportBar (this);
	mainLayout->addWidget (transportBar);

	// === Volume + Channel ===
	QHBoxLayout *volLayout = new QHBoxLayout ();
	volLayout->setContentsMargins (0, 0, 0, 0);

	QLabel *volLabel = new QLabel ("VOL", this);
	volLabel->setFixedWidth (24);
	volLayout->addWidget (volLabel);

	volumeSlider = new QSlider (Qt::Horizontal, this);
	volumeSlider->setObjectName ("volumeSlider");
	volumeSlider->setRange (0, 100);
	volumeSlider->setValue (70);
	volLayout->addWidget (volumeSlider);

	channelSelector = new QComboBox (this);
	channelSelector->setMinimumWidth (60);
	volLayout->addWidget (channelSelector);

	mainLayout->addLayout (volLayout);

	// === Menu Buttons Row 1: controls, spectrum, http, scan ===
	QHBoxLayout *menuRow1 = new QHBoxLayout ();
	menuRow1->setContentsMargins (0, 0, 0, 0);
	menuRow1->setSpacing (2);

	QPushButton *controlsBtn = new QPushButton ("controls", this);
	connect (controlsBtn, &QPushButton::clicked,
	         theRadio, &RadioInterface::handle_configButton);
	menuRow1->addWidget (controlsBtn);

	QPushButton *spectrumBtn = new QPushButton ("spectrum", this);
	connect (spectrumBtn, &QPushButton::clicked,
	         theRadio, &RadioInterface::handle_spectrumButton);
	menuRow1->addWidget (spectrumBtn);

	QPushButton *httpBtn = new QPushButton ("http", this);
	connect (httpBtn, &QPushButton::clicked,
	         theRadio, &RadioInterface::handle_httpButton);
	menuRow1->addWidget (httpBtn);

	QPushButton *scanBtn = new QPushButton ("scan", this);
	connect (scanBtn, &QPushButton::clicked,
	         theRadio, &RadioInterface::handle_scanButton);
	menuRow1->addWidget (scanBtn);

	mainLayout->addLayout (menuRow1);

	// === Menu Buttons Row 2: scanlist, favorites, tech, device ===
	QHBoxLayout *menuRow2 = new QHBoxLayout ();
	menuRow2->setContentsMargins (0, 0, 0, 0);
	menuRow2->setSpacing (2);

	QPushButton *scanlistBtn = new QPushButton ("scanlist", this);
	connect (scanlistBtn, &QPushButton::clicked,
	         theRadio, &RadioInterface::handle_scanListButton);
	menuRow2->addWidget (scanlistBtn);

	QPushButton *favBtn = new QPushButton ("playlist", this);
	connect (favBtn, &QPushButton::clicked,
	         this, &WinampShell::handlePlaylistToggle);
	menuRow2->addWidget (favBtn);

	QPushButton *techBtn = new QPushButton ("tech", this);
	connect (techBtn, &QPushButton::clicked,
	         theRadio, [this] () { theRadio->handle_detailButton (); });
	menuRow2->addWidget (techBtn);

	QPushButton *deviceBtn = new QPushButton ("device", this);
	connect (deviceBtn, &QPushButton::clicked,
	         theRadio, &RadioInterface::handle_devicewidgetButton);
	menuRow2->addWidget (deviceBtn);

	mainLayout->addLayout (menuRow2);

	// === Status Bar ===
	statusBar = new QWidget (this);
	statusBar->setObjectName ("statusBar");
	QHBoxLayout *statusLayout = new QHBoxLayout (statusBar);
	statusLayout->setContentsMargins (4, 0, 4, 0);
	statusLayout->setSpacing (8);

	ewfIndicator = new QLabel ("EWF", statusBar);
	ewfIndicator->setObjectName ("ewfIndicator");
	statusLayout->addWidget (ewfIndicator);

	syncIndicator = new QLabel ("SYNC", statusBar);
	syncIndicator->setObjectName ("syncIndicator");
	statusLayout->addWidget (syncIndicator);

	statusLayout->addStretch ();

	timeDisplay = new QLabel ("--:--", statusBar);
	timeDisplay->setObjectName ("timeDisplay");
	statusLayout->addWidget (timeDisplay);

	mainLayout->addWidget (statusBar);
}

void	WinampShell::connectRadio () {
	// transport buttons
	connect (transportBar, &WinampTransportBar::prevClicked,
	         this, &WinampShell::handlePrevService);
	connect (transportBar, &WinampTransportBar::playClicked,
	         this, &WinampShell::handleMuteToggle);
	connect (transportBar, &WinampTransportBar::stopClicked,
	         this, &WinampShell::handleStopChannel);
	connect (transportBar, &WinampTransportBar::nextClicked,
	         this, &WinampShell::handleNextService);
	connect (transportBar, &WinampTransportBar::ejectClicked,
	         this, &WinampShell::handleEjectChannel);

	// feature buttons
	connect (transportBar, &WinampTransportBar::epgClicked,
	         this, &WinampShell::handleEpgToggle);
	connect (transportBar, &WinampTransportBar::timerClicked,
	         this, &WinampShell::handleTimerToggle);
	connect (transportBar, &WinampTransportBar::recClicked,
	         this, &WinampShell::handleRecToggle);

	// volume
	connect (volumeSlider, &QSlider::valueChanged,
	         this, &WinampShell::updateVolume);

	// signals from RadioInterface
	connect (theRadio, &RadioInterface::dlsText,
	         this, &WinampShell::updateDynamicLabel);
	connect (theRadio, &RadioInterface::set_synced,
	         this, &WinampShell::updateSyncState);
}

bool	WinampShell::eventFilter (QObject *obj, QEvent *event) {
	// drag window by title bar, display area, or labels
	if (obj == titleBar || obj == titleLabel ||
	    obj == displayArea || obj == serviceNameLabel ||
	    obj == bitrateLabel || obj == freqLabel || obj == stereoIndicator) {
	   if (event->type () == QEvent::MouseButtonPress) {
	      QMouseEvent *me = static_cast<QMouseEvent *>(event);
	      if (me->button () == Qt::LeftButton) {
	         dragPosition = me->globalPosition ().toPoint ()
	                        - frameGeometry ().topLeft ();
	         dragging = true;
	         return true;
	      }
	   }
	   else if (event->type () == QEvent::MouseMove) {
	      QMouseEvent *me = static_cast<QMouseEvent *>(event);
	      if (dragging && (me->buttons () & Qt::LeftButton)) {
	         move (me->globalPosition ().toPoint () - dragPosition);
	         return true;
	      }
	   }
	   else if (event->type () == QEvent::MouseButtonRelease) {
	      dragging = false;
	   }
	}
	return QWidget::eventFilter (obj, event);
}

void	WinampShell::mousePressEvent (QMouseEvent *event) {
	QWidget::mousePressEvent (event);
}

void	WinampShell::mouseMoveEvent (QMouseEvent *event) {
	QWidget::mouseMoveEvent (event);
}

void	WinampShell::moveEvent (QMoveEvent *event) {
	QWidget::moveEvent (event);
	if (theDockManager != nullptr)
	   theDockManager->updatePositions ();
}

void	WinampShell::closeEvent (QCloseEvent *event) {
	theDockManager->saveLayout ();
	theRadio->TerminateProcess ();
	event->accept ();
	QApplication::quit ();
}

// === Slot implementations ===

void	WinampShell::handlePrevService () {
	theRadio->handle_prevServiceButton ();
}

void	WinampShell::handleNextService () {
	theRadio->handle_nextServiceButton ();
}

void	WinampShell::handleMuteToggle () {
	theRadio->handle_muteButton ();
}

void	WinampShell::handleStopChannel () {
	theRadio->handle_muteButton ();
}

void	WinampShell::handleEjectChannel () {
	// show/hide the channel selector popup or config
	theRadio->handle_configButton ();
}

void	WinampShell::handleEpgToggle () {
	if (theEpgBrowser->isVisible ()) {
	   theEpgBrowser->hide ();
	}
	else {
	   // set up EPG browser with current service data
	   if (theRadio->channel.currentService.isValid) {
	      QDate today = QDate::currentDate ();
	      uint32_t eid = theRadio->channel.Eid;
	      uint16_t sid = theRadio->channel.currentService.SId;
	      QString svcName = theRadio->channel.currentService.serviceName;
	      QString chName = theRadio->channel.channelName;
	      theEpgBrowser->setUp (today, eid, sid, svcName, chName);
	      fprintf (stderr, "EPG: setUp for %s (Eid=%X, Sid=%X)\n",
	               svcName.toUtf8 ().data (), eid, sid);
	   }
	   theEpgBrowser->show ();
	   theDockManager->updatePositions ();
	}
}

void	WinampShell::handleTimerToggle () {
	if (theTimerWidget->isVisible ())
	   theTimerWidget->hide ();
	else {
	   theTimerWidget->show ();
	   theDockManager->updatePositions ();
	}
}

void	WinampShell::handleRecToggle () {
	if (theRecordingManager->isRecording ())
	   theRecordingManager->stopManualRecording ();
	else
	   theRecordingManager->startManualRecording ();
}

void	WinampShell::updateServiceName (const QString &name) {
	serviceNameLabel->setText (name);
	titleLabel->setText ("Qt-DAB - " + name);
}

void	WinampShell::updateDynamicLabel (const QString &text, int) {
	tickerLabel->setText (text);
}

void	WinampShell::updateSyncState (bool synced) {
	syncIndicator->setProperty ("synced", synced);
	syncIndicator->setStyleSheet (synced ?
	   "QLabel { color: #00FF00; }" :
	   "QLabel { color: #666666; }");
}

void	WinampShell::updateTimeDisplay () {
	QDateTime now = QDateTime::currentDateTime ();
	timeDisplay->setText (now.toString ("dd.MM. HH:mm:ss"));

	// poll service info from RadioInterface
	if (theRadio->channel.currentService.isValid) {
	   QString svcName = theRadio->channel.currentService.serviceName;
	   if (serviceNameLabel->text () != svcName)
	      serviceNameLabel->setText (svcName);
	}

	// update channel/freq info
	if (theRadio->channel.tunedFrequency > 0) {
	   double freqMHz = theRadio->channel.tunedFrequency / 1000000.0;
	   freqLabel->setText (QString::number (freqMHz, 'f', 3) + " MHz");
	}

	// update bitrate from rateLabel
	QString rateText = theRadio->rateLabel->text ();
	if (!rateText.isEmpty () && rateText != "ratelabel")
	   bitrateLabel->setText (rateText);

	// update stereo
	QString stereoText = theRadio->stereoLabel->text ();
	if (!stereoText.isEmpty () && stereoText != "stereo")
	   stereoIndicator->setText (stereoText);
}

void	WinampShell::updateVolume (int value) {
	theRadio->setVolume (value);
}

void	WinampShell::handlePlaylistToggle () {
	if (playlistPanel->isVisible ()) {
	   playlistPanel->hide ();
	}
	else {
	   playlistPanel->show ();
	   theDockManager->updatePositions ();
	}
}

void	WinampShell::addServiceToList (const QString &name, int, int) {
	// avoid duplicates
	for (int i = 0; i < serviceListWidget->count (); i++) {
	   if (serviceListWidget->item (i)->text () == name)
	      return;
	}
	serviceListWidget->addItem (name);

	// update timer widget service list
	QStringList services;
	for (int i = 0; i < serviceListWidget->count (); i++)
	   services.append (serviceListWidget->item (i)->text ());
	theTimerWidget->setServiceList (services);
}

void	WinampShell::clearServiceList () {
	serviceListWidget->clear ();
}

void	WinampShell::serviceClicked (const QString &name) {
	if (theRecordingManager->isRecording ()) {
	   tickerLabel->setText ("Aufnahme läuft – Senderwechsel gesperrt");
	   return;
	}
	// use the existing localSelect mechanism
	QString channel = theRadio->channel.channelName;
	theRadio->localSelect (channel, name);
}
