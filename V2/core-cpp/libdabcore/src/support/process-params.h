// DAB Classic v3: portiert aus Qt-DAB sources/support/process-params.h
// (Jan van Katwijk, GPLv2+), Qt entfernt. Die Ringpuffer fuer die GUI-Scopes
// sind gestrichen; stattdessen tragen die Felder die v1-Defaults aus
// main/radio.cpp (QSettings DAB_GENERAL / CONFIG_HANDLER).
#
/*
 *    Copyright (C) 2016 .. 2023
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
//
#pragma once

#include	<stdint.h>
#include	"dab-constants.h"

class	processParams {
public:
	uint8_t	dabMode		= 1;		// DAB_GENERAL/dabMode
	int16_t	threshold	= 3;		// DAB_GENERAL/threshold
	int16_t	diff_length	= DIFF_LENGTH;	// DAB_GENERAL/diff_length
	int16_t	tii_delay	= 3;		// DAB_GENERAL/tii_delay (min 3)
	int16_t	tii_depth	= 4;		// DAB_GENERAL/tii_depth
	int16_t	echo_depth	= 1;		// DAB_GENERAL/echo_depth
	int16_t	bitDepth	= 8;
//	Werte, die v1 im ofdmHandler aus CONFIG_HANDLER liest
	int	decoder		= DECODER_3;	// "decoders"
	bool	correlationOrder = false;	// S_CORRELATION_ORDER
	bool	dxMode		= false;	// S_DX_MODE
	int16_t	tiiThreshold	= 6;		// TII_THRESHOLD
	int	tiiCollision	= -1;		// "tiiCollision"
	bool	dcRemoval	= false;	// configHandler::get_dcRemoval
	bool	tiiEnabled	= true;		// TII-Auswertung im Nullsymbol
};
