// DAB Classic v3: portiert aus Qt-DAB sources/frontend/ofdm-decoder.cpp
// (Jan van Katwijk, GPLv2+), Qt entfernt – siehe ofdm-decoder.h.
#
/*
 *    Copyright (C) 2014 .. 2024
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
 *
 *	Once the bits are "in", interpretation and manipulation
 *	should reconstruct the data blocks.
 *	Ofdm_decoder is called for Block_0 and the FIC blocks,
 *	its invocation results in 2 * Tu bits
 */
#include	<vector>
#include	<cmath>
#include	<algorithm>
#include	"ofdm-decoder.h"
#include	"freq-interleaver.h"
#include	"dab-params.h"
#include	"dab-constants.h"
/**
  *	\brief ofdmDecoder
  *	The class ofdmDecoder is
  *	taking the data from the ofdmProcessor class in, and
  *	will extract the Tu samples, do an FFT and extract the
  *	carriers and map them on (soft) bits
  */
#define	ALPHA	0.005f
static inline
Complex	normalize (const Complex &V) {
DABFLOAT length	= jan_abs (V);
	if (length < 0.001)
	   return Complex (0, 0);
	return Complex (V) / length;
}
//
//	The bessel function is under windows too slow too work with
//	that is why we created a table that is filled on startup
static DABFLOAT besselTable [2048];
static inline
DABFLOAT IO_Bessel	(DABFLOAT x) {
	return std::cyl_bessel_i (0.0f, x);
}

// and table access is with this function
static inline
DABFLOAT IO (DABFLOAT x) {
	return besselTable [((int)(x * 32)) % 2048];
}

static inline
void	limit_symmetrically (DABFLOAT &v, DABFLOAT limit) {
	if (v < -limit)
	   v = -limit;
	if (v > limit)
	   v = limit;
}
static inline
Complex w (DABFLOAT kn) {
	DABFLOAT re	= cos (kn * M_PI / 4);
	DABFLOAT im	= sin (kn * M_PI / 4);
	return Complex (re, im);
}

static Complex W_table [8];
static inline
DABFLOAT makeA (int i, Complex S, Complex prevS) {
	return abs  (prevS + W_table [i] * S);
}

	ofdmDecoder::ofdmDecoder	(uint8_t	dabMode,
	                                 int16_t	bitDepth) :
	                                    params	(dabMode),
	                                    theTable	(dabMode),
	                                    myMapper	(dabMode),
	                                    fft (params. get_T_u (), false),
	                                    phaseReference (params. get_T_u ()),
	                                    conjVector	(params. get_T_u ()),
	                                    fft_buffer	(params. get_T_u ()),
	                                    sigmaSQ_Vector (params. get_T_u ()),
	                                    meanLevelVector (params. get_T_u ()){
	(void)bitDepth;
//
	this	-> T_s		= params. get_T_s	();
	this	-> T_u		= params. get_T_u	();
	this	-> nrBlocks	= params. get_L		();
	this	-> carriers	= params. get_carriers	();
	this	-> T_g		= T_s - T_u;

	repetitionCounter	= 10;
	cnt			= 0;
	f_n			= 1;
	f_d			= 1;
	vv			= 0;
	reset ();
	decoder			= DECODER_1;

	sqrt_2			= sqrt (2);
//
//	Prefil some tables for faster access
	for (int i = 0; i < 2048; i ++) {
	   besselTable [i] = IO_Bessel (((float)i) / 32.0);
	}

	for (int i = 0; i < 8; i ++)
	   W_table [i] = w (-i);
}

	ofdmDecoder::~ofdmDecoder	() {
}
//
void	ofdmDecoder::stop ()	{
}
//
void	ofdmDecoder::reset ()	{
	for (int i = 0; i < T_u; i ++) {
	   sigmaSQ_Vector [i]	= 0;
	   meanLevelVector [i]	= 0;
	}
	meanValue	= 1.0f;
	avgBit		= 10.0f;
}
//
void	ofdmDecoder::processBlock_0 (std::vector <Complex> buffer) {
	fft. fft (buffer);
//	we are now in the frequency domain, and we keep the carriers
//	for their phases.
	memcpy (phaseReference. data (), buffer. data (),
	                                      T_u * sizeof (Complex));
}
//
//	Just interested. In the ideal case the constellation of the
//	decoded symbols is precisely in the four points
//	k * (1, 1), k * (1, -1), k * (-1, -1), k * (-1, 1)
//	To ease computation, we map all incoming values onto quadrant 1
//
//	For the computation of the MER we use the definition
//	from ETSI TR 101 290 (appendix C1)
float	ofdmDecoder::computeQuality (Complex *v) {
	for (int i = 0; i < carriers; i ++) {
	   Complex ss	= v [T_u / 2 - carriers / 2 + i];
	   float ab	= jan_abs (ss) / sqrt_2;
	   f_n		=  0.99 * f_n + 0.01 * (jan_abs (ss) * jan_abs (ss));
	   float R	= abs (abs (real (ss)) - ab);
	   float I	= abs (abs (imag (ss)) - ab);
	   f_d		= 0.99 * f_d + 0.01 * (R * R + I * I);
	}
	return 10 * log10 (f_n / f_d + 0.1);
}

