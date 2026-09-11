// DAB Classic v3: portiert aus Qt-DAB sources/backend/audio/fdk-aac.h
// (Jan van Katwijk, GPLv2+): QObject/Signal newAudio/RingBuffer -> PcmSink
// (IAacDecoder). Die Bibliothek wird nicht gelinkt, sondern zur Laufzeit
// per LoadLibrary ("libfdk-aac-2.dll" neben der EXE) geladen
// (Entscheidung 20); nur die Typen kommen aus aacdecoder_lib.h.
#
/*
 *    Copyright (C) 2020
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
 *	Use the fdk-aac library.
 */
#pragma once
#ifdef	DABCORE_FDK_AAC_RUNTIME

#include	<stdint.h>
#include	"dab-constants.h"
#include	<aacdecoder_lib.h>
#include	"aac-decoder.h"

//	Funktionstabelle der zur Laufzeit geladenen DLL
struct FdkAacApi {
	HANDLE_AACDECODER (*open)	(TRANSPORT_TYPE, UINT);
	void		(*close)	(HANDLE_AACDECODER);
	AAC_DECODER_ERROR (*fill)	(HANDLE_AACDECODER, UCHAR *[], const UINT [], UINT *);
	AAC_DECODER_ERROR (*decodeFrame)(HANDLE_AACDECODER, INT_PCM *, const INT, const UINT);
	CStreamInfo	*(*getStreamInfo)(HANDLE_AACDECODER);
	bool		loaded		() const { return open != nullptr; }
};

//	Laedt die DLL einmalig (thread-sicher); nullptr, wenn nicht vorhanden.
const FdkAacApi	*fdkAacApi ();

class	fdkAAC : public IAacDecoder {
public:
		fdkAAC (PcmSink sink);
		~fdkAAC	() override;

int16_t		MP42PCM (stream_parms *sp,
                         uint8_t   packet [],
                         int16_t   packetLength) override;
bool		takesLoas () const override { return true; }
const char	*name () const override { return "fdk-aac"; }
private:
	const FdkAacApi		*api;
	PcmSink			sink;
	bool			working;
	HANDLE_AACDECODER	handle;
	std::vector<complex16>	pcmBuffer;
};

#endif
