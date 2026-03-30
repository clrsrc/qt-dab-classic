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
#include	<QDir>

//	FFTW Wisdom management -- load once, save on exit
bool		fftWisdom::wisdomLoaded = false;

std::string	fftWisdom::getWisdomPath () {
	QString path = QDir::homePath () + "/.qt-dab-fftw-wisdom";
	return path. toStdString ();
}

void	fftWisdom::loadWisdom () {
	if (wisdomLoaded)
	   return;
	wisdomLoaded = true;
	std::string path = getWisdomPath ();
#ifdef	__WITH_DOUBLES__
	fftw_import_wisdom_from_filename (path. c_str ());
#else
	fftwf_import_wisdom_from_filename (path. c_str ());
#endif
}

void	fftWisdom::saveWisdom () {
	std::string path = getWisdomPath ();
#ifdef	__WITH_DOUBLES__
	fftw_export_wisdom_to_filename (path. c_str ());
#else
	fftwf_export_wisdom_to_filename (path. c_str ());
#endif
}

	fftHandler::fftHandler	(int size, bool dir) {
	this	-> size		= size;
	this	-> dir		= dir;

	fftWisdom::loadWisdom ();

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

