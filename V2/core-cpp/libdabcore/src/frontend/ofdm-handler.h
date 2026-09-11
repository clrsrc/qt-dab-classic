// DAB Classic v3: portiert aus Qt-DAB sources/frontend/ofdm-handler.h
// (Jan van Katwijk, GPLv2+), Qt entfernt: QThread -> std::thread,
// QSettings -> processParams (v1-Defaults), 11 Signale -> ReceiverCallbacks,
// mscHandler -> IMscSink, etiGenerator und Scope-Puffer gestrichen.
// Die run()-Schleife (Sync, Frequenzkorrektur, FIC, SNR, TII) ist
// unveraendert; das Beenden laeuft wie in v1 ueber throw 20/21 aus
// sampleReader::getSamples, gefangen innerhalb des Threads.
#
/*
 *    Copyright (C) 2015 .. 2025
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *    Lazy Chair Computing
 *
 *    This file is part of Qt-DAB
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
/*
 *	the ofdmHandler is the embodying of all functionality related
 *	to the ofdm processing and preparation for further decoding
 */
#include	"dab-constants.h"
#include	<vector>
#include	<cstdint>
#include	<atomic>
#include	<thread>
#include	"sample-reader.h"
#include	"fic-handler.h"
#include	"msc-sink.h"
#include	"ofdm-decoder.h"
#include	"isample-source.h"
#include	"estimator.h"
#include	"dab-params.h"
#include	"process-params.h"
#include	"receiver-callbacks.h"

class ofdmHandler {
public:
		ofdmHandler  	(ISampleSource *,
	                         processParams *,
	                         IMscSink *,
	                         ReceiverCallbacks *,
	                         uint8_t cpuSupport);
		~ofdmHandler			();
	void		start			();
	void		stop			();
	bool		isRunning		() const { return threadRunning. load (); }

	void		setScanMode		(bool);
	void		getFrameQuality		(int *, int*, int *);
//
//	for the tii settings
	void		setTIIThreshold		(int16_t);
	void		setTIICollisions	(int);
//	servicing our subordinates: der FIC-Zustand fuer Abfragen
	ficHandler	&fic			() { return theFicHandler; }
	const ficHandler &fic			() const { return theFicHandler; }
	void		setCorrelationOrder	(bool);
	void		setDXMode		(bool);
	void		set_dcRemoval		(bool);
	void		handleDecoderSelector	(int);
private:
	processParams		*p;
	dabParams		params;
	IMscSink		*theMscSink;
	ReceiverCallbacks	*cb;
	uint8_t			cpuSupport;
	sampleReader		theReader;
	ficHandler		theFicHandler;
	ofdmDecoder		theOfdmDecoder;
	phaseTable		theTable;
	estimator		theEstimator;

	DABFLOAT		snr;
	int16_t			tiiThreshold;
	int			tiiCollision;

	int			decoder;
	int			thresHold;
	int			totalFrames;
	int			goodFrames;
	int			badFrames;
	float			rateError;
	int16_t			tiiDelay;
	int16_t			tiiCounter;
	int16_t			attempts;
	bool			scanMode;
	int32_t			T_null;
	int32_t			T_u;
	int32_t			T_s;
	int32_t			T_g;
	int32_t			T_F;
	int32_t			nrBlocks;
	int32_t			carriers;
	int32_t			carrierDiff;
	int16_t			fineOffset;
	int32_t			coarseOffset;
	bool			correctionNeeded;
	std::vector<Complex>	ofdmBuffer;
	bool			correlationOrder;
	bool			dxMode;
	std::thread		theThread;
	std::atomic<bool>	threadRunning;
	void			run		();
};
