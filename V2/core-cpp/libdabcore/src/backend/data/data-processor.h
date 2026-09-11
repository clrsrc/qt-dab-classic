// DAB Classic v3: portiert aus Qt-DAB sources/backend/data/data-processor.h
// (Jan van Katwijk, GPLv2+): QObject/Signal show_mscErrors gestrichen,
// RadioInterface/RingBuffer -> BackendCallbacks, QScopedPointer ->
// std::unique_ptr, Paket-Tracer entfernt. Nur MOT (DSCTy 60) wird
// weitergereicht; TDC/IP/Journaline entfallen (Entscheidung 19).
// Paket-Reassembly und FEC (RS 204/188) unveraendert.
#
/*
 *    Copyright (C) 2016 .. 2025
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *    Lazy Chair Computing
 *
 *    This file is part of the Qt-DAB
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

#include	<vector>
#include	<cstdio>
#include	<cstring>
#include	<memory>
#include	"frame-processor.h"
#include	"reed-solomon.h"
#include	"mot-handler.h"
#include	"backend-callbacks.h"

class	dataProcessor final : public frameProcessor {
public:
	dataProcessor	(const packetdata	*pd,
	                 BackendCallbacks	*cb);
	~dataProcessor	() override;
void	addtoFrame	(const std::vector<uint8_t> &) override;
void	stop		() override;
private:
	BackendCallbacks *cb;
	uint32_t	SId;
	int16_t		bitRate;
	uint8_t		DSCTy;
	int16_t		appType;
	int16_t		packetAddress;
	uint8_t		DGflag;
	int16_t		FEC_scheme;
	int16_t		last_cntIdx;
	std::vector<uint8_t>	series;
	int16_t		fillPointer;
	bool		assembling;
	std::vector<uint8_t> AppVector;
	std::vector<uint8_t> FECVector;
	bool		FEC_table [9];
	reedSolomon my_rsDecoder;
//
//	result handlers
	void		handleTDCAsyncstream 	(const uint8_t *, int32_t);
	void		handlePackets		(const uint8_t *, int16_t);
	void		handlePacket		(const uint8_t *vec);

	void		handleRSPacket		(const uint8_t *);
	void		registerFEC		(const uint8_t *, int);
	void		clear_FECtable		();
	bool		FEC_complete		();
	void		handle_RSpackets	(const std::vector<uint8_t> &);
	void		handle_RSpacket		(const uint8_t *, int16_t);
	int		addPacket		(const uint8_t *,
	                                         std::vector<uint8_t> &, int);

	void		processRS		(std::vector<uint8_t> &appdata,
                                  	         const std::vector<uint8_t> &RSdata);

	std::unique_ptr<IDataHandler> my_dataHandler;
};
