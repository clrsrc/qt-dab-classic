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

#include	"super-frame.h"
#include	"xml-extractor.h"
#include	<QTableWidget>
#include	<QPushButton>
#include	<QComboBox>
#include	<QLabel>
#include	<QDate>
#include	<QPixmap>
#include	<QSettings>
#include	<QVBoxLayout>
#include	<QHBoxLayout>
#include	<vector>

class	UnifiedTimerModel;

class EpgBrowser : public superFrame {
Q_OBJECT
public:
		EpgBrowser	(const QString &epgFilePath,
		                 UnifiedTimerModel *timerModel,
		                 QSettings *settings,
		                 QWidget *parent = nullptr);
		~EpgBrowser	();

	void	setUp		(const QDate &date, uint32_t Eid,
	                         uint16_t SId, const QString &serviceName,
	                         const QString &channelName);
	void	addLogo		(const QPixmap &p);
	void	clear		();

signals:
	void	switchRequested	(const QString &serviceName,
	                         const QString &channelName);
	void	recordRequested	(const QString &serviceName,
	                         const QString &title,
	                         int durationMinutes);

private slots:
	void	handlePrevDay	();
	void	handleNextDay	();
	void	handleTimeRangeChanged (int index);
	void	handleSwitchClicked	();
	void	handleRecordClicked	();
	void	handleRefresh		();

private:
	void	setupUi		();
	void	loadAndDisplay	(int dateOffset);
	scheduleDescriptor loadSchedule (const QDate &date,
	                                 uint32_t Eid, uint32_t SId);
	void	displaySchedule	(const scheduleDescriptor &sched);
	QString	bestProgramName	(const programDescriptor &p);
	void	addProgramRow	(const programDescriptor &p, int row);

	UnifiedTimerModel	*theTimerModel;
	QSettings		*theSettings;
	xmlExtractor		xmlHandler;

	QString			path_for_files;
	QString			currentServiceName;
	QString			currentChannelName;
	uint32_t		currentEid;
	uint32_t		currentSid;
	QDate			startDate;
	int			dateOffset;

	// UI elements
	QLabel			*serviceLabel;
	QLabel			*serviceLogo;
	QLabel			*dateLabel;
	QPushButton		*prevButton;
	QPushButton		*nextButton;
	QComboBox		*timeRangeCombo;
	QTableWidget		*programTable;

	// cached schedule for button actions
	std::vector<programDescriptor>	displayedPrograms;
};
