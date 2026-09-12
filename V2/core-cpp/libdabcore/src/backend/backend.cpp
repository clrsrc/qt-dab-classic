// DAB Classic v3: portiert aus Qt-DAB sources/backend/backend.cpp
// (Jan van Katwijk, GPLv2+), siehe backend.h.
#
/*
 *    Copyright (C) 2016 .. 2024
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
#include	"dab-constants.h"
#include	"backend.h"
#include	<chrono>
//
//	Interleaving is - for reasons of simplicity - done
//	inline rather than through a special class-object
#define CUSize  (4 * 16)

//	fragmentsize == Length * CUSize
	Backend::Backend	(const descriptorType	*d,
	                         BackendCallbacks	*cb,
	                         int			flag,
	                         uint8_t		cpuSupport,
	                         AacDecoderKind		aacKind):
	                                    deconvolver (d, cpuSupport),
	                                    hardBits (d -> bitRate * 24),
	                                    driver (d, cb, aacKind) {
	this	-> backendType		= d -> type;
	this	-> startAddr		= d -> startAddr;
	this	-> Length		= d -> length;
        this    -> fragmentSize         = d -> length * CUSize;
	this	-> bitRate		= d -> bitRate;
	this	-> serviceId		= d -> SId;
	this	-> serviceName		= d -> serviceName;
	this	-> shortForm		= d -> shortForm;
	this	-> protLevel		= d -> protLevel;
	this	-> subChId		= d -> subchId;
	this	-> borf			= flag;
	frameTap. store (nullptr);

	interleaveData. resize (16);
	for (int i = 0; i < 16; i ++) {
	   interleaveData [i]. resize (fragmentSize);
	   memset (interleaveData [i]. data (), 0,
	                               fragmentSize * sizeof (int16_t));
	}

	countforInterleaver	= 0;
	interleaverIndex	= 0;

	tempX. resize (fragmentSize);

	uint8_t shiftRegister [9];
	disperseVector. resize (24 * bitRate);
	memset (shiftRegister, 1, 9);
	for (int i = 0; i < bitRate * 24; i ++) {
	   uint8_t b = shiftRegister [8] ^ shiftRegister [4];
	   for (int j = 8; j > 0; j--)
	      shiftRegister [j] = shiftRegister [j - 1];
	   shiftRegister [0] = b;
	   disperseVector [i] = b;
	}
//	for local buffering the input, we have
	nextIn				= 0;
	nextOut				= 0;
	usedSlots			= 0;
	for (int i = 0; i < NUMBER_SLOTS; i ++)
	   theData [i]. resize (fragmentSize);
	running. store (true);
	theThread = std::thread ([this] () { run (); });
}

	Backend::~Backend () {
	stopRunning ();
}

//	Aufruf aus dem OFDM-Thread: Segment in den Ring legen. Ist der Ring
//	voll, wartet der Erzeuger (v1: tryAcquire (1, 200) in Schleife, solange
//	running); so bremst ein langsames Backend den OFDM-Thread (Backpressure).
int32_t	Backend::process	(int16_t *softBits, int16_t cnt) {
	(void)cnt;
	{
	   std::unique_lock<std::mutex> lk (slotM);
	   while (usedSlots >= NUMBER_SLOTS) {
	      if (!running. load ())
	         return 0;
	      slotCv. wait_for (lk, std::chrono::milliseconds (200));
	   }
	   if (!running. load ())
	      return 0;
	   memcpy (theData [nextIn]. data (), softBits,
	                           fragmentSize * sizeof (int16_t));
	   nextIn = (nextIn + 1) % NUMBER_SLOTS;
	   usedSlots ++;
	}
	slotCv. notify_all ();
	return 1;
}

const	int16_t interleaveMap [] = {0,8,4,12,2,10,6,14,1,9,5,13,3,11,7,15};
void	Backend::processSegment (int16_t *softBits_in) {

	for (uint16_t i = 0; i < fragmentSize; i ++) {
	   tempX [i] = interleaveData [(interleaverIndex +
	                                interleaveMap [i & 017]) & 017][i];
	   interleaveData [interleaverIndex][i] = softBits_in [i];
	}

	interleaverIndex = (interleaverIndex + 1) & 0x0F;
//	Slot freigeben (v1: freeSlots. release (1))
	{
	   std::lock_guard<std::mutex> lk (slotM);
	   nextOut = (nextOut + 1) % NUMBER_SLOTS;
	   usedSlots --;
	}
	slotCv. notify_all ();

//	only continue when de-interleaver is filled
	if (countforInterleaver <= 15) {
	   countforInterleaver ++;
	   return;
	}

	deconvolver. deconvolve (tempX. data(), fragmentSize, hardBits. data());
//	and the energy dispersal
	for (uint16_t i = 0; i < bitRate * 24; i ++)
	   hardBits [i] ^= disperseVector [i];
	if (!running. load ())
	   return;
//	Timeshift: der Tap entscheidet, wann der Rahmen beim Driver landet
	IFrameTap *tap = frameTap. load ();
	if (tap != nullptr)
	   tap -> onBackendFrame (hardBits);
	else
	   driver. addtoFrame (hardBits);
}

void	Backend::setFrameTap	(IFrameTap *tap) {
	frameTap. store (tap);
}

void	Backend::deliverFrame	(const std::vector<uint8_t> &hardBits) {
	if (running. load ())
	   driver. addtoFrame (hardBits);
}

void	Backend::run () {
	while (running. load ()) {
	   int16_t slot;
	   {
	      std::unique_lock<std::mutex> lk (slotM);
	      slotCv. wait (lk, [this] () {
	                       return usedSlots > 0 || !running. load (); });
	      if (!running. load ())
	         return;
	      slot = nextOut;
	   }
	   processSegment (theData [slot]. data());
	}
}

//	Beenden: Thread anhalten und auf ihn warten (v1: usleep-Schleife bis
//	isFinished). Nach der Rueckkehr laufen keine Callbacks mehr.
void	Backend::stopRunning () {
	{
	   std::lock_guard<std::mutex> lk (slotM);
	   running. store (false);
	}
	slotCv. notify_all ();
	if (theThread. joinable ())
	   theThread. join ();
//	der Backend-Thread steht: ab hier kommt kein Tap-Aufruf mehr
	frameTap. store (nullptr);
	driver. stop ();
}

bool	Backend::is_dataBackend () {
	return backendType == PACKET_SERVICE;
}
