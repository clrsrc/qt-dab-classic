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

#include	<QWidget>
#include	<QLabel>
#include	<QSlider>
#include	<QComboBox>
#include	<QSettings>
#include	<QVBoxLayout>
#include	<QHBoxLayout>
#include	<QTimer>
#include	<QScrollArea>

class	RadioInterface;
class	WinampTransportBar;
class	DockManager;
class	UnifiedTimerModel;
class	UnifiedTimerWidget;
class	EpgBrowser;
class	EwfMonitor;
class	RecordingManager;
class	QListWidget;

class WinampShell : public QWidget {
Q_OBJECT
public:
		WinampShell	(RadioInterface *radio,
		                 QSettings *settings,
		                 QWidget *parent = nullptr);
		~WinampShell	();

	DockManager		*getDockManager	();

protected:
	bool	eventFilter	(QObject *obj, QEvent *event) override;
	void	moveEvent	(QMoveEvent *event) override;
	void	closeEvent	(QCloseEvent *event) override;
	void	mousePressEvent	(QMouseEvent *event) override;
	void	mouseMoveEvent	(QMouseEvent *event) override;

private slots:
	void	handlePrevService	();
	void	handleNextService	();
	void	handleMuteToggle	();
	void	handleStopChannel	();
	void	handleEjectChannel	();
	void	handleEpgToggle		();
	void	handleTimerToggle	();
	void	handleRecToggle		();
	void	handlePlaylistToggle	();

	// service list updates
	void	addServiceToList	(const QString &name, int, int);
	void	serviceClicked		(const QString &name);
	void	clearServiceList	();

	// updates from RadioInterface
	void	updateServiceName	(const QString &name);
	void	updateDynamicLabel	(const QString &text, int);
	void	updateSyncState		(bool synced);
	void	updateTimeDisplay	();
	void	updateVolume		(int value);

private:
	void	setupUi		();
	void	connectRadio	();
	void	loadServiceLogo	();

	RadioInterface		*theRadio;
	QSettings		*theSettings;

	// UI components
	QWidget			*titleBar;
	QLabel			*titleLabel;

	QWidget			*displayArea;
	QLabel			*bitrateLabel;
	QLabel			*freqLabel;
	QLabel			*stereoIndicator;

	QLabel			*serviceNameLabel;
	QWidget			*mediaWidget;
	QLabel			*logoLabel;
	QLabel			*slideLabel;
	QLabel			*tickerLabel;

	WinampTransportBar	*transportBar;

	QSlider			*volumeSlider;
	QComboBox		*channelSelector;

	QWidget			*statusBar;
	QLabel			*ewfIndicator;
	QLabel			*syncIndicator;
	QLabel			*motIndicator;
	QLabel			*timeDisplay;

	QTimer			displayRefreshTimer;

	// sub-windows
	DockManager		*theDockManager;
	UnifiedTimerWidget	*theTimerWidget;
	EpgBrowser		*theEpgBrowser;

	// playlist panel
	QWidget			*playlistPanel;
	QListWidget		*serviceListWidget;

	// managers
	UnifiedTimerModel	*theTimerModel;
	EwfMonitor		*theEwfMonitor;
	RecordingManager	*theRecordingManager;

	// frameless window drag
	QPoint			dragPosition;
	bool			dragging;
};
