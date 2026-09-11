// DAB Classic v3: portiert aus Qt-DAB sources/frontend/sample-reader.h
// (Jan van Katwijk, GPLv2+), Qt entfernt: QObject/Signale (show_spectrum,
// show_dcOffset), riffWriter-Dump und Spektrumpuffer gestrichen; Quelle
// ist ISampleSource statt deviceHandler, Warten per waitForSamples statt
// usleep-Polling. DC-Entfernung, Equalizer, Frequenzkorrektur wie v1.
#
/*
 *    Copyright (C) 2016 .. 2023
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
 *	Reading the samples from the input device. Since it has its own
 *	"state", we embed it into its own class
 */
#include	"dab-constants.h"
#include	<cstdint>
#include	<atomic>
#include	<vector>
#include	"isample-source.h"
#include	"equalizer.h"

class	sampleReader {
public:
	      	sampleReader	(ISampleSource *theRig);
	      	~sampleReader		();
	      void	setRunning	(bool b);
	      float	getSLevel	();
	      Complex	getSample	(float);
	      void	getSamples	(std::vector<Complex> &v,
	                                 int index,
	                                 int32_t n, int32_t phase,  bool saving);
	      void	set_dcRemoval	(bool);
	      float	dcOffsetIndicator () const { return dcIndicator; }
private:
	      equalizer		theEqualizer;
	      ISampleSource	*theRig;
	      int32_t		currentPhase;
	      std::atomic<bool>	running;
	      int32_t		bufferContent;
	      float		sLevel;
	      int32_t		sampleCount;
	      int32_t		corrector;

	      static constexpr int DC_FAST_SETTLE_SAMPLES = 100000;
	      std::atomic<bool>	dcRemoval;
	      DABFLOAT		dcReal;
	      DABFLOAT		dcImag;
	      DABFLOAT		IQ_Real;
	      DABFLOAT		IQ_Imag;
	      int		dcSampleCounter;
	      int		dcDisplayCounter;
	      float		dcIndicator;
};
