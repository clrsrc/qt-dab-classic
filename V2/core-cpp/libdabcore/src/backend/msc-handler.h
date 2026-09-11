// DAB Classic v3: portiert aus Qt-DAB sources/backend/msc-handler.h
// (Jan van Katwijk, GPLv2+): QObject/QMutex -> IMscSink/std::mutex,
// RadioInterface/logger/RingBuffer/FILE -> BackendCallbacks je Backend,
// Signal activeServices entfaellt. CIF-Aufbau und Verteilung an die
// Backends unveraendert. Mehrere Backends gleichzeitig (Entscheidung 24).
#
/*
 *    Copyright (C) 2025
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
#
#pragma once

#include	<mutex>
#include	<memory>
#include	<cstdint>
#include	<vector>
#include	"dab-constants.h"
#include	"dab-params.h"
#include	"msc-sink.h"
#include	"backend-callbacks.h"
#include	"aac-decoder.h"

class	Backend;

class	mscHandler: public IMscSink {
public:
			mscHandler		(uint8_t dabMode, uint8_t cpuSupport);
			~mscHandler		() override;
	void		processMscBlock		(const std::vector<int16_t> &,
	                                                  int) override;
	void		resetChannel		() override;
	void		stop			() override;
//	Backend anlegen; cb muss bis stopBackend gueltig bleiben.
	Backend		*startBackend		(const descriptorType &,
	                                         BackendCallbacks *,
	                                         int flag,
	                                         AacDecoderKind);
	void		stopBackend		(Backend *);
	bool		serviceRuns		(uint32_t SId, uint16_t subChId);
	int		activeServices		();
private:
	dabParams	params;
	uint8_t		cpuSupport;
	std::mutex	locker;
	std::vector<Backend*> theBackends;
	std::vector<int16_t> cifVector;
	int16_t		BitsperBlock;
	int16_t		numberofblocksperCIF;
};
