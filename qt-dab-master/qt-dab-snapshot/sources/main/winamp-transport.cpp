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

#include	"winamp-transport.h"

	WinampTransportBar::WinampTransportBar (QWidget *parent):
	                                        QWidget (parent) {
	setupUi ();
}

	WinampTransportBar::~WinampTransportBar () {
}

void	WinampTransportBar::setupUi () {
QHBoxLayout *mainLayout = new QHBoxLayout (this);
	mainLayout->setContentsMargins (2, 2, 2, 2);
	mainLayout->setSpacing (1);

	// transport buttons
	prevButton = new QPushButton ("|<", this);
	prevButton->setObjectName ("btnPrev");
	prevButton->setToolTip ("Previous Service");
	connect (prevButton, &QPushButton::clicked,
	         this, &WinampTransportBar::prevClicked);
	mainLayout->addWidget (prevButton);

	playButton = new QPushButton (">", this);
	playButton->setObjectName ("btnPlay");
	playButton->setToolTip ("Play / Unmute");
	connect (playButton, &QPushButton::clicked,
	         this, &WinampTransportBar::playClicked);
	mainLayout->addWidget (playButton);

	stopButton = new QPushButton ("[]", this);
	stopButton->setObjectName ("btnStop");
	stopButton->setToolTip ("Stop / Mute");
	connect (stopButton, &QPushButton::clicked,
	         this, &WinampTransportBar::stopClicked);
	mainLayout->addWidget (stopButton);

	nextButton = new QPushButton (">|", this);
	nextButton->setObjectName ("btnNext");
	nextButton->setToolTip ("Next Service");
	connect (nextButton, &QPushButton::clicked,
	         this, &WinampTransportBar::nextClicked);
	mainLayout->addWidget (nextButton);

	ejectButton = new QPushButton ("^", this);
	ejectButton->setObjectName ("btnEject");
	ejectButton->setToolTip ("Channel Select");
	connect (ejectButton, &QPushButton::clicked,
	         this, &WinampTransportBar::ejectClicked);
	mainLayout->addWidget (ejectButton);

	// separator
	mainLayout->addSpacing (8);

	// feature buttons
	epgButton = new QPushButton ("EPG", this);
	epgButton->setObjectName ("btnEpg");
	epgButton->setToolTip ("Electronic Program Guide");
	connect (epgButton, &QPushButton::clicked,
	         this, &WinampTransportBar::epgClicked);
	mainLayout->addWidget (epgButton);

	ewfButton = new QPushButton ("EWF", this);
	ewfButton->setObjectName ("btnEwf");
	ewfButton->setToolTip ("Emergency Warning");
	connect (ewfButton, &QPushButton::clicked,
	         this, &WinampTransportBar::ewfClicked);
	mainLayout->addWidget (ewfButton);

	timerButton = new QPushButton ("TMR", this);
	timerButton->setObjectName ("btnTimer");
	timerButton->setToolTip ("Timer Manager");
	connect (timerButton, &QPushButton::clicked,
	         this, &WinampTransportBar::timerClicked);
	mainLayout->addWidget (timerButton);

	recButton = new QPushButton ("REC", this);
	recButton->setObjectName ("btnRec");
	recButton->setToolTip ("Record");
	connect (recButton, &QPushButton::clicked,
	         this, &WinampTransportBar::recClicked);
	mainLayout->addWidget (recButton);
}
