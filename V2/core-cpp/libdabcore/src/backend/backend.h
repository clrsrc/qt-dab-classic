// DAB Classic v3: portiert aus Qt-DAB sources/backend/backend.h
// (Jan van Katwijk, GPLv2+). Immer "threaded" (v1 __THREADED_BACKEND__):
// QThread -> std::thread, QSemaphore freeSlots/usedSlots -> Mutex +
// Condition-Variable mit Zaehler, QString -> std::string; RadioInterface,
// logger, RingBuffer und FILE-Dump -> BackendCallbacks.
// De-Interleaving, Deconvolver und Energy-Dispersal unveraendert.
#
/*
 *    Copyright (C) 2016 .. 2025
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *    Lazy Chair Computing
 *
 *    This file is part of the Qt-DAB.
 *
 *    Qt-DAB is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    Qt-DAB is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with Qt-DAB; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#
#pragma once

#include	<vector>
#include	<thread>
#include	<atomic>
#include	<mutex>
#include	<condition_variable>
#include	<string>
#include	<cstdio>
#include        "backend-driver.h"
#include        "backend-deconvolver.h"
#include	"backend-callbacks.h"
#include	"backend-frame-tap.h"
#include	"aac-decoder.h"

#define	NUMBER_SLOTS	25

class	Backend {
public:
		Backend	(const descriptorType	*d,
	                 BackendCallbacks	*cb,
	                 int			flag,
	                 uint8_t		cpuSupport,
	                 AacDecoderKind		aacKind);
		~Backend();
	int32_t	process		(int16_t *, int16_t);
	void	stopRunning	();
	bool	is_dataBackend	();
//	Timeshift (Plan M4 1.1): mit gesetztem Tap gehen die Hardbit-Rahmen
//	nicht direkt an den Driver, sondern an den Tap, der sie ueber
//	deliverFrame zurueckgibt. Setzen/Loeschen aus dem Kommandothread.
	void	setFrameTap	(IFrameTap *tap);
	void	deliverFrame	(const std::vector<uint8_t> &hardBits);
//	we need sometimes to access the key parameters for decoding
	int		backendType;
	int		serviceId;
	int		startAddr;
	int		Length;
	bool		shortForm;
	int		protLevel;
	int16_t		bitRate;
	int16_t		subChId;
	std::string	serviceName;
	int		borf;

private:
	backendDeconvolver	deconvolver;
	std::vector<uint8_t>	hardBits;
	backendDriver		driver;
	std::atomic<IFrameTap *>	frameTap;
	void	run();
	std::atomic<bool>	running;
	std::thread		theThread;
//	Ring mit NUMBER_SLOTS Segmenten (v1: QSemaphore freeSlots/usedSlots)
	std::mutex		slotM;
	std::condition_variable	slotCv;
	int			usedSlots;
	std::vector<int16_t>	theData [NUMBER_SLOTS];
	int16_t		nextIn;
	int16_t		nextOut;
	void		processSegment	(int16_t *Data);

	int16_t		fragmentSize;
	std::vector<std::vector <int16_t>> interleaveData;
	std::vector<int16_t> tempX;
	int16_t		countforInterleaver;
	int16_t		interleaverIndex;
	std::vector<uint8_t> disperseVector;
};
