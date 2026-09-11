// DAB Classic v3: portiert aus Qt-DAB sources/support/fft-handler.cpp
// (Jan van Katwijk, GPLv2+), Qt entfernt: Wisdom-Datei (QDir) entfaellt,
// Planner-Lock wie v1 (d9716d8) bleibt.
#
/*
 *    Copyright (C) 2015 .. 2020
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
//
//
#include	"fft-handler.h"
#include	<cstdlib>
#include	<cstring>
#include	<mutex>

//	The FFTW planner (and wisdom import/export) is NOT thread-safe.
//	With FFTW_MEASURE the planning window is large, so guard all
//	planner interaction with a single process-wide lock.
static std::mutex	fftwPlannerLock;

	fftHandler::fftHandler	(int size, bool dir) {
	this	-> size		= size;
	this	-> dir		= dir;

//	Serialize plan creation across all threads.
	std::lock_guard<std::mutex> guard (fftwPlannerLock);

#ifdef	__WITH_DOUBLES__
	fftVector		= (Complex *)
	                          fftw_malloc (sizeof (Complex) * size);
	plan			= fftw_plan_dft_1d (size,
	                           reinterpret_cast <fftw_complex *>(fftVector),
                                   reinterpret_cast <fftw_complex *>(fftVector),
                                   FFTW_FORWARD, FFTW_MEASURE);
#else
	fftVector		= (Complex *)
	                          fftwf_malloc (sizeof (Complex) * size);
	plan			= fftwf_plan_dft_1d (size,
	                           reinterpret_cast <fftwf_complex *>(fftVector),
                                   reinterpret_cast <fftwf_complex *>(fftVector),
                                   FFTW_FORWARD, FFTW_MEASURE);
#endif
}

	fftHandler::~fftHandler	() {
	std::lock_guard<std::mutex> guard (fftwPlannerLock);
#ifdef	__WITH_DOUBLES__
	fftw_destroy_plan (plan);
	fftw_free (fftVector);
#else
	fftwf_destroy_plan (plan);
	fftwf_free (fftVector);
#endif
}

void	fftHandler::fft		(std::vector<Complex> &v) {
	if (dir) {
	   for (int i = 0; i < size; i ++)
	      fftVector [i] = conj (v [i]);
	}
	else {
	   memcpy (fftVector, v. data (), size * sizeof (Complex));
	}
#ifdef	__WITH_DOUBLES__
	fftw_execute (plan);
#else
	fftwf_execute (plan);
#endif
	if (dir) {
	   for (int i = 0;  i < size; i ++)
	      v [i] = conj (fftVector [i]);
	}
	else {
	   memcpy (v. data (), fftVector, size * sizeof (Complex));
	}
}

void	fftHandler::fft		(Complex  *v) {
	memcpy (fftVector, v, size * sizeof (Complex));
#ifdef	__WITH_DOUBLES__
	fftw_execute (plan);
#else
	fftwf_execute (plan);
#endif
	memcpy (v, fftVector, size * sizeof (Complex));
}