//	for the other blocks of data, the first step is to go from
//	time to frequency domain, to get the carriers.
//	we distinguish between FIC blocks and other blocks,
//	only to spare a test. The mapping code is the same

//
//	DAB (and DAB+) bits are encoded is DPSK, 2 bits per carrier,
//	depending on the quadrant the carrier is in. There are
//	of course two different approaches in decoding the bits
//	One is looking at the X and Y components, and
//	their length, relative to each other,
//	Ideally, the X and Y are of equal size, in practice they are not.

//
//	The decoders 1  and 2  are based on "Soft optimal 2" in
//	"Soft decisions for DQPSK demodulation for the Viterbi
//	decoding of the convolutional codes"
//	Thushara C Hewavithana and Mike Brooks
//
//	the formula (decoders 1 and 2) (in my own words)
//	Corrector = sqrt (2) / (SigmaSQ * (1 /SNR + 2))
//	where corrector is to be applied on real (symbol) and imag (symbol)
//	The implied assumption here is that abs (symbol) == 1,
//
//	The basic idea behind the formula is to enlarge the
//	spread in sizes of the real (symb) and imag (symb),
//	which is obviously inversely proportional with the sigmaSq
//
//	It shows that the resulting values for "Corrector" are
//	small, so for mapping those on -127 .. 127 a "scaler" is needed
//	(Thanks to Rolf Zerr, aka Old-dab).
//
//	Decoder 4 is an interpretation of the so-called "Optimal 3"
//	version in the aforementioned paper.

void	ofdmDecoder::decode (std::vector <Complex> &buffer,
	                     int32_t	blkno,
	                     std::vector<int16_t> &softbits,
	                     DABFLOAT	snr, float clockError) {

DABFLOAT sum	= 0;

	memcpy (fft_buffer. data (), &((buffer. data ()) [T_g]),
	                               T_u * sizeof (Complex));
//	first step: do the FFT
	fft. fft (fft_buffer. data ());
//
//	map the fft output onto bits ("carrier" elements in, 2 * softbits out)
	switch (this -> decoder) {
	  case DECODER_1:
	      sum	= decoder_12 (fft_buffer, softbits,
	                                              snr, 1, clockError);
	      break;
	  case DECODER_2:
	      sum	= decoder_12 (fft_buffer, softbits,
	                                              snr, 2, clockError);
	      break;
	  default:
	  case DECODER_3:
	      sum	= decoder_3 (fft_buffer, softbits,
	                                    snr, clockError, blkno == 2);
	      break;
	  case DECODER_4:
	      sum	= decoder_4 (fft_buffer, softbits, snr);
	      break;
	}

	meanValue	= compute_avg (meanValue, sum /carriers, 0.1);

//		end of decoding	, now for displaying things	//
//////////////////////////////////////////////////////////////////
//	From time to time we report the quality of symbol 2
//	(v1: Konstellation/IQ-Puffer entfallen hier).
	if (blkno == 2) {
	   if (iqOn. load () && iqHook)
	      emitIq ();
	   if (++cnt > repetitionCounter) {
	      if (qualityCb) {
	         float freqOffset = compute_frequencyOffset (fft_buffer. data (),
	                                                     phaseReference. data ());
	         float Quality	= computeQuality (conjVector. data ());
	         float timeOffset = compute_timeOffset (fft_buffer. data (),
	                                                phaseReference. data ());
	         qualityCb (Quality, timeOffset, freqOffset);
	      }
	      cnt = 0;
	   }
	}

	memcpy (phaseReference. data(), fft_buffer. data (),
	                            T_u * sizeof (Complex));
}

