// DAB Classic v3: portiert aus Qt-DAB sources/backend/data/mot/mot-object.cpp
// (Jan van Katwijk, GPLv2+), siehe mot-object.h.
#
/*
 *    Copyright (C) 2015 .. 2024
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
 *
 *	We pack each MOT object into a single instance of the class
 *	motObject. The class is instantiated with a "type 3", i.e.
 *	header object.
 *	MOTobjects are identified by their TransportId.
 *	The callers assure that when calling motObject or
 *	its functions, they share their unique transportId;
 *	The associated body segments have to wait until the header
 *	is in.
 */
#include	"mot-object.h"
#include	<algorithm>
#include	<cstdio>
#include	"charsets.h"

	   motObject::motObject (BackendCallbacks *cb,
	                         uint32_t	SId,
	                         bool		dirElement,
	                         uint16_t	transportId,
	                         const uint8_t	*segment,
	                         int32_t	segmentSize,
	                         bool		lastFlag) {
int32_t pointer = 7;
uint16_t	rawContentType = 0;

	(void)lastFlag;
	this	-> cb			= cb;
	this	-> SId			= SId;
	this	-> dirElement		= dirElement;
	this	-> name			= "";
	this	-> transportId		= transportId;
	this	-> segmentSize		= segmentSize;
	this	-> numofSegments	= -1;

	headerSize     =
	   ((segment [3] & 0x0F) << 9) |
                   (segment [4] << 1) | ((segment [5] >> 7) & 0x01);
	bodySize       =
	   (segment [0] << 20) | (segment [1] << 12) |
                            (segment [2] << 4 ) | ((segment [3] & 0xF0) >> 4);

// Extract the content type
	rawContentType  |= ((segment [5] >> 1) & 0x3F) << 8;
	rawContentType	|= ((segment [5] & 0x01) << 8) | segment [6];
	contentType = static_cast<MOTContentType>(rawContentType);

	int reference = segmentSize == -1 ? headerSize :
	                 headerSize == -1 ? segmentSize :
	                  std::min ((int)headerSize, (int)segmentSize);

        while ((uint16_t)pointer < reference) {
           uint8_t PLI	= (segment [pointer] & 0300) >> 6;
           uint8_t paramId = (segment [pointer] & 077);
           uint16_t     length;
           switch (PLI) {
              case 00:
                 pointer += 1;
                 break;

              case 01:
                 pointer += 2;
                 break;

	      case 02:
                 pointer += 5;
                 break;

              case 03: {
//	Review G4: Laengenfeld und Parameterdaten nur innerhalb des
//	Segments (reference) lesen
                 if (pointer + 1 >= reference)
                    return;
                 if ((segment [pointer + 1] & 0x80) != 0) {
                    if (pointer + 2 >= reference)
                       return;
                    length = (segment [pointer + 1] & 0x7F) << 8 |
                              segment [pointer + 2];
                    pointer = pointer + 3 ;
                 }
                 else {
                    length = segment [pointer + 1] & 0x7F;
	            pointer = pointer + 2;
                 }
	         switch (paramId) {
	            case 12: {	// contentName 6.2.2.1.1
                       if (pointer >= reference)
                          return;
                       uint8_t charSet = segment [pointer] >> 4;
	               std::string nameText;
                       for (int i = 1; (i < length) &&
                                       (pointer + i < reference); i ++) {
	                  if (i < 64)
                             nameText. push_back ((char)segment [pointer + i]);
	               }
	               name = toStringUsingCharset (
	                           nameText. data (),
	                           (CharacterSet) charSet,
	                           (int)nameText. size ());
                       pointer += length;
	               break;
	            }

	            default:	// alle anderen Parameter ueberspringen
	                        // (v1: reserved, trigger time, expiration,
	                        // priority, label, body version, mime type,
	                        // compression, CAInfo ... jeweils pointer += length)
	               pointer += length;
	               break;
	         }
              }
	   }
	}
}

	motObject::~motObject () {
}

uint16_t	motObject::get_transportId () {
	return transportId;
}

//      type 4 is a segment.
//	The pad/dir software will only call this whenever it has
//	established that the current slide has a header with the
//	same transport Id
//
//	Note that segments do not need to come in in the right order
void	motObject::addBodySegment (const uint8_t	*bodySegment,
	                           int16_t	segmentNumber,
	                           int32_t	segmentSize,
	                           bool		lastFlag) {

	if ((segmentNumber < 0) || (segmentNumber >= 8192))
	   return;
//
//	check already exists
	if (motMap. find (segmentNumber) != motMap. end ())
	   return;

//      Note that the last segment may have a different size
        if (!lastFlag && (this -> segmentSize == -1))
           this -> segmentSize = segmentSize;

	std::vector<uint8_t> segment (bodySegment, bodySegment + segmentSize);
	motMap. insert (std::make_pair (segmentNumber, std::move (segment)));
//
        if (lastFlag)
           numofSegments = segmentNumber + 1;

	if (numofSegments == -1)
	   return;
//
//	once we know how many segments there are/should be,
//	we check for completeness
	for (int16_t i = 0; i < numofSegments; i ++) {
	   if (motMap. find (i) == motMap. end())
	      return;
	}
//	The motObject is (seems to be) complete
	handleComplete ();
}

void	motObject::handleComplete	() {
std::vector<uint8_t> result;
	for (const auto &it : motMap)
	   result. insert (result. end (), it. second. begin (), it. second. end ());
	if ((name == "") && !dirElement) {
	   char t [16];
	   snprintf (t, sizeof (t), "%x", transportId);
	   name = t;
	}
	if (name != "")
	   emitBe (cb -> motObject, result, name, (int)contentType, dirElement, SId);
}

int	motObject::get_headerSize	() {
	return headerSize;
}

const char	*motMimeType (int ct) {
	switch (ct) {
	   case MOTCTImageGIF:	return "image/gif";
	   case MOTCTImageJFIF:	return "image/jpeg";
	   case MOTCTImageBMP:	return "image/bmp";
	   case MOTCTImagePNG:	return "image/png";
	   case MOTCTTextASCII:	return "text/plain";
	   case MOTCTTextLatin1: return "text/plain";
	   case MOTCTTextHTML:	return "text/html";
	   case MOTCTTextPDF:	return "application/pdf";
	   default:		return "application/octet-stream";
	}
}
