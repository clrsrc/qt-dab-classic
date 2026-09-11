// DAB Classic v3: portiert aus Qt-DAB sources/backend/data/pad-handler.h
// (Jan van Katwijk, GPLv2+): QObject/Signale showLabel, show_mothandling,
// show_dl2 -> BackendCallbacks (dls, dlPlus, motObject); QByteArray ->
// std::string (Rohbytes), QString -> std::string (UTF-8), QScopedPointer ->
// std::unique_ptr. X-PAD-/DLS-Segmentierung unveraendert. DL+ (TS 102 980)
// vollstaendig: alle Tags (contentType 0..63) mit IT/IR statt nur
// Titel/Komponist/Sendername (v1 add_toDL2).
#
/*
 *    Copyright (C) 2015 .. 2025
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

#include	<cstring>
#include	<cstdint>
#include	<vector>
#include	<string>
#include	<memory>
#include	"mot-object.h"
#include	"backend-callbacks.h"

class	padHandler {
public:
			padHandler		(uint32_t SId, BackendCallbacks *);
			~padHandler		();
	void		processPAD		(const uint8_t *,
	                                         int16_t, uint8_t, uint8_t);
private:
	uint32_t	SId;
	BackendCallbacks *cb;
	void		handle_variablePAD	(const uint8_t *,
	                                             int16_t, uint8_t);
	void		handle_shortPAD		(const uint8_t *,
	                                             int16_t, uint8_t);
	void		dynamicLabel		(const uint8_t *,
	                                             int16_t, uint8_t);
	void		new_MSC_element		(const std::vector<uint8_t> &);
	void		add_MSC_element		(const std::vector<uint8_t> &);
	void		build_MSC_segment	(const std::vector<uint8_t> &);
	void		showLabel		(const std::string &);
	std::string	dynamicLabelText;	// Rohbytes im Zeichensatz charSet
	int16_t		charSet;
	std::unique_ptr<motObject>	currentSlide;
	uint8_t		last_appType;
	bool		mscGroupElement;
	int		xpadLength;
	int16_t		still_to_go;
	std::vector<uint8_t> shortpadData;
        bool		lastSegment;
        bool		firstSegment;
	int16_t		segmentNumber;
//      dataGroupLength is set when having processed an appType 1
        int 		dataGroupLength;

	int16_t		segmentno;
	int16_t		remainDataLength;
	bool		isLastSegment;
	bool		moreXPad;

//
//      The msc_dataGroupBuffer is - as the name suggests - used for
//      assembling the msc_data group.
        std::vector<uint8_t> msc_dataGroupBuffer;

//	DL+: Text des letzten vollstaendigen Labels als Codepoints
//	(die DL+-Marker zaehlen Zeichen, nicht Bytes)
	std::vector<uint32_t>	dlCodepoints;
	std::string	extractText		(uint16_t, uint16_t);
	void		add_toDL2		(const std::string &);
	void		add_toDL2		(const uint8_t *, int,
	                                                uint8_t, uint8_t);
};
