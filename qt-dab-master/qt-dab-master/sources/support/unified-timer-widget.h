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
#include	<QTableView>
#include	<QPushButton>
#include	<QVBoxLayout>
#include	<QHeaderView>
#include	<QLabel>
#include	<QComboBox>
#include	<QDateTimeEdit>
#include	<QSpinBox>
#include	<QGroupBox>

class	UnifiedTimerModel;
class	QSettings;

class UnifiedTimerWidget : public superFrame {
Q_OBJECT
public:
		UnifiedTimerWidget	(UnifiedTimerModel *model,
		                         QSettings *settings,
		                         QWidget *parent = nullptr);
		~UnifiedTimerWidget	();

	void	setServiceList		(const QStringList &services);

private slots:
	void	handleAddTimer		();
	void	handleClearAll		();
	void	handleDoubleClick	(const QModelIndex &index);
	void	updateEndTime		();

private:
	void	setupUi		();

	UnifiedTimerModel	*theModel;
	QSettings		*theSettings;
	QTableView		*timerTableView;

	// add-timer form
	QComboBox		*typeSelector;
	QComboBox		*serviceCombo;
	QDateTimeEdit		*startTimeEdit;
	QSpinBox		*durationSpin;
	QLabel			*endTimeLabel;
	QPushButton		*addButton;
	QPushButton		*clearButton;
};
