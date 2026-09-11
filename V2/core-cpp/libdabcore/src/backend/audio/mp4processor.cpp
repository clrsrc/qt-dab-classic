// DAB Classic v3: portiert aus Qt-DAB sources/backend/audio/mp4processor.cpp
// (Jan van Katwijk, GPLv2+), siehe mp4processor.h.
#
/*
 *    Copyright (C) 2014 .. 2025
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
 ************************************************************************
 *	may 15 2015. A real improvement on the code
 *	is the addition from Stefan Poeschel to create a
 *	header for the aac that matches, really a big help!!!!
 *
 *	(2019:)Furthermore, the code in the "build_aacFile" function is
 *	his as well. Chapeau!
 ************************************************************************
 */
#include	"mp4processor.h"

#include	"crc-handlers.h"
#include	<cstring>
#include	"charsets.h"
#include	"pad-handler.h"
#include	"bitWriter.h"

//
/**
  *	\class mp4Processor is the main handler for the aac frames
  *	the class proper processes input and extracts the aac frames
  *	that are processed by the "faadDecoder" class
  */
	mp4Processor::mp4Processor (uint32_t		SId,
	                            int16_t		bitRate,
	                            BackendCallbacks	*cb,
	                            AacDecoderKind	aacKind):
	                                cb (cb),
	                                my_padhandler (SId, cb),
 	                                my_rsDecoder (8, 0435, 0, 1, 10) {
	this	-> bitRate	= bitRate;	// input rate
	this	-> stereo	= false;
//	Der Decoder liefert PCM direkt an den Backend-Callback; die
//	Stereo-Kennung kommt aus den Superframe-Parametern (v1 isStereo).
	aacDecoder = createAacDecoder (aacKind,
	               [this] (const complex16 *pcm, int n, int rate, bool ps, bool sbr) {
	                  emitBe (this -> cb -> pcm, pcm, n, rate, ps, sbr, this -> stereo);
	               });
	if (aacDecoder)
	   emitBe (cb -> log, "info", std::string ("AAC-Decoder: ") + aacDecoder -> name ());
	else
	   emitBe (cb -> log, "error", "kein AAC-Decoder verfuegbar");

	superFramesize		= 110 * (bitRate / 8);
	RSDims			= bitRate / 8;
	frameBytes. resize (RSDims * 120);	// input
	outVector . resize (RSDims * 110);
	blockFillIndex		= 0;
	blocksInBuffer		= 0;
	frameCount		= 0;
	frameErrors		= 0;
	aacErrors		= 0;
	crcErrors		= 0;
	aacFrames		= 0;
	successFrames		= 0;
	rsErrors		= 0;
	totalCorrections	= 0;
	goodFrames		= 0;
	statFrameErrors		= 0;
	statRsErrors		= 0;
	statAacErrors		= 0;
	statRsCorrections	= 0;
	statFrames		= 0;
	stopWorking. store (false);
}

	mp4Processor::~mp4Processor () {
	stop ();
}

//	Fehlerzaehler etwa 1 Hz melden (je 42 DAB-Rahmen = 1,008 s Sendezeit,
//	damit auch bei --fast deterministisch); Zaehler danach zuruecksetzen.
void	mp4Processor::reportStats () {
	if (++statFrames < 42)
	   return;
	statFrames = 0;
	emitBe (cb -> stats, statFrameErrors, statRsErrors,
	                     statAacErrors, statRsCorrections);
	statFrameErrors = statRsErrors = statAacErrors = statRsCorrections = 0;
}
/**
  *	\brief addtoFrame
  *
  *	a DAB+ superframe consists of 5 consecutive DAB frames
  *	we add vector for vector to the superframe. Once we have
  *	5 lengths of "old" frames, we check
  *	Note that the packing in the entry vector is still one bit
  *	per Byte, nbits is the number of Bits (i.e. containing bytes)
  *	the function adds nbits bits, packed in bytes, to the frame
  */
