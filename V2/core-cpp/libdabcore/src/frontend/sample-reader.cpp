// DAB Classic v3: portiert aus Qt-DAB sources/frontend/sample-reader.cpp
// (Jan van Katwijk, GPLv2+), Qt entfernt – siehe sample-reader.h.
#
/*
 *    Copyright (C) 2013 .. 2023
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
#include	"sample-reader.h"
#include	"dab-constants.h"
#include	<algorithm>

static
Complex oscillatorTable [SAMPLERATE];
constexpr float ALPHA = 1.0f / SAMPLERATE;

	sampleReader::sampleReader (ISampleSource	*theRig_i):
	                               theRig (theRig_i) {
	currentPhase	= 0;
	sLevel		= 0;
	sampleCount	= 0;
	dcRemoval	= false;
	dcReal		= 0;
	dcImag		= 0;
	IQ_Real		= 0;
	IQ_Imag		= 0;
	dcSampleCounter	= 0;
	dcDisplayCounter = 0;
	dcIndicator	= 0;

	for (int i = 0; i < SAMPLERATE; i ++)
	   oscillatorTable [i] = Complex
	                            (cos (2.0 * M_PI * i / SAMPLERATE),
	                             sin (2.0 * M_PI * i / SAMPLERATE));

	bufferContent	= 0;
	corrector	= 0;
	running. store (true);
}

	sampleReader::~sampleReader () {
}

void	sampleReader::setRunning (bool b) {
	running. store (b);
}

float	sampleReader::getSLevel () {
	return sLevel;
}

Complex	sampleReader::getSample (float phaseOffset) {
std::vector<Complex> buffer (1);

	getSamples (buffer, 0, 1, phaseOffset,  false);
	return buffer [0];
}

void	sampleReader::getSamples (std::vector<Complex>  &v_out,
	                           int index,
	                           int32_t nrSamples,
	                           int32_t phaseOffset, bool saving) {
auto *buffer	= dynVec (std::complex<float>, nrSamples);
	(void)saving;
	corrector	= phaseOffset;

//	if we get a kill signal, do the kill
	if (!running. load())
	   throw 21;
//
//	wait for samples (v1: usleep (10)-Schleife; hier Condition-Variable
//	der Quelle mit kurzem Timeout, damit "running" geprueft werden kann)
	if (nrSamples > bufferContent) {
	   bufferContent = theRig -> samples ();
	   while ((bufferContent < nrSamples) && running. load()) {
	      theRig -> waitForSamples (nrSamples, 10);
	      bufferContent = theRig -> samples ();
	   }
	}

	if (!running. load())
	   throw 20;
//
//	so here, bufferContent >= n
	nrSamples	= theRig -> getSamples (buffer, nrSamples);
	bufferContent	-= nrSamples;

//	OK, we have samples!!
	bool doDcRemoval = dcRemoval. load ();

	for (int i = 0; i < nrSamples; i ++) {
	   float Alpha;
	   if (dcSampleCounter < DC_FAST_SETTLE_SAMPLES)  {
	      Alpha	= 1.0f / 8192;
	      dcSampleCounter++;
	   }
	   else
	      Alpha	= ALPHA;
	   std::complex<float> v = buffer [i];
	   if (doDcRemoval) {
	      dcReal		= compute_avg (dcReal, real (v), Alpha);
	      dcImag		= compute_avg (dcImag, imag (v), Alpha);
	      v = std::complex<float> (real (v) - dcReal, imag (v) - dcImag);
	      v = theEqualizer. equalize (v);
	      DABFLOAT real_V	= abs (real (v));
	      DABFLOAT imag_V	= abs (imag (v));
	      IQ_Real		= compute_avg (IQ_Real, real_V, Alpha);
	      IQ_Imag		= compute_avg (IQ_Imag, imag_V, Alpha);
	   }

//	first: adjust frequency. We need Hz accuracy
//	Note that "phase" itself might be negative
	   currentPhase	-= phaseOffset;
	   currentPhase	= (currentPhase + SAMPLERATE) % SAMPLERATE;
	   v_out  [index + i]	= v * oscillatorTable [currentPhase];
	   sLevel = 0.00001 * jan_abs (v_out [index + i]) + (1 - 0.00001) * sLevel;
	}

//	Spektrum-Scope: 2048 Samples einsammeln, dann (rate-begrenzt) melden
	if (spectrumOn. load () && spectrumHook) {
	   auto now = std::chrono::steady_clock::now ();
	   if (now >= spectrumNext) {
	      if (spectrumBuf. size () != 2048)
	         spectrumBuf. resize (2048);
	      int take = std::min (nrSamples, 2048 - spectrumFill);
	      for (int i = 0; i < take; i ++)
	         spectrumBuf [spectrumFill + i] = v_out [index + i];
	      spectrumFill += take;
	      if (spectrumFill >= 2048) {
	         spectrumFill = 0;
//	Deadline fortschreiben (mittlere Rate = rateHz), ohne Nachholen nach Pausen
	         const auto interval = std::chrono::milliseconds (spectrumIntervalMs. load ());
	         if (spectrumNext + interval < now)
	            spectrumNext = now;
	         spectrumNext += interval;
	         spectrumHook (spectrumBuf. data (), 2048);
	      }
	   }
	}

	if (doDcRemoval) {
	   dcDisplayCounter += nrSamples;
	   if (dcDisplayCounter >= SAMPLERATE) {
	      DABFLOAT sum = IQ_Real + IQ_Imag;
	      if (sum > 1.0e-10f)
	         dcIndicator = 10 * (IQ_Real - IQ_Imag) / (sum / 2);
	      dcDisplayCounter = 0;
	   }
	}

	sampleCount	+= nrSamples;
}

void	sampleReader::setSpectrum	(bool on, int rateHz) {
	if (rateHz < 1) rateHz = 1;
	if (rateHz > 10) rateHz = 10;
	spectrumIntervalMs. store (1000 / rateHz);
	if (on && !spectrumOn. load ()) {
	   spectrumFill = 0;
	   spectrumNext = std::chrono::steady_clock::time_point {};
	}
	spectrumOn. store (on);
}

void	sampleReader::set_dcRemoval	(bool b) {
	if (b && !dcRemoval. load ())
	   dcSampleCounter = 0;
	dcRemoval	= b;
}
