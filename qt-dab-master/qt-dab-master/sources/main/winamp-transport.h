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
#include	<QPushButton>
#include	<QHBoxLayout>

class WinampTransportBar : public QWidget {
Q_OBJECT
public:
		WinampTransportBar	(QWidget *parent = nullptr);
		~WinampTransportBar	();

	QPushButton	*prevButton;
	QPushButton	*playButton;
	QPushButton	*stopButton;
	QPushButton	*nextButton;
	QPushButton	*ejectButton;

	// feature buttons
	QPushButton	*epgButton;
	QPushButton	*ewfButton;
	QPushButton	*timerButton;
	QPushButton	*recButton;

signals:
	void	prevClicked	();
	void	playClicked	();
	void	stopClicked	();
	void	nextClicked	();
	void	ejectClicked	();
	void	epgClicked	();
	void	ewfClicked	();
	void	timerClicked	();
	void	recClicked	();

private:
	void	setupUi		();
};
