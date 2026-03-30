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

#include	<QAbstractTableModel>
#include	<QDateTime>
#include	<QString>
#include	<QTimer>
#include	<QJsonArray>
#include	<QSettings>
#include	<vector>
#include	<cstdint>

enum class TimerType : uint8_t {
	ManualSwitch	= 0,
	ManualRecord	= 1,
	EpgSwitch	= 2,
	EpgRecord	= 3
};

struct TimerEntry {
	int		id;
	TimerType	type;
	QString		serviceName;
	QString		channelName;
	QDateTime	startTime;
	int		durationMinutes;	// 0 for switch-only
	bool		active;
	bool		fired;			// true after timer has been triggered
	QString		programTitle;		// from EPG, empty for manual
};

class UnifiedTimerModel : public QAbstractTableModel {
Q_OBJECT
public:
	enum Column {
	   COL_TYPE	= 0,
	   COL_SERVICE	= 1,
	   COL_DATETIME	= 2,
	   COL_DURATION	= 3,
	   COL_TITLE	= 4,
	   COL_STATUS	= 5,
	   COL_DELETE	= 6,
	   COL_COUNT	= 7
	};

		UnifiedTimerModel	(QSettings *, QObject *parent = nullptr);
		~UnifiedTimerModel	();

	int	rowCount	(const QModelIndex &parent = QModelIndex()) const override;
	int	columnCount	(const QModelIndex &parent = QModelIndex()) const override;
	QVariant data		(const QModelIndex &index,
	                         int role = Qt::DisplayRole) const override;
	QVariant headerData	(int section, Qt::Orientation orientation,
	                         int role = Qt::DisplayRole) const override;

	int	addTimer	(TimerType type,
	                         const QString &serviceName,
	                         const QString &channelName,
	                         const QDateTime &startTime,
	                         int durationMinutes = 0,
	                         const QString &programTitle = "");
	int	findConflict	(const QDateTime &startTime,
	                         int durationMinutes) const;
	void	markFired	(int id);
	void	removeTimer	(int row);
	void	removeTimerById	(int id);
	void	clearAll	();
	int	timerCount	() const;
	const TimerEntry &timerAt (int row) const;

	void	saveToFile	();
	void	loadFromFile	();
	void	importOldScheduler (const QString &schedulerFile);

	static	QString	timerTypeToString	(TimerType t);
	static	QString	timerTypeToIcon		(TimerType t);

signals:
	void	switchTimerFired	(const QString &serviceName,
	                                 const QString &channelName);
	void	recordTimerFired	(const QString &serviceName,
	                                 const QString &channelName,
	                                 int durationMinutes,
	                                 const QString &title);

private slots:
	void	checkTimers	();

private:
	void	sortByStartTime	();
	int	nextId		();

	QSettings		*settings;
	std::vector<TimerEntry>	timers;
	QTimer			pollTimer;
	int			idCounter;
	QString			timerFilePath;
};
