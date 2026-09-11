// DAB Classic v3: portiert aus Qt-DAB sources/backend/data/mot/mot-object.h
// (Jan van Katwijk, GPLv2+): QObject/QImage/QLabel/QDir gestrichen,
// QByteArray -> std::vector<uint8_t>, QString -> std::string (UTF-8),
// Signal handle_motObject -> BackendCallbacks::motObject. Header-Parsing
// und Segment-Reassembly unveraendert.
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

#pragma once

#include	"dab-constants.h"
#include	"mot-content-types.h"
#include	"backend-callbacks.h"
#include	<map>
#include	<string>
#include	<vector>
#include	<iterator>

class	motObject {
public:
		motObject (BackendCallbacks *cb,
	                   uint32_t	SId,
	                   bool		dirElement,
	                   uint16_t	transportId,
	                   const uint8_t	*segment,
	                   int32_t	segmentSize,
	                   bool		lastFlag);
		~motObject	();
	void	addBodySegment (const uint8_t	*bodySegment,
                                int16_t	segmentNumber,
                                int32_t	segmentSize,
	                        bool	lastFlag);
	uint16_t	get_transportId	();
	int		get_headerSize	();
private:
	BackendCallbacks *cb;
	uint32_t	SId;
	bool		dirElement;
	uint16_t	transportId;
	int16_t		numofSegments;
	int32_t		segmentSize;
	int32_t		headerSize;
	int32_t		bodySize;
	MOTContentType	contentType;
	std::string	name;
	void		handleComplete	();
	std::map<int, std::vector<uint8_t>> motMap;
};

//	MIME-Typ aus dem MOT-Content-Type (TS 101 756 Tabelle 17)
const char	*motMimeType (int contentType);