void	mp4Processor::addtoFrame (const std::vector<uint8_t> &V) {
int16_t	nbits	= 24 * bitRate;
int16_t nbytes	= nbits / 8;
uint8_t	temp	= 0;

	if (stopWorking)
	   return;
	for (int i = 0; i < nbytes; i ++) {	// in bytes
	   temp = 0;
	   for (int j = 0; j < 8; j ++)
	      temp = (temp << 1) | (V [i * 8 + j] & 01);
	   frameBytes [blockFillIndex * nbytes + i] = temp;
	}
//
	blocksInBuffer ++;
	blockFillIndex = (blockFillIndex + 1) % 5;
	frameCount ++;
	reportStats ();
//
/**
  *	we take the last five blocks to look at
  */
	if (blocksInBuffer < 5) {
	   return;
	}
//
//	The buffer is filled enough, let's process
///	first, we show the number of successful frames
	if (++frameCount >= 25) {
	   frameCount = 0;
	   frameErrors = 0;
	}
//
//	Thanks to the "healing"  checkAndCorrect function, we
//	do not need to apply a RS before testing
	if (!fc. checkAndCorrect (&frameBytes [blockFillIndex * nbytes])) {
	   blocksInBuffer = 4;
	   statFrameErrors ++;
	   return;
	}
//
	if (!handleRS (frameBytes. data (), blockFillIndex * nbytes,
	              outVector, frameErrors, rsErrors)) {
	   blocksInBuffer = 4;
	   statFrameErrors ++;
	   statRsErrors += frameErrors;
	   return;
	}
	statRsCorrections += rsErrors;

	blocksInBuffer = 0;
	processSuperframe (frameBytes. data (), blockFillIndex * nbytes);
	if (++successFrames > 25) {
	      successFrames	= 0;
	      rsErrors	= 0;
	}	// end of ... >= 5
}
//
//	when handling a superframe we want to be sure that the fc is correct
bool	mp4Processor::processSuperframe (uint8_t *frameBytes,
	                                           int16_t  index) {
uint8_t		num_aus;
int		tmp;
stream_parms    streamParameters;
	(void)frameBytes; (void)index;
//	bits 0 .. 15 is firecode
//	bit 16 is unused
	streamParameters. dacRate = (outVector [2] >> 6) & 01;	// bit 17
	streamParameters. sbrFlag = (outVector [2] >> 5) & 01;	// bit 18
	streamParameters. aacChannelMode
	                          = (outVector [2] >> 4) & 01;	// bit 19
	streamParameters. psFlag  = (outVector [2] >> 3) & 01;	// bit 20
	streamParameters. mpegSurround	= (outVector [2] & 07);	// bits 21 .. 23
//
//	added for the aac file writer
	streamParameters. CoreSrIndex	=
	              streamParameters. dacRate ?
	                            (streamParameters. sbrFlag ? 6 : 3) :
	                            (streamParameters. sbrFlag ? 8 : 5);
	streamParameters. CoreChConfig	=
	              streamParameters. aacChannelMode ? 2 : 1;

	streamParameters. ExtensionSrIndex =
	              streamParameters. dacRate ? 3 : 5;

	switch (2 * streamParameters. dacRate + streamParameters. sbrFlag) {
	  default:		// cannot happen
	   case 0:
	      num_aus = 4;
	      au_start [0] = 8;
	      au_start [1] = outVector [3] * 16 + (outVector [4] >> 4);
	      au_start [2] = (outVector [4] & 0xf) * 256 +
	                      outVector [5];
	      au_start [3] = outVector [6] * 16 +
	                     (outVector [7] >> 4);
	      au_start [4] = 110 *  (bitRate / 8);
	      break;
//
	   case 1:
	      num_aus = 2;
	      au_start [0] = 5;
	      au_start [1] = outVector [3] * 16 +
	                     (outVector [4] >> 4);
	      au_start [2] = 110 *  (bitRate / 8);
	      break;
//
	   case 2:
	      num_aus = 6;
	      au_start [0] = 11;
	      au_start [1] = outVector [3] * 16 + (outVector [4] >> 4);
	      au_start [2] = (outVector [4] & 0xf) * 256 + outVector [ 5];
	      au_start [3] = outVector [6] * 16 + (outVector [7] >> 4);
	      au_start [4] = (outVector [7] & 0xf) * 256 + outVector [8];
	      au_start [5] = outVector [9] * 16 + (outVector [10] >> 4);
	      au_start [6] = 110 *  (bitRate / 8);
	      break;
//
	   case 3:
	      num_aus = 3;
	      au_start [0] = 6;
	      au_start [1] = outVector [3] * 16 + (outVector [4] >> 4);
	      au_start [2] = (outVector [4] & 0xf) * 256 + outVector [5];
	      au_start [3] = 110 * (bitRate / 8);
	      break;
	}
/**
  *	OK, the result is N * 110 * 8 bits (still single bit per byte!!!)
  *	extract the AU's, and prepare a buffer,  with the sufficient
  *	lengthy for conversion to PCM samples
  */
	for (int i = 0; i < num_aus; i ++) {
	   int16_t	aac_frame_length;

///	sanity check 1
	   if (au_start [i + 1] < au_start [i]) {
//	should not happen, all errors were corrected
	      return false;
	   }

	   aac_frame_length = au_start [i + 1] - au_start [i] - 2;
//	just a sanity check
	   if ((aac_frame_length >=  960) || (aac_frame_length < 0)) {
	      return false;
	   }

//	but first the crc check
	   if (!check_crc_bytes (&outVector [au_start [i]],
	                                aac_frame_length)) {
	      crcErrors ++;
	      statAacErrors ++;
	      return true;
	   }
//
//	=====================================================================
//	ANZAPFPUNKT (Timeshift / AAC-Passthrough / Frame-Dump):
//	&outVector [au_start [i]] .. + aac_frame_length ist die rohe, CRC-
//	geprüfte AAC-Zugriffseinheit (960-Sample-Frame) dieses Superframes;
//	streamParameters beschreibt Abtastrate/SBR/PS/Kanalmodus.
//	build_aacFile rahmt sie als LOAS/LATM (abspielbarer .aac-Strom), das
//	geht als aacFrame-Callback hinaus (v1: frameBuffer + Signal newFrame
//	bzw. fwrite in den Hintergrund-Dump).
//	=====================================================================
	   std::vector<uint8_t> fileBuffer;
	   int segmentSize =
	              build_aacFile (aac_frame_length,
	                             &streamParameters,
	                             &(outVector [au_start [i]]),
	                             fileBuffer);
	   emitBe (cb -> aacFrame, fileBuffer. data (), segmentSize);

//	first handle the pad data if any. v1 tat das nur fuer Vordergrund-
//	dienste; v3 wertet PAD fuer alle Slots aus (DLS/DL+ auch im Hintergrund).
	   if (((outVector [au_start [i + 0]] >> 5) & 07) == 4)
	      handle_PAD (outVector, au_start [i]);
//
//	then handle the audio
	   stereo = (streamParameters. aacChannelMode == 1) ||
	            (streamParameters. psFlag == 1);
	   if (aacDecoder == nullptr)
	      tmp = -1;
	   else
	   if (aacDecoder -> takesLoas ()) {
	      tmp = aacDecoder -> MP42PCM (&streamParameters,
	                                      fileBuffer. data (),
	                                      segmentSize);
	   }
	   else {
	      uint8_t theAudioUnit [2 * 960 + 10];	// sure, large enough
	      memcpy (theAudioUnit,
	                 &outVector [au_start [i]], aac_frame_length);
	      memset (&theAudioUnit [aac_frame_length], 0, 10);
	      tmp = aacDecoder -> MP42PCM (&streamParameters,
	                                      theAudioUnit,
	                                      aac_frame_length);
	   }

	   if (tmp <= 0) {
	      aacErrors ++;
	      statAacErrors ++;
	   }
	   if (++aacFrames > 25) {
	      aacErrors	= 0;
	      aacFrames	= 0;
	   }
//
//	what would happen if the errors were in the 10 parity bytes
//	rather than in the 110 payload bytes?
	}
	return true;
}

