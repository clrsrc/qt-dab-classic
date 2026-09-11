// DAB Classic v3: portiert aus Qt-DAB sources/output/converter_48000.cpp
// (Jan van Katwijk, GPLv2+), siehe converter48k.h.
#
/*
 *    Copyright (C) 2011 .. 2023
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
#include	"converter48k.h"
#include	<cstdio>
/*
 */
	converter_48000::converter_48000 ():
	                                   filter_16_48 (5, 8000, 48000),
	                                   filter_24_48 (5, 12000, 89000),
	                                   filter_32_96 (5, 16000, 96000) {
	buffer_32_96. resize (0);
}

	converter_48000::~converter_48000 () {
}

int	converter_48000::convert (const complex16 *V,
	                         int32_t amount, int32_t rate,
	                         std::vector<float> 	&outB) {
	switch (rate) {
	   case 16000:
	      return convert_16000 (V, amount, outB);
	   case 24000:
	      return convert_24000 (V, amount, outB);
	   case 32000:
	      return convert_32000 (V, amount, outB);
	   default:
	   case 48000:
	      return convert_48000 (V, amount, outB);
	}
}

//	scale up from 16 -> 48
//	amount gives number of pairs
int	converter_48000::convert_16000  (const complex16 *V, int amount,
	                                 std::vector<float> &out) {
int	teller = 0;

	out. resize (2 * 3 * amount);
	for (int i = 0; i < amount; i ++) {
	   std::complex<float> X =
	                std::complex<float> (3 * real (V [i]) / 32767.0,
	                                     3 *imag (V [i]) / 32767.0);
	   X = filter_16_48. Pass (X);
	   out [teller ++] = real (X);
	   out [teller ++] = imag (X);
	   X = filter_16_48. Pass (std::complex<float> (0, 0));
	   out [teller ++] = real (X);
	   out [teller ++] = imag (X);
	   X = filter_16_48. Pass (std::complex<float> (0, 0));
	   out [teller ++] = real (X);
	   out [teller ++] = imag (X);
	}
	return teller;
}

//	scale up from 24000 -> 48000
//	amount gives number of pairs
int	converter_48000::convert_24000	(const complex16 *V,
	                                 int amount,
	                                 std::vector<float> &out) {
int teller	= 0;
	out. resize (2 * 2 * amount);
	for (int i = 0; i < amount; i ++) {
	   std::complex<float> X =
	              std::complex<float> (2 * real (V [i]) / 32767.0,
	                                   2 * imag (V [i]) / 32767.0);
	   X = filter_24_48. Pass (X);
	   out [teller ++] = real (X);
	   out [teller ++] = imag (X);
	   X = filter_24_48. Pass (std::complex<float> (0, 0));
	   out [teller ++] = real (X);
	   out [teller ++] = imag (X);
	}
	return teller;
}

//	scale up from 32000 -> 48000
//	amount is number of pairs
int	converter_48000::convert_32000	(const complex16 *V,
	                                 int amount,
	                                 std::vector<float> &out) {
int teller	= 0;
	out. resize (3 * amount);
	for (int i = 0; i < amount; i ++) {
	   std::complex<float> X =
	              std::complex<float> (3 * real (V [i]) / 32768.0,
                                           3 * imag (V [i]) / 32768.0);
	   X = filter_32_96. Pass (X);
	   buffer_32_96. push_back (X);
	   X = filter_32_96. Pass (std::complex<float> (0, 0));
	   buffer_32_96. push_back (X);
	   X = filter_32_96. Pass  (std::complex<float> (0, 0));
	   buffer_32_96. push_back (X);
	   if (buffer_32_96. size () >= 6) {	// should be 0 .. 6
	      out [teller ++] = real (buffer_32_96 [0]);
	      out [teller ++] = imag (buffer_32_96 [0]);
	      out [teller ++] = real (buffer_32_96 [2]);
	      out [teller ++] = imag (buffer_32_96 [2]);
	      out [teller ++] = real (buffer_32_96 [4]);
	      out [teller ++] = imag (buffer_32_96 [4]);
	      buffer_32_96. resize (0);
	   }
	}
	return teller;
}

int	converter_48000::convert_48000	(const complex16 *V,
	                                 int amount,
	                                 std::vector<float> & out) {
	out. resize (2 * amount);
	for (int i = 0; i < amount; i ++) {
	   out [2 * i] = real (V [i]) / 32768.0;
	   out [2 * i + 1] = imag (V [i]) / 32768.0;
	}
	return 2 * amount;
}