//
//	While DAB symbols do not carry pilots, it is known that
//	arg (carrier [i, j] * conj (carrier [i + 1, j])
//	should be K * M_PI / 4,  (k in {1, 3, 5, 7}) so basically
//	carriers in decoded symbols can be used as if they were pilots
//
//	so, with that in mind we experiment with formula 5.39
//	and 5.40 from "OFDM Baseband Receiver Design for Wireless
//	Communications (Chiueh and Tsai)"
float	ofdmDecoder::compute_timeOffset (Complex *r, Complex *v) {
Complex sum	= Complex (0, 0);

	for (int i = -carriers / 2; i < carriers / 2; i += 6) {
	   int index_1 = i < 0 ? i + T_u : i;
	   int index_2 = (i + 1) < 0 ? (i + 1) + T_u : (i + 1);
	   Complex s = r [index_1] * conj (v [index_2]);
	   s = Complex (abs (real (s)), abs (imag (s)));
	   Complex leftTerm = s * conj (Complex (abs (s) / sqrt (2),
	                                                 abs (s) / sqrt (2)));
	   s = r [index_2] * conj (v [index_2]);
	   s = Complex (abs (real (s)), abs (imag (s)));
	   Complex rightTerm = s * conj (Complex (abs (s) / sqrt (2),
	                                                 abs (s) / sqrt (2)));
	   sum += conj (leftTerm) * rightTerm;
	}

	return arg (sum);
}
//
//	Ideally, the processed carrier should have a value
//	equal to (2 * k + 1) * PI / 4
//	The offset is a measure of the frequency "error"
float	ofdmDecoder::compute_frequencyOffset (Complex *r, Complex *c) {
Complex theta = Complex (0, 0);

	for (int i = - carriers / 2; i < carriers / 2; i += 6) {
	   int index = i < 0 ? i + T_u : i;
	   Complex val = r [index] * conj (c [index]);
	   val		= Complex (abs (real (val)), abs (imag (val)));
	   theta	+= val * Complex (1, -1);
	}

	float uu =  arg (theta) / (2 * M_PI) * SAMPLERATE / T_u;
	vv	= 0.9 * vv + 0.1 * abs (uu);;
	return vv;
}

void	ofdmDecoder::handle_decoderSelector	(int decoder) {
	this	-> decoder	= decoder;
}

void	ofdmDecoder::setIq	(bool on, int rateHz) {
	if (rateHz < 1) rateHz = 1;
	if (rateHz > 10) rateHz = 10;
	iqIntervalMs. store (1000 / rateHz);
	if (on && !iqOn. load ())
	   iqNext = std::chrono::steady_clock::time_point {};
	iqOn. store (on);
}

//	Konstellation von Symbol 2 aus conjVector (Decoder 3 legt dort die auf
//	den Einheitskreis normierten Differenzwerte ab, Decoder 1/2/4 die
//	unnormierten; v1 teilte durch die groesste Amplitude – hier je Traeger
//	normiert, damit alle Decoder dasselbe Bild liefern).
void	ofdmDecoder::emitIq	() {
//	Symbol 2 kommt alle 96 ms; die Deadline wird fortgeschrieben (nicht
//	"jetzt + Intervall"), damit die mittlere Rate rateHz erreicht und nicht
//	durch die Rahmenquantisierung unterschritten wird. Nach einer Pause
//	(kein Sync) kein Nachholen.
	auto now = std::chrono::steady_clock::now ();
	if (now < iqNext)
	   return;
	const auto interval = std::chrono::milliseconds (iqIntervalMs. load ());
	if (iqNext + interval < now)
	   iqNext = now;
	iqNext += interval;
	iqOut. resize (2 * carriers);
	int n = 0;
	for (int k = - carriers / 2; k <= carriers / 2; k ++) {
	   if (k == 0)
	      continue;
	   int index = k < 0 ? k + T_u : k;
	   Complex v = conjVector [index];
	   DABFLOAT a = jan_abs (v);
	   if (a > 1.0e-6f)
	      v = v / a;
	   else
	      v = Complex (0, 0);
	   iqOut [n ++] = (int8_t)std::lround (std::clamp (real (v) * 127.0f, -127.0f, 127.0f));
	   iqOut [n ++] = (int8_t)std::lround (std::clamp (imag (v) * 127.0f, -127.0f, 127.0f));
	}
	iqHook (iqOut);
}
//
//	The various actual decoders
static inline
Complex toQ1	(const Complex f) {
	return Complex (real (f) >= 0 ? real (f) : - real (f),
	                imag (f) >= 0 ? imag (f) : - imag (f));
}

