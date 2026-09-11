// DAB Classic v3: portiert aus Qt-DAB sources/backend/data/mot/mot-handler.h
// (Jan van Katwijk, GPLv2+): virtual_dataHandler (QObject) -> IDataHandler,
// RadioInterface -> BackendCallbacks. MSC-Datagroup-Auswertung unveraendert.
#
/*
 *    Copyright (C) 2015 .. 2017
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

#include	"dab-constants.h"
#include	"backend-callbacks.h"
#include	<vector>

//	v1 virtual_dataHandler
class	IDataHandler {
public:
		IDataHandler	() {}
virtual		~IDataHandler	() {}
virtual	void	add_mscDatagroup	(const std::vector<uint8_t> &x) {
	   (void)x;
	}
};

class	motObject;
class	motDirectory;

#define	MOT_TABLESIZE	256
struct motTable_ {
	uint16_t	transportId;
	int32_t		orderNumber;
	motObject	*motSlide;
};

class	motHandler: public IDataHandler {
public:
		motHandler	(BackendCallbacks *, uint32_t SId);
		~motHandler	() override;
	void	add_mscDatagroup	(const std::vector<uint8_t> &) override;
private:
	BackendCallbacks *cb;
	void		setHandle	(motObject *, uint16_t);
	motObject	*getHandle	(uint16_t);
	int		orderNumber;
	motDirectory	*theDirectory;
	uint32_t	SId;
	motTable_	motTable [MOT_TABLESIZE];	// v1: global static
};