bool	mp4Processor::handleRS (const uint8_t *frameBytes,
	                        int16_t base,
	                        std::vector<uint8_t> &outVector,
	                        int16_t &errorLines, int16_t &repairs) {
uint8_t		rsIn	[120];
uint8_t		rsOut	[110];
int16_t		ler;

	errorLines	= 0;
	repairs		= 0;
/**
  *	apply reed-solomon error repar
  *	OK, what we now have is a vector with RSDims * 120 uint8_t's
  *	the superframe, containing parity bytes for error repair
  *	take into account the interleaving that is applied.
  */
	for (int j = 0; j < RSDims; j ++) {
	   for (int k = 0; k < 120; k ++)
	      rsIn [k] = frameBytes [(base + j + k * RSDims) % (RSDims * 120)];
	   ler = my_rsDecoder. dec (rsIn, rsOut, 135);
	   for (int k = 0; k < 110; k ++)
	      outVector [j + k * RSDims] = rsOut [k];
	   if (ler < 0) {
	      errorLines ++;
	   }
	   else {
	      repairs += ler;
	   }
	}

	return errorLines == 0;
}

int	mp4Processor::build_aacFile (int16_t aac_frame_len,
	                             stream_parms *sp,
	                             uint8_t *data,
	                             std::vector<uint8_t> &fileBuffer) {
BitWriter	au_bw;

	au_bw. AddBits (0x2B7, 11);	// syncword
	au_bw. AddBits (    0, 13);	// audioMuxLengthBytes - written later
//	AudioMuxElement(1)

	au_bw. AddBits (    0, 1);	// useSameStreamMux
//	StreamMuxConfig()

	au_bw. AddBits (    0, 1);	// audioMuxVersion
	au_bw. AddBits (    1, 1);	// allStreamsSameTimeFraming
	au_bw. AddBits (    0, 6);	// numSubFrames
	au_bw. AddBits (    0, 4);	// numProgram
	au_bw. AddBits (    0, 3);	// numLayer

	if (sp  -> sbrFlag) {
	   au_bw. AddBits (0b00101, 5); // SBR
	   au_bw. AddBits (sp -> CoreSrIndex, 4); // samplingFrequencyIndex
	   au_bw. AddBits (sp -> CoreChConfig, 4); // channelConfiguration
	   au_bw. AddBits (sp -> ExtensionSrIndex, 4);	// extensionSamplingFrequencyIndex
	   au_bw. AddBits (0b00010, 5);		// AAC LC
	   au_bw. AddBits (0b100, 3);	// GASpecificConfig() with 960 transform
	} else {
	   au_bw. AddBits (0b00010, 5); // AAC LC
	   au_bw. AddBits (sp -> CoreSrIndex, 4); // samplingFrequencyIndex
	   au_bw. AddBits (sp -> CoreChConfig, 4); // channelConfiguration
	   au_bw. AddBits (0b100, 3);	// GASpecificConfig() with 960 transform
}

	au_bw. AddBits (0b000, 3);	// frameLengthType
	au_bw. AddBits (0xFF, 8);	// latmBufferFullness
	au_bw. AddBits (   0, 1);	// otherDataPresent
	au_bw. AddBits (   0, 1);	// crcCheckPresent

//	PayloadLengthInfo()
	for (size_t i = 0; i < (size_t)(aac_frame_len / 255); i++)
	   au_bw. AddBits (0xFF, 8);
	au_bw. AddBits (aac_frame_len % 255, 8);

	au_bw. AddBytes (data, aac_frame_len);
	au_bw. WriteAudioMuxLengthBytes ();
	fileBuffer	= au_bw. GetData ();
	return fileBuffer. size ();
}

void    mp4Processor::handle_PAD (const std::vector<uint8_t> &v,
                                          int startIndex) {
int16_t count = v [startIndex + 1];
uint8_t *buffer = dynVec (uint8_t, count);
        memcpy (buffer, &v [startIndex + 2], count);
        uint8_t L0  = buffer [count - 1];
        uint8_t L1  = buffer [count - 2];
        my_padhandler. processPAD (buffer, count - 3, L1, L0);
}

void	mp4Processor::stop	() {
	stopWorking. store (true);
}