DABFLOAT ofdmDecoder::decoder_12 (const std::vector<Complex> &fft_buffer,
	                        std::vector<int16_t> &softbits,
	                        DABFLOAT	snr,
	                        int		decType,
	                        float		clockError) {
DABFLOAT sum	= 0;
DABFLOAT corrFact	= (decType == 1) ? 1.5 : 1.0;
DABFLOAT levelFact	= (decType == 1) ? 100.0 : 60.0;
	(void)clockError;

//
//	Note for the reader
//	Some cheap dabsticks show a - sometime pretty large - clock offset.
//	indicating that the samplerate is slightly off.
//	We show the samplerate offset already for a long time,
//	old-dab suggested - in a version derived from Qt-DAB - to compensate
//	for the error
//	If the clockerror is N, then the relatieve error increase between
//	two successive block is app N / 750, the phase of that error
//	is computed. Of course higher frequencies suffer more than lower
//	frequencies, so old-dab had a scheme where, depending the
//	relative frequency of the bin, the correction was stronger

	for (int i = 0; i < carriers; i ++) {
//	here we really start
	   int16_t	index		= myMapper.  mapIn (i);
	   if (index < 0)
	      index += T_u;

	   Complex current	= fft_buffer [index];
	   Complex prevS	= phaseReference [index];
	   Complex fftBin	= current * normalize (conj (prevS));
//
//	correction on the fftBin value using the approach from
//	old-dab, see text above

	   conjVector [index]	= fftBin;
	   DABFLOAT binAbsLevel	= jan_abs (fftBin);
//	updates

	   Complex fftBin_at_1	= toQ1 (fftBin);

	   meanLevelVector [index] =
	        compute_avg (meanLevelVector [index], binAbsLevel, ALPHA);

	   DABFLOAT d_x		=  real (fftBin_at_1) -
	                                  meanLevelVector [index] / sqrt_2;
	   DABFLOAT d_y		=  imag (fftBin_at_1) -
	                                  meanLevelVector [index] / sqrt_2;
	   DABFLOAT sigmaSQ	= d_x * d_x + d_y * d_y;
	   sigmaSQ_Vector [index] =
	             compute_avg (sigmaSQ_Vector [index], sigmaSQ, ALPHA);
//
	   DABFLOAT corrector	=
	         corrFact *  meanLevelVector [index] / sigmaSQ_Vector [index];
	   corrector		/= (1 / snr + 2);
	   Complex R1	= corrector * normalize (fftBin) *
	                     (DABFLOAT)(sqrt (binAbsLevel * jan_abs (prevS)));
	   DABFLOAT scaler	=  levelFact / meanValue;

	   DABFLOAT leftBit	= - real (R1) * scaler;
	   limit_symmetrically (leftBit, MAX_VITERBI);
	   softbits [i]		= (int16_t)leftBit;

	   DABFLOAT rightBit	= - imag (R1) * scaler;
	   limit_symmetrically (rightBit, MAX_VITERBI);
	   softbits [i + carriers]	= (int16_t)rightBit;
	   sum			+= jan_abs (R1);
	}
	return sum;
}


