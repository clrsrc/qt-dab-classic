// DAB Classic v3: portiert aus Qt-DAB sources/backend/audio/mp4processor.h
// (Jan van Katwijk, GPLv2+): QObject und die Signale show_frameErrors,
// show_rsErrors, show_aacErrors, isStereo, newFrame, show_rsCorrections
// -> BackendCallbacks (stats ~1 Hz, aacFrame, pcm); Decoder ueber
// IAacDecoder (faad2 oder FDK zur Laufzeit) statt Compile-Zeit-Auswahl.
// Superframe-Sync, Firecode, Reed-Solomon und AU-Extraktion unveraendert.
#
/*
 *    Copyright (C) 2014 .. 2017
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *    Lazy Chair Computing
 *
 *    This file is part of the Qt-DAB.
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
 *    You should have received a copy of the GNU General Public License
 *    along with Qt-DAB; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#
#pragma once
/*
 * 	Handling superframes for DAB+ and delivering
 * 	frames into the ffmpeg or faad decoding library
 */
//
#include	"dab-constants.h"
#include	<cstdio>
#include	<cstdint>
#include	<vector>
#include	<memory>
#include	<chrono>
#include	<atomic>
#include	"frame-processor.h"
#include	"firecode-checker.h"
#include	"reed-solomon.h"
#include	"pad-handler.h"
#include	"aac-decoder.h"
#include	"backend-callbacks.h"

class	mp4Processor final : public frameProcessor {
public:
			mp4Processor	(uint32_t,	// SId
	                                 int16_t,	// bitRate
	                                 BackendCallbacks *,
	                                 AacDecoderKind);
			~mp4Processor	() override;
	void		addtoFrame	(const std::vector<uint8_t> &) override;
	void		stop		() override;
private:
	BackendCallbacks *cb;
	padHandler	my_padhandler;
	reedSolomon	my_rsDecoder;
	std::unique_ptr<IAacDecoder>	aacDecoder;
	firecodeChecker	fc;

	std::atomic<bool>	stopWorking;
	bool		handleRS (const uint8_t *frameBytes,
	                          int16_t base,
                                  std::vector<uint8_t> &outVector,
	                          int16_t &errorLines, int16_t &repairs);

	bool		processSuperframe (uint8_t *, int16_t);
	int		build_aacFile (int16_t aac_frame_len,
                                       stream_parms *sp,
                                       uint8_t	*data,
                                       std::vector<uint8_t> &fileBuffer);

	void		handle_PAD	(const std::vector<uint8_t> &, int);
	void		reportStats	();
	int16_t		superFramesize;
	int16_t		blockFillIndex;
	int16_t		blocksInBuffer;
	int16_t         frameCount;
        int16_t         frameErrors;
        int16_t         rsErrors;
        int16_t		crcErrors;
        int16_t         aacErrors;
        int16_t         aacFrames;
        int16_t         successFrames;
	int		goodFrames;
	int		totalCorrections;
	int16_t		bitRate;
	bool		stereo;
	std::vector<uint8_t> frameBytes;
	std::vector<uint8_t> outVector;
	int16_t		RSDims;
	int16_t		au_start	[10];
//	Zaehler seit dem letzten stats-Callback (Wanduhr, ~1 Hz)
	int		statFrameErrors;
	int		statRsErrors;
	int		statAacErrors;
	int		statRsCorrections;
	int		statFrames;
};
