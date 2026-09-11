// DAB Classic v3: portiert aus Qt-DAB sources/backend/audio/fdk-aac.cpp
// (Jan van Katwijk, GPLv2+), siehe fdk-aac.h. Dazu die Fabrik
// createAacDecoder / availableAacDecoders fuer beide Decoder.
#
/*
 *    Copyright (C) 2020 .. 2024
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
 *
 *    Use the fdk-aac library.
 */
#include	"aac-decoder.h"
#include	"faad-decoder.h"
#include	"fdk-aac.h"
#include	<cstring>
#include	<cstdio>
#include	<mutex>

#ifdef	DABCORE_FDK_AAC_RUNTIME
#ifdef	_WIN32
#define WIN32_LEAN_AND_MEAN
#include	<windows.h>
#else
#include	<dlfcn.h>
#endif

static FdkAacApi	theApi;
static bool		apiTried	= false;
static std::mutex	apiM;

//	Die DLL liegt neben der EXE (core/-Ordner); LoadLibrary sucht dort
//	zuerst, danach im PATH.
const FdkAacApi	*fdkAacApi () {
	std::lock_guard<std::mutex> lk (apiM);
	if (apiTried)
	   return theApi. loaded () ? &theApi : nullptr;
	apiTried = true;
	memset (&theApi, 0, sizeof (theApi));
#ifdef	_WIN32
	HMODULE h = LoadLibraryA ("libfdk-aac-2.dll");
	if (h == nullptr)
	   return nullptr;
	auto sym = [h] (const char *n) { return (void *)GetProcAddress (h, n); };
#else
	void *h = dlopen ("libfdk-aac.so.2", RTLD_NOW);
	if (h == nullptr)
	   return nullptr;
	auto sym = [h] (const char *n) { return dlsym (h, n); };
#endif
	FdkAacApi a;
	a. open		= (decltype (a. open)) sym ("aacDecoder_Open");
	a. close	= (decltype (a. close)) sym ("aacDecoder_Close");
	a. fill		= (decltype (a. fill)) sym ("aacDecoder_Fill");
	a. decodeFrame	= (decltype (a. decodeFrame)) sym ("aacDecoder_DecodeFrame");
	a. getStreamInfo = (decltype (a. getStreamInfo)) sym ("aacDecoder_GetStreamInfo");
	if (!a. open || !a. close || !a. fill || !a. decodeFrame || !a. getStreamInfo) {
	   fprintf (stderr, "libfdk-aac-2: Symbole fehlen, Decoder nicht verfuegbar\n");
	   return nullptr;
	}
	theApi = a;
	return &theApi;
}

//
/**
  *	For interpreting the HeAAC frames we have the faad decoder
  *	and the fdk-aac decoder
  */
	fdkAAC::fdkAAC (PcmSink s): sink (std::move (s)) {
	working			= false;
	api			= fdkAacApi ();
	if (api == nullptr)
	   return;
	handle			= api -> open (TT_MP4_LOAS, 1);
	if (handle == nullptr)
	   return;
	working			= true;
}

	fdkAAC::~fdkAAC () {
	if (working)
	   api -> close (handle);
}

int16_t	fdkAAC::MP42PCM (stream_parms *sp,
	                 uint8_t   packet [],
	                 int16_t   packetLength) {
uint32_t	packet_size;
AAC_DECODER_ERROR err;
uint8_t		*ptr	= packet;
static thread_local INT_PCM decode_buf [8 * sizeof (INT_PCM) * 2048];
INT_PCM		*bufp	= &decode_buf [0];
int		output_size	= 8 * 2048;

	if (!working)
	   return -1;

	if ((packet [0] != 0x56)  || ((packet [1] >> 5) != 7))
	   return -1;

	packet_size  = (((packet [1] & 0x1F) << 8) | packet [2]) + 3;
	if (packet_size != (uint32_t)packetLength)
	   return -1;

	uint32_t	valid_size = packet_size;
	err = api -> fill (handle, &ptr, &packet_size, &valid_size);
	if (err != AAC_DEC_OK)
	   return -1;

	err = api -> decodeFrame (handle,
	                              bufp,
		                      output_size, 0);
	if (err == AAC_DEC_NOT_ENOUGH_BITS)
	   return -1;

	if (err != AAC_DEC_OK)
	   return -1;

	CStreamInfo *info = api -> getStreamInfo (handle);
	if (!info || info -> sampleRate <= 0)
	   return -1;

	if (info -> numChannels == 2) {		// default for DAB+
	   pcmBuffer. resize (info -> frameSize);
	   for (int i = 0; i < info -> frameSize; i ++)
	      pcmBuffer [i] = complex16 (bufp [2 * i], bufp [2 * i + 1]);
	   if (sink)
	      sink (pcmBuffer. data (), info -> frameSize, info -> sampleRate,
	            sp -> psFlag != 0, sp -> sbrFlag != 0);
	}
	else
	if (info -> numChannels == 1) {
	   pcmBuffer. resize (info -> frameSize);
	   for (int i = 0; i < info -> frameSize; i ++)
	      pcmBuffer [i] = complex16 (((int16_t *)bufp) [i], ((int16_t *)bufp) [i]);
	   if (sink)
	      sink (pcmBuffer. data (), info -> frameSize, info -> sampleRate,
	            sp -> psFlag != 0, sp -> sbrFlag != 0);
	}
	else
	   fprintf (stderr, "Cannot handle these channels\n");

	return info -> numChannels;
}
#endif	// DABCORE_FDK_AAC_RUNTIME

// --- Fabrik ---------------------------------------------------------------

std::vector<std::string> availableAacDecoders () {
	std::vector<std::string> r;
#ifdef	__WITH_FAAD__
	r. push_back ("faad2");
#endif
#ifdef	DABCORE_FDK_AAC_RUNTIME
	if (fdkAacApi () != nullptr)
	   r. push_back ("fdk-aac");
#endif
	return r;
}

AacDecoderKind aacDecoderKindFromName (const std::string &n) {
	if (n == "faad2" || n == "faad")
	   return AacDecoderKind::Faad2;
	if (n == "fdk" || n == "fdk-aac")
	   return AacDecoderKind::Fdk;
	return AacDecoderKind::Auto;
}

std::unique_ptr<IAacDecoder> createAacDecoder (AacDecoderKind kind, PcmSink sink) {
#ifdef	DABCORE_FDK_AAC_RUNTIME
	if (kind != AacDecoderKind::Faad2 && fdkAacApi () != nullptr)
	   return std::make_unique<fdkAAC> (std::move (sink));
#endif
#ifdef	__WITH_FAAD__
	if (kind != AacDecoderKind::Fdk)
	   return std::make_unique<faadDecoder> (std::move (sink));
#endif
	(void)kind;
	return nullptr;
}
