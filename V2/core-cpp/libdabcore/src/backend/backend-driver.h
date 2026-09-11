// DAB Classic v3: portiert aus Qt-DAB sources/backend/backend-driver.h
// (Jan van Katwijk, GPLv2+): RadioInterface/logger/RingBuffer/FILE-Dump
// -> BackendCallbacks, QScopedPointer -> std::unique_ptr, MP2 entfaellt
// (Entscheidung 19).
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

#pragma once

#include	<atomic>
#include	<memory>
#include	<vector>
#include	<utility>
#include	"dab-constants.h"
#include	"frame-processor.h"
#include	"backend-callbacks.h"
#include	"aac-decoder.h"

class	backendDriver {
public:
	backendDriver	(const descriptorType *,
	                 BackendCallbacks *,
	                 AacDecoderKind);
	~backendDriver	();
void	addtoFrame	(const std::vector<uint8_t> &outData);
void	stop		();
private:
	std::atomic<bool> running;
	std::unique_ptr<frameProcessor>	theProcessor;
};