DABFLOAT ofdmDecoder::decoder_3 (const std::vector<Complex> &fft_buffer,
	                        std::vector<int16_t> &softbits,
	                        DABFLOAT	snr,
	                        float		clockError,
	                        bool		updateDisplay) {
DABFLOAT	sum = 0;
DABFLOAT	scaler	= 140.0f / meanValue;
	(void)snr; (void)clockError;

	for (int i = 0; i < carriers; i ++) {
	   int16_t	index		= myMapper.  mapIn (i);
	   if (index < 0)
	      index += T_u;

	   Complex current	= fft_buffer [index];
	   Complex prevS	= phaseReference [index];
//
//	R1 = current * normalize(conj(prevS)) * jan_abs(prevS)
//	Since normalize divides by |prevS| and we multiply by |prevS|,
//	these cancel out: R1 = current * conj(prevS)
	   Complex R1	= current * conj (prevS);

	   if (updateDisplay)
	      conjVector [index] = R1 / jan_abs (R1);

	   DABFLOAT leftBit	= - real (R1) * scaler;
	   limit_symmetrically (leftBit, MAX_VITERBI);
	   softbits [i]		= (int16_t)leftBit;

	   DABFLOAT rightBit	= - imag (R1) * scaler;
	   limit_symmetrically (rightBit, MAX_VITERBI);
	   softbits [i + carriers]   = (int16_t)rightBit;
	   sum += jan_abs (R1);
	}
	return sum;
}

DABFLOAT ofdmDecoder::decoder_4 (const std::vector<Complex> &fft_buffer,
	                        std::vector<int16_t> &softbits,
	                        DABFLOAT	snr) {
DABFLOAT sum	= 0;
	(void)snr;

	for (int i = 0; i < carriers; i ++) {
	   int16_t	index	= myMapper.  mapIn (i);
	   if (index < 0)
	      index += T_u;
	   Complex current	= fft_buffer [index];
	   Complex prevS	= phaseReference [index];
	   Complex fftBin	= current * normalize (conj (prevS));
	   conjVector [index]	= fftBin;
	   DABFLOAT binAbsLevel	= jan_abs (fftBin);
//
//	updates

	   Complex fftBin_at_1	= Complex (abs (real (fftBin)),
	                                   abs (imag (fftBin)));

	   meanLevelVector [index] =
	        compute_avg (meanLevelVector [index], binAbsLevel, ALPHA);

	   DABFLOAT d_x		=  abs (real (fftBin_at_1)) -
	                                  meanLevelVector [index] / sqrt_2;
	   DABFLOAT d_y		=  abs (imag (fftBin_at_1)) -
	                                  meanLevelVector [index] / sqrt_2;
	   DABFLOAT sigmaSQ	= d_x * d_x + d_y * d_y;
	   sigmaSQ_Vector [index] =
	             compute_avg (sigmaSQ_Vector [index], sigmaSQ, ALPHA);
//
	   DABFLOAT A	= 1.0 / sigmaSQ_Vector [index];
	   DABFLOAT P1	= makeA (1, current, prevS) * A;
	   DABFLOAT P7	= makeA (7, current, prevS) * A;
	   DABFLOAT P3	= makeA (3, current, prevS) * A;
	   DABFLOAT P5	= makeA (5, current, prevS) * A;

	   DABFLOAT IO_P1 = IO (P1);
	   DABFLOAT IO_P7 = IO (P7);
	   DABFLOAT IO_P3 = IO (P3);
	   DABFLOAT IO_P5 = IO (P5);

	   DABFLOAT F1	= (IO_P1 + IO_P7) / (IO_P3 + IO_P5);
	   DABFLOAT F2	= (IO_P1 + IO_P3) / (IO_P5 + IO_P7);
	   if (std::isinf (F1))
	      F1 = 10.0;
	   if (std::isinf (F2))
	      F2 = 10.0;
	   if (F1 < 0.01)
	      F1 = 0.01;
	   if (F2 < 0.01)
	      F2 = 0.01;
	   DABFLOAT b1 = log (F1);
	   DABFLOAT b2 = log (F2);

	   if (std::isnan (b1))
	      b1 = 0;
	   if (std::isnan (b2))
	      b2 = 0;
	   DABFLOAT scaler   =  100.0 / meanValue;

	   DABFLOAT leftBit	=  - b1 * scaler;
	   limit_symmetrically (leftBit, MAX_VITERBI);
	   softbits [i]	= (int16_t)leftBit;

	   DABFLOAT rightBit	=  - b2 * scaler;
	   limit_symmetrically (rightBit, MAX_VITERBI);
	   softbits [carriers + i] = (int16_t)rightBit;
	   sum		+= abs (Complex (b1, b2));
	}
	return sum;
}
