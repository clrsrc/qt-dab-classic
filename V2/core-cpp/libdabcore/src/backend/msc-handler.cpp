// DAB Classic v3: portiert aus Qt-DAB sources/backend/msc-handler.cpp
// (Jan van Katwijk, GPLv2+), siehe msc-handler.h.
#
/*
 *    Copyright (C) 2014 .. 2025
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *    Lazy Chair Computing
 *
 *    This file is part of the Qt-DAB program
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
#include	"dab-constants.h"
#include	"msc-handler.h"
#include	"backend.h"
#include	"dab-params.h"
//
//	Interface program for processing the MSC.
//	The dabProcessor assumes the existence of an msc-handler, whether
//	a service is selected or not.

#define	CUSize	(4 * 16)
static int cifTable [] = {18, 72, 0, 36};

//	Note CIF counts from 0 .. 3
//
		mscHandler::mscHandler	(uint8_t	dabMode,
	                                 uint8_t 	cpuSupport):
	                                       params (dabMode) {
	this	-> cpuSupport	= cpuSupport;
	cifVector. resize (55296);
	BitsperBlock		= 2 * params. get_carriers();
	numberofblocksperCIF = cifTable [(dabMode - 1) & 03];
}

		mscHandler::~mscHandler () {
	stop ();
}

void	mscHandler::stop () {
	std::lock_guard<std::mutex> lk (locker);
	for (auto &b : theBackends) {
	   b -> stopRunning();
	   delete b;
	}
	theBackends. resize (0);
}

//
//	Note, the set_Channel function is called from within a
//	different thread than the process_mscBlock method is,
//	so, a little bit of locking seems wise while
//	the actual changing of the settings is done in the
//	thread executing process_mscBlock
void	mscHandler::resetChannel () {
	stop ();
}

void	mscHandler::stopBackend	(Backend *which) {
	std::lock_guard<std::mutex> lk (locker);
	for (int i = 0; i < (int)(theBackends. size ());  i ++) {
	   Backend *b = theBackends. at (i);
	   if (b == which) {
	      b -> stopRunning ();
	      delete b;
	      theBackends. erase (theBackends. begin () + i);
	      break;
	   }
	}
}
//
//	Note that - in general - the backens run in their own thread
Backend	*mscHandler::startBackend (const descriptorType &d,
	                           BackendCallbacks *cb,
	                           int flag,
	                           AacDecoderKind aacKind) {
	std::lock_guard<std::mutex> lk (locker);
	Backend *b = new Backend (&d, cb, flag, cpuSupport, aacKind);
	theBackends. push_back (b);
	return b;
}

bool	mscHandler::serviceRuns	(uint32_t SId, uint16_t subChId) {
	std::lock_guard<std::mutex> lk (locker);
	for (auto &backend : theBackends)
	   if ((backend -> serviceId == (int) SId) && (backend -> subChId == subChId))
	      return true;
	return false;
}

int	mscHandler::activeServices () {
	std::lock_guard<std::mutex> lk (locker);
	return (int)theBackends. size ();
}

//
//	add blocks. First is (should be) block 4, last is (should be)
//	nrBlocks -1.
//	Note that this method is called from within the ofdm-processor thread
//	while the set_xxx methods are called from within the
//	gui thread, so some locking is added
//

void	mscHandler::processMscBlock	(const std::vector<int16_t> &softBits,
	                                 int blkno) {
int16_t	currentblk	= (blkno - 4) % numberofblocksperCIF;

//	and the normal operation is:
	memcpy (&cifVector [currentblk * BitsperBlock],
	                    softBits. data(), BitsperBlock * sizeof (int16_t));
	if (currentblk < numberofblocksperCIF - 1)
	   return;

//	OK, now we have a full CIF and it seems there is some work to
//	be done.  We assume that the backend itself
//	does the work in a separate thread.
	std::lock_guard<std::mutex> lk (locker);
	for (auto & b: theBackends) {
	   int16_t startAddr	= b -> startAddr;
	   int16_t Length	= b -> Length;
//	Review 16.09.2026 M4: ein CIF hat 864 CUs (cifVector = 864 * 64);
//	nur innerhalb dieser Grenze lesen (FIG 0/1 wird schon gefiltert,
//	das hier ist die letzte Sicherung vor dem memcpy)
	   if ((Length > 0) && (startAddr >= 0) &&
	       (startAddr + Length <= 864))
	      (void) b -> process (&cifVector [startAddr * CUSize],
	                                      Length * CUSize);
	}
}
