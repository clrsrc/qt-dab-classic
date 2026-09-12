// DAB Classic v3: portiert aus Qt-DAB sources/frontend/ofdm-decoder.h
// (Jan van Katwijk, GPLv2+), Qt entfernt: QObject/Signale (showIQ,
// show_quality, show_stdDev) und die Scope-Ringpuffer gestrichen; die
// Qualitaetswerte gehen bei Bedarf ueber einen Callback. Decoder 1..4
// unveraendert, Decoder 3 ist Standard (wie v1).
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
#pragma once

#include	"dab-constants.h"
#include	<vector>
#include	<cstdint>
#include	<atomic>
#include	<chrono>
#include	<functional>
#include	"phasetable.h"
#include	"freq-interleaver.h"
#include	"dab-params.h"
#include	"fft-handler.h"

#ifndef	M_PI_2
#define	M_PI_2	(M_PI / 2)
#endif
#ifndef	M_PI_4
#define	M_PI_4	(M_PI / 4)
#endif
class	ofdmDecoder {
public:
		ofdmDecoder		(uint8_t dabMode, int16_t bitDepth);
		~ofdmDecoder		();
//	Note: the parameter should not be altered, it is used later on
	void	processBlock_0		(std::vector<Complex>);
	void	decode			(std::vector<Complex> &,
	                                 int32_t n,
	                                 std::vector<int16_t> &,
	                                 DABFLOAT, float);
	void	stop			();
	void	reset			();
	void	handle_decoderSelector	(int);
//	Qualitaet (MER, Zeit-, Frequenzversatz) von Symbol 2, ca. 1x/s;
//	nur berechnet, wenn ein Callback gesetzt ist.
	void	setQualityCallback	(std::function<void(float, float, float)> cb) {
	   qualityCb = std::move (cb);
	}
//	IQ-Scope (V3, Ersatz fuer den v1-iqBuffer): Konstellation von Symbol 2,
//	1536 Traeger in Frequenzreihenfolge (k = -768..-1, 1..768) nach der
//	Differenzdemodulation, auf den Einheitskreis normiert, als int8-Paare
//	I,Q (127 = 1,0). Hoechstens rateHz-mal je Sekunde; aus: kein Aufwand.
	void	setIqHook		(std::function<void(const std::vector<int8_t> &)> h) {
	   iqHook = std::move (h);
	}
	void	setIq			(bool on, int rateHz);
private:
	std::function<void(const std::vector<int8_t> &)> iqHook;
	std::atomic<bool>	iqOn {false};
	std::atomic<int>	iqIntervalMs {200};
	std::chrono::steady_clock::time_point iqNext {};
	std::vector<int8_t>	iqOut;
	void	emitIq			();
	dabParams		params;
	phaseTable		theTable;
	interLeaver		myMapper;
	fftHandler		fft;
	std::function<void(float, float, float)> qualityCb;

	DABFLOAT		decoder_12 (const std::vector<Complex> &,
                                            std::vector<int16_t> &,
                                            DABFLOAT        snr,
                                            int             decType,
	                                    float	   rateError);
	DABFLOAT		decoder_3  (const std::vector<Complex> &,
                                            std::vector<int16_t> &,
                                            DABFLOAT        snr,
	                                    float	   clockError,
	                                    bool	   updateDisplay);
	DABFLOAT		decoder_4  (const std::vector<Complex> &,
                                            std::vector<int16_t> &,
                                            DABFLOAT        snr);

	float		computeQuality		(Complex *);
	float		compute_timeOffset      (Complex *,
	                                         Complex *);
	float		compute_frequencyOffset (Complex *,
	                                         Complex *);
	int32_t		T_s;
	int32_t		T_u;
	int32_t		T_g;
	int32_t		nrBlocks;
	int32_t		carriers;
	std::vector<Complex>	phaseReference;
	std::vector<Complex>	conjVector;
	std::vector<Complex>	fft_buffer;
	std::vector<DABFLOAT>	sigmaSQ_Vector;
	std::vector<DABFLOAT>	meanLevelVector;

	float		meanValue;
	float		avgBit;
	int		decoder;
	int		repetitionCounter;
	int		cnt;
	float		f_n, f_d;		// computeQuality
	float		vv;			// compute_frequencyOffset

	double		sqrt_2;
};
