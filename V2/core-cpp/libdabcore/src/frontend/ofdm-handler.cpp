// DAB Classic v3: portiert aus Qt-DAB sources/frontend/ofdm-handler.cpp
// (Jan van Katwijk, GPLv2+), Qt entfernt – siehe ofdm-handler.h.
#
/*
 *    Copyright (C) 2014 .. 2025
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
 *    along with Qt-DAB if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include	"ofdm-handler.h"

#include	<utility>
#include	<chrono>
#include	"dab-constants.h"
#include	"process-params.h"
#include	"dab-params.h"
#include	"timesyncer.h"
#include	"freqsyncer.h"
#include	"correlator.h"

#include	"tii-detector.h"

/**
  *	\brief ofdmHandler
  *	The ofdmHandler class is the driver of the processing
  *	of the samplestream.
  */

	ofdmHandler::ofdmHandler	(ISampleSource	*inputDevice,
	                                 processParams	*p,
	                                 IMscSink	*mscSink,
	                                 ReceiverCallbacks *callbacks,
	                                 uint8_t	cpuSupport):
	                                    params (p -> dabMode),
	                                    theReader (inputDevice),
	                                    theFicHandler (callbacks, p -> dabMode,
	                                                      cpuSupport),
	                                    theOfdmDecoder (p -> dabMode,
	                                                 inputDevice -> bitDepth()),
	                                    theTable (p -> dabMode),
	                                    theEstimator (p, &theTable) {
	this	-> p			= p;
	this	-> theMscSink		= mscSink;
	this	-> cb			= callbacks;
	this	-> cpuSupport		= cpuSupport;
	this	-> thresHold		= p -> threshold;
	this	-> T_null		= params. get_T_null ();
	this	-> T_s			= params. get_T_s ();
	this	-> T_u			= params. get_T_u ();
	this	-> T_g			= T_s - T_u;
	this	-> T_F			= params. get_T_F ();
	this	-> nrBlocks		= params. get_L ();
	this	-> carriers		= params. get_carriers ();
	this	-> carrierDiff		= params. get_carrierDiff ();

	this	-> tiiDelay		= p -> tii_delay;
	this	-> tiiCounter		= 0;
	this	-> correlationOrder	= p -> correlationOrder;
	this	-> dxMode		= p -> dxMode;
	this	-> decoder		= p -> decoder;

	ofdmBuffer. resize (3 * T_s);
	fineOffset			= 0;
	coarseOffset			= 0;
	correctionNeeded		= true;
	attempts			= 0;

	goodFrames			= 0;
	badFrames			= 0;
	totalFrames			= 0;
	scanMode			= false;
	threadRunning. store (false);

	tiiThreshold	= p -> tiiThreshold;
	tiiCollision	= p -> tiiCollision;
	theOfdmDecoder. handle_decoderSelector (decoder);
	theReader. set_dcRemoval (p -> dcRemoval);

	this	-> snr		= 10;	// until we know better
}

	ofdmHandler::~ofdmHandler () {
	stop ();
}

void	ofdmHandler::setTIIThreshold	(int16_t threshold) {
	tiiThreshold = threshold;
}

void	ofdmHandler::setTIICollisions	(int subId) {
	tiiCollision = subId;
}

void	ofdmHandler::start () {
	if (threadRunning. load ())
	   return;
	fineOffset			= 0;
	coarseOffset			= 0;
	attempts			= 0;
	rateError			= 0;
	goodFrames			= 0;
	badFrames			= 0;
	totalFrames			= 0;
	theOfdmDecoder. reset	();
	theFicHandler.  restart	();
	if (!scanMode)
	   theMscSink -> resetChannel ();
	theReader. setRunning (true);	// vor dem Thread, sonst Race mit stop()
	threadRunning. store (true);
	theThread = std::thread ([this] () { run (); });
}

