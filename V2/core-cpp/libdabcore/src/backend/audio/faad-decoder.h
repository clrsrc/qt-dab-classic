// DAB Classic v3: portiert aus Qt-DAB sources/backend/audio/faad-decoder.h
// (Jan van Katwijk, GPLv2+): QObject/Signal newAudio/RingBuffer -> PcmSink
// (IAacDecoder). Initialisierung (ASC mit 960-Transform) und Dekodierung
// unveraendert.
#
/*
 *    Copyright (C) 2014 .. 2017
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
#pragma once
#ifdef	__WITH_FAAD__

#include	"dab-constants.h"
#include        <neaacdec.h>
#include	"aac-decoder.h"

class	faadDecoder : public IAacDecoder {
public:
        faadDecoder     (PcmSink sink);
        ~faadDecoder	() override;
int16_t	 MP42PCM         (stream_parms *sp,
                         uint8_t buffer [],
                         int16_t bufferLength) override;
bool	takesLoas	() const override { return false; }
const char *name	() const override { return "faad2"; }
private:
bool    initialize      (stream_parms *);

        bool            processorOK;
        bool            aacInitialized;
        uint32_t        aacCap;
        NeAACDecHandle  aacHandle;
        NeAACDecConfigurationPtr        aacConf;
        NeAACDecFrameInfo       hInfo;
        int32_t         baudRate;
	PcmSink		sink;
	std::vector<complex16>	pcmBuffer;
};
#endif