void	ofdmHandler::stop	() {
	theReader. setRunning (false);
	if (theThread. joinable ())
	   theThread. join ();
	threadRunning. store (false);
	theFicHandler. stop ();
}
/***
   *	\brief run
   *	The main thread, reading samples,
   *	time synchronization and frequency synchronization
   *	Identifying blocks in the DAB frame
   *	and sending them to the ofdmDecoder who will transfer the results
   *	Finally, estimating the small frequency error
   */
void	ofdmHandler::run	() {
timeSyncer	myTimeSyncer (&theReader);
TII_Detector	theTIIDetector (p -> dabMode, &theTable);
freqSyncer	myFreqSyncer (p, &theTable);
correlator	myCorrelator (p, &theTable);
int32_t		startIndex	= -1;
std::vector<int16_t> softbits;
int	frameCount	= 0;
int	sampleCount	= 0;
int	totalSamples	= 0;
int	cCount		= 0;
bool	inSync		= false;
int	tryCounter	= 0;
int	snrCount	= 0;
bool	syncedReported	= false;	// setSynced nur bei Aenderung melden

	this	-> snr		= 10; 	// until really computed
	softbits. resize (2 * params. get_carriers());
	fineOffset		= 0;
	coarseOffset		= 0;
	correctionNeeded	= true;
	attempts		= 0;

//	v1 setzte hier theReader.setRunning (true). Das erzeugt ein Race mit
//	stop(): kommt stop() vor dieser Zeile, laeuft der Thread endlos.
//	V3: setRunning (true) steht in start(), vor dem Anlegen des Threads.
	auto setSynced = [&] (bool b) {
	   if (b != syncedReported) {
	      syncedReported = b;
	      emitCb (cb -> synced, b);
	   }
	};
//
//	to get some idea of the signal strength
	try {
	   const int tempSize = 128;
	   std::vector<Complex> temp (tempSize);
	   for (int i = 0; i < T_F / tempSize; i ++) {
	      theReader. getSamples (temp, 0, tempSize, 0, true);
	   }

	   while (true) {
	      if (!inSync) {
	         totalFrames ++;
	         totalSamples	= 0;
	         frameCount	= 0;
	         sampleCount	= 0;
	         setSynced (false);
	         theTIIDetector. reset ();
	         switch (myTimeSyncer. sync (T_null, T_F)) {
	            case TIMESYNC_ESTABLISHED:
	               inSync	= true;
	               setSynced (true);
	               break;			// yes, we are ready

	            case NO_DIP_FOUND:
	               if (++ attempts >= 8) {
	                  emitCb (cb -> noSignal);
	                  attempts = 0;
	               }
	               continue;

	            default:			// does not happen
	            case NO_END_OF_DIP_FOUND:
	               continue;
	         }

	         theReader. getSamples (ofdmBuffer, 0,
	                        T_u, coarseOffset + fineOffset, false);
	         startIndex = myCorrelator. findIndex (ofdmBuffer,
	                                               correlationOrder,
	                                               thresHold);

	         if (startIndex < 0) { // no sync, try again
	            if (!correctionNeeded) {
	               emitCb (cb -> syncLost);
	            }
	            badFrames ++;
	            setSynced (false);
	            inSync	= false;
	            continue;
	         }
	         sampleCount	= startIndex;
	      }
	      else {	// we are in sync and continue with a next frame
	         totalFrames ++;
	         frameCount ++;
	         totalSamples	+= sampleCount;
	         if (frameCount >= 10) {
	            rateError = SAMPLERATE *
	                          (totalSamples / ((float)frameCount * T_F) - 1);
	            emitCb (cb -> clockError, (int)rateError);
	            totalSamples = 0;
	            frameCount	= 0;
	         }

	         theReader. getSamples (ofdmBuffer, 0,
	                               T_u, coarseOffset + fineOffset,  false);
	         startIndex = myCorrelator. findIndex (ofdmBuffer,
	                                               correlationOrder,
	                                               2.5 * thresHold);
//
	         if (startIndex < 0) { // no sync, try again
	            if (!correctionNeeded) {
	               emitCb (cb -> syncLost);
	            }
	            badFrames	++;
	            inSync	= false;
	            setSynced (false);
	            continue;
	         }
	         sampleCount = startIndex;
	      }

	      goodFrames ++;
	      double cLevel	= 0;

//	The size of the ofdm Buffer is large enough to
//	read All data of the first block in
	      theReader. getSamples (ofdmBuffer,
	                             T_u,
	                             startIndex,
	                             coarseOffset + fineOffset, true);
//
//	Then we move the data of the first block to the start of the buffer:
	      memmove (ofdmBuffer. data (),
	               &((ofdmBuffer. data()) [startIndex]),
	                  T_u * sizeof (Complex));

//Block_0:
/**
  *	Block 0 is special in that it is used for fine time synchronization,
  *	for coarse frequency synchronization
  *	and its content is used as a reference for decoding the
  *	first datablock.
  *	We read the missing samples in the ofdm buffer
  */
	      sampleCount	+= T_u;
	      (void) theOfdmDecoder. processBlock_0 (ofdmBuffer);

//	Here we look only at the block_0 when we need a coarse
//	frequency synchronization.
	      correctionNeeded = !theFicHandler. syncReached ();
	      if (correctionNeeded && (tryCounter == 0)) {
	         int correction	=
	            myFreqSyncer. estimateCarrierOffset (ofdmBuffer);
	         if (correction != 100) {
	            if (abs (coarseOffset) > Khz (35))
	               coarseOffset = 0;
	            else {
	               coarseOffset	+=  correction * carrierDiff;
	               tryCounter	= 5;
	            }
	         }
	      }
	      else
	      if (!correctionNeeded)
	         tryCounter = 5;
	      else
	      if (tryCounter > 0)
	         tryCounter --;

/**
  *	after block 0, we will just read in the other
  *	(params -> L - 1) blocks
  */
//Data_blocks:
/**
  *	The first ones are the FIC blocks these are handled within
  *	the thread executing this "task", the other blocks
  *	are passed on to be handled in the mscHandler, running
  *	possibly in a different thread.
  *	We immediately start with building up an average of
  *	the phase difference between the samples in the cyclic prefix
  *	and the	corresponding samples in the datapart.
  */
	      cCount	= 0;
	      cLevel	= 0;
	      Complex FreqCorr	= Complex (0, 0);
	      for (int ofdmSymbolCount = 1;
	           ofdmSymbolCount < nrBlocks; ofdmSymbolCount ++) {
	         theReader. getSamples (ofdmBuffer, 0,
	                                 T_s, coarseOffset + fineOffset,  true);
	         sampleCount += T_s;
	         for (int i = (int)T_u; i < (int)T_s; i ++) {
	            FreqCorr +=
	                      ofdmBuffer [i] * conj (ofdmBuffer [i - T_u]);
	            cLevel += jan_abs (ofdmBuffer [i]) +
	                                  jan_abs (ofdmBuffer [i - T_u]);
	         }
	         cCount += 2 * T_g;
//
//	Normal Processing (ETI-Generator entfaellt in V2)
//	we distinguish vetween processing everything in this thread, or
//	delegate processing of the data blocks in the MSC thread
//	Of course, if scanning is ON, then we do not process
//	the payload at all
	         if (ofdmSymbolCount <= 3) {
	            theOfdmDecoder.
                           decode (ofdmBuffer, ofdmSymbolCount,
	                                            softbits, snr, rateError);
	            theFicHandler.
                            processFICBlock (softbits, ofdmSymbolCount);
	         }
	         if (scanMode)
	            continue;
	         if (ofdmSymbolCount >= 4) {
	            theOfdmDecoder.
	                    decode (ofdmBuffer, ofdmSymbolCount,
	                                             softbits, snr, rateError);
	            theMscSink ->
	                    processMscBlock (softbits, ofdmSymbolCount);
	         }
	      }
/**
  *	OK,  here we are at the end of the frame
  *	Assume everything went well and skip T_null samples
  */
	      theReader. getSamples (ofdmBuffer, 0,
	                         T_null, coarseOffset + fineOffset, false);
	      sampleCount += T_null;
//
//	The snr is computed, where we take as "noise" the signal strength
//	of the NULL period (the one without TII data)
//	" The TII signal shall fill the null symbol of each transmission
//	frame comprising the CIFs of CIF count 0, 1, 2, 3
//	modulo 8 (transmission mode I).
//	We have the CIF count of the previous frame
	      if (p -> dabMode == 1) {
	         int16_t CIF_hi, CIF_lo;
	         theFicHandler. getCIFcount (CIF_hi, CIF_lo);
	         if ((CIF_lo & 0x07) >= 4) {
	            if (p -> tiiEnabled) {
	               theTIIDetector. addBuffer (ofdmBuffer);
	               if (++tiiCounter >= tiiDelay) {
	                  tiiCounter = 0;
	                  std::vector<tiiData> resVec =
	                       theTIIDetector. processNULL (tiiThreshold,
	                                                    tiiCollision);
	                  emitCb (cb -> tii, resVec);
	               }
	            }
	         }
	         else {	// compute SNR
	            float sum	= 0;
	            for (int i = 0; i < T_null; i ++)
	               sum += jan_abs (ofdmBuffer [i]);
	            sum /= T_null;
	            float snrV	=
	                 20 * log10 ((cLevel / cCount + 0.005) / (sum + 0.005));
	            this -> snr = 0.85f * this -> snr + 0.15f * snrV;
	            float snrCopy = this -> snr;
	            snrCount ++;
	            if (snrCount >= 3) {
	               snrCount = 0;
	               emitCb (cb -> snr, snrCopy);
	            }
	         }
	      }
/**
  *	The first sample to be found for the next frame should be T_g
  *	samples ahead. Before going for the next frame, we
  *	we just check the fineCorrector
  */
//NewOffset:
//     we integrate the newly found frequency error with the
//     existing frequency error.

	      int oldCoarseOffset	= coarseOffset;
	      fineOffset += 0.18 * arg (FreqCorr) / (2 * M_PI) * carrierDiff;
	      if (fineOffset > carrierDiff / 2) {
	         coarseOffset += carrierDiff;
	         fineOffset -= carrierDiff;
	      }
	      else
	      if (fineOffset < -carrierDiff / 2) {
	         coarseOffset -= carrierDiff;
	         fineOffset += carrierDiff;
	      }
	      emitCb (cb -> corrector, coarseOffset, (float)fineOffset);
	      if ((oldCoarseOffset != coarseOffset) &&
	          (theFicHandler. getFICQuality () < 40))
	              correctionNeeded = true;;

//ReadyForNewFrame:
///	and off we go, up to the next frame
	   }
	}
	catch (int e) {
//	   stop requested (20/21 from sampleReader::getSamples)
	   (void)e;
	}
	threadRunning. store (false);
}
//
//
void	ofdmHandler::setScanMode	(bool b) {
	scanMode	= b;
	attempts	= 0;
}

void	ofdmHandler::getFrameQuality	(int	*totalFrames,
	                                 int	*goodFrames,
	                                 int	*badFrames) {
	*totalFrames		= this	-> totalFrames;
	*goodFrames		= this	-> goodFrames;
	*badFrames		= this	-> badFrames;
	this	-> totalFrames	= 0;
	this	-> goodFrames	= 0;
	this	-> badFrames	= 0;
}

void	ofdmHandler::handleDecoderSelector	(int d) {
	theOfdmDecoder. handle_decoderSelector (d);
}

void	ofdmHandler::setCorrelationOrder	(bool b) {
	correlationOrder = b;
}

void	ofdmHandler::setDXMode		(bool b) {
	dxMode	= b;
}

void	ofdmHandler::set_dcRemoval	(bool b) {
	theReader. set_dcRemoval (b);
}
