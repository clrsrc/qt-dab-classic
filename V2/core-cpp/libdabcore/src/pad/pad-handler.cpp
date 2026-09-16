// DAB Classic v3: portiert aus Qt-DAB sources/backend/data/pad-handler.cpp
// (Jan van Katwijk, GPLv2+), siehe pad-handler.h.
#
/*
 *    Copyright (C) 2015 .. 2023
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
#include	"pad-handler.h"
#include	<cstring>
#include	"crc-handlers.h"
#include	"charsets.h"
#include	"mot-object.h"
/**
  *	\class padHandler
  *	Handles the pad segments passed on from mp2- and mp4Processor
  */
	padHandler::padHandler	(uint32_t	SId,
	                         BackendCallbacks *cb) {
	this	-> SId			= SId;
	this	-> cb			= cb;
//
//	mscGroupElement indicates whether we are handling an
//	msc datagroup or not.
	mscGroupElement	= false;
	dataGroupLength	= 0;

//	xpadLength tells - if mscGroupElement is "on" - the size of the
//	xpadfields, needed for handling xpads without CI's
	xpadLength	= -1;
//
//	and for the shortPad we maintain
	still_to_go	= 0;
	lastSegment	= false;
	firstSegment	= false;
	segmentNumber	= -1;
	charSet		= 0;
	last_appType	= 0;
//
//
	segmentno		= -1;
	remainDataLength	= 0;
	isLastSegment		= false;
	moreXPad		= false;
}

	padHandler::~padHandler() {
}

//	Data is stored reverse, we pass the vector and the index of the
//	last element of the XPad data.
//	 L0 is the "top" byte of the L field, L1 the next to top one.
void	padHandler::processPAD (const uint8_t *buffer, int16_t last,
	                        uint8_t L1, uint8_t L0) {
uint8_t	fpadType	= (L1 >> 6) & 03;

	if (fpadType != 00) {
	   return;
	}
//
//	OK, we'll try

	uint8_t x_padInd	= (L1 >> 4) & 03;
	uint8_t CI_flag         = L0 & 02;

	switch (x_padInd) {
	   default:
	      break;

	   case  01 :
	      handle_shortPAD	 (buffer, last, CI_flag);
	      break;

	   case  02:
	      handle_variablePAD (buffer, last, CI_flag);
	      break;
	}
}

//	Vollstaendiges Label: Rohbytes im Zeichensatz -> UTF-8, an DL+
//	merken und als dls melden (v1: toQStringUsingCharset + showLabel)
void	padHandler::showLabel (const std::string &raw) {
	std::string displayText =
	         toStringUsingCharset (raw. data (),
	                               (CharacterSet)charSet,
	                               (int)raw. size ());
	add_toDL2 (displayText);
	emitBe (cb -> dls, displayText);
}

//	Since the data is stored in reversed order, we pass
//	on the vector address and the offset of the last element
//	in that vector
//
//	shortPad's are 4  byte values. If the CI is on, then type 2
//	indicates the start of a segment. Type 3 the continuation.
//	The start of a message, i.e. segment 0 is (a.o) found by
//	a (1, 0) value of the firstSegment/lastSegment values.
//	The end of a segment might be indicated by a specific pattern
//	of these 2 values, but it is not clear to me how.
//	For me, the end of a segment is when we collected the amount
//	of values specified for the segment.
void	padHandler::handle_shortPAD (const uint8_t *b,
	                             int16_t last, uint8_t CIf) {
int16_t	i;

	if (CIf != 0) {	// has a CI flag
	   uint8_t CI    = b [last];
	   firstSegment  = (b [last - 1] & 0x40) != 0;
	   lastSegment   = (b [last - 1] & 0x20) != 0;
	   charSet       = b [last - 2] & 0x0F;
	   uint8_t AcTy  = CI & 037;	// application type

	   if (firstSegment)
	      dynamicLabelText. clear ();
	   switch (AcTy) {
	      default:
	         break;

	      case 0:	// end marker
	         break;
//
	      case 2:	// start of fragment, extract the length
	         if (firstSegment && !lastSegment) {
	            segmentNumber   = b [last - 2] >> 4;
	            if (dynamicLabelText. size () > 0) {
	               showLabel (dynamicLabelText);
	            }
	            dynamicLabelText = "";
	         }
	         still_to_go     = b [last - 1] & 0x0F;
	         shortpadData. resize (0);
	         shortpadData. push_back (b [last - 3]);
	         break;

	      case 3:	// continuation of fragment
	         for (i = 0; (i < 3) && (still_to_go > 0); i ++) {
	            still_to_go --;
	            shortpadData. push_back (b [last - 1 - i]);
	         }

	         if ((still_to_go <= 0) && (shortpadData. size() > 1)) {
	             dynamicLabelText. append ((const char *)shortpadData. data (),
	                                       shortpadData. size ());
	             shortpadData. resize (0);
	         }
	         break;

	   }
	}
	else {	// No CI flag
 //	X-PAD field is all data
	   for (i = 0; (i < 4) && (still_to_go > 0); i ++) {
	      shortpadData. push_back (b [last - i]);
	      still_to_go --;
	   }

//	at the end of a frame
	   if ((still_to_go <= 0) && (shortpadData. size() > 0)) {
//
//	just to avoid doubling by unsollicited shortpads
	      dynamicLabelText. append ((const char *)shortpadData. data (),
	                                shortpadData. size ());
	      shortpadData. resize (0);
//	if we are at the end of the last segment (and the text is not empty)
//	then show it.
	      if (!firstSegment && lastSegment) {
	         if (dynamicLabelText. size() > 0) {
	            showLabel (dynamicLabelText);
	         }
	         dynamicLabelText. clear();
	      }
	   }
	}
}
///////////////////////////////////////////////////////////////////////
//
//	Here we end up when F_PAD type = 00 and X-PAD Ind = 02
static
int16_t	lengthTable [] = {4, 6, 8, 12, 16, 24, 32, 48};

//	Since the data is reversed, we pass on the vector address
//	and the offset of the last element in the vector,
//	i.e. we start (downwards)  beginning at b [last];
void	padHandler::handle_variablePAD (const uint8_t *b,
	                                int16_t last, uint8_t CI_flag) {
int16_t	CI_Index = 0;
uint8_t CI_table [4];
int16_t	base	= last;
std::vector<uint8_t> data;		// for the local addition

//	If an xpadfield shows with a CI_flag == 0, and if we are
//	dealing with an msc field, the size to be taken is
//	the size of the latest xpadfield that had a CI_flag != 0
	if (CI_flag == 0) {
	   if (xpadLength > 0) {
	      if (last < xpadLength - 1) {
	         return;
	      }

	      data. resize (xpadLength);
	      for (int16_t j = 0; j < xpadLength; j ++)
	         data [j] = b [last - j];
	      switch (last_appType) {
// Dynamic label segment, start of X-PAD data group
	         case 2:
// Dynamic label segment, continuation of X-PAD data group
	         case 3:
	            dynamicLabel((uint8_t *)(data.data()), xpadLength, 3);
	            break;

	         case 12:   // MOT, start of X-PAD data group
	         case 13:   // MOT, continuation of X-PAD data group
	            if (mscGroupElement)
	               add_MSC_element(data);
	            break;
	         default:
	            break;
	      }
	   }
	   return;
	}
//
//	The CI flag in the F_PAD data is set, so we have local CI's
//	7.4.2.2: Contents indicators are one byte long

//	Review G4: b [0 .. last] ist gueltig; ohne Untergrenze las die
//	Schleife bei fehlerhaften Laengen vor den Puffer
	while ((base >= 0) && ((b [base] & 037) != 0) && (CI_Index < 4))
	   CI_table [CI_Index ++] = b [base --];

	if (CI_Index < 4) 	// we have a "0" indicator, adjust base
	   base -= 1;
	if (CI_Index == 0)
	   return;

//	The space for the CI's does belong to the CPadfield, so
//	do not forget to take into account the '0'field if CI_Index < 4
	{
	   xpadLength = 0;
	   for (int16_t i = 0; i < CI_Index; i ++)
	      xpadLength += lengthTable [CI_table [i] >> 5];
	   xpadLength += CI_Index == 4 ? 4 : CI_Index + 1;
	}

//	Handle the contents
	for (int16_t i = 0; i < CI_Index; i ++) {
	   uint8_t appType	= CI_table [i] & 037;
	   int16_t length	= lengthTable [CI_table [i] >> 5];

	   if (appType == 1) {	// length spec
	      if (base < 1)	// Review G4
	         return;
	      dataGroupLength = ((b [base] & 077) << 8) | b [base - 1];
	      base -= 4;
	      last_appType = 1;
	      continue;
	   }

//	collect data, reverse the reversed bytes
	   if (base - (length - 1) < 0)	// Review G4: Feld passt nicht mehr
	      return;
	   data. resize (length);
	   for (int16_t j = 0; j < length; j ++)
	      data [j] = b [base - j];

	   switch (appType) {
	      default:
	         return; // sorry, we do not handle this

	      case 1:	//
	         return;
	      case 2:	// Dynamic label segment, start of X-PAD data group
	      case 3:	// Dynamic label segment, continuation of X-PAD data group
	         dynamicLabel ((uint8_t *)(data. data()),
	                        data. size(), CI_table [i]);
	         break;

	      case 12:	 // MOT, start of X-PAD data group
	         new_MSC_element (data);
	         break;

 	      case 13:	 // MOT, continuation of X-PAD data group
	         add_MSC_element (data);
	         break;
	   }

	   last_appType = appType;
	   base -= length;
	   if (base < 0 && i < CI_Index - 1) {
	      return;
	   }
	}
}
//
//	A dynamic label is created from a sequence of (dynamic) xpad
//	fields, starting with CI = 2, continuing with CI = 3
void	padHandler::dynamicLabel (const uint8_t *data,
	                                int16_t length, uint8_t CI) {
int16_t  dataLength                = 0;

	if ((CI & 037) == 02) {	// start of segment
	   uint16_t prefix = (data [0] << 8) | data [1];
	   uint8_t field_1 = (prefix >> 8) & 017;
	   uint8_t Cflag   = (prefix >> 12) & 01;
	   uint8_t first   = (prefix >> 14) & 01;
	   uint8_t last    = (prefix >> 13) & 01;
	   dataLength	   = length - 2; // The length with header removed

	   if (first) {
	      segmentno = 1;
	      charSet = (prefix >> 4) & 017;
	      dynamicLabelText. clear ();
	   }
	   else {
	      int test = ((prefix >> 4) & 07) + 1;
	      if (test != segmentno + 1) {
	         segmentno = -1;
	         return;
	      }
	      segmentno = ((prefix >> 4) & 07) + 1;
	   }
//
//	etsi TS 102 980 specifies the DL plus objects,
 	  if (Cflag) {		// special dynamic label command
	      uint16_t Command = (prefix >> 8) & 0x0f;
	      uint8_t  field_2	= (prefix >> 4) & 0x0f;
	      uint8_t  field_3	= prefix & 0x0f;
	      switch (Command) {
	         case 1:
	            dynamicLabelText. clear ();
	            segmentno = -1;
	            break;
	         case 2:
	            if (dataLength > 2) {
	               add_toDL2 (&data [2], dataLength - 2, field_2, field_3);
	            }
	            break;
	         default:
	            break;
	      }
	   }
	   else {		// Dynamic text length
	      int16_t totalDataLength = field_1 + 1;
	      if (length - 2 < totalDataLength) {
	         dataLength = length - 2; // the length is shortened by header
	         moreXPad   = true;
	      }
	      else {
	         dataLength = totalDataLength;  // no more xpad app's 3
	         moreXPad   = false;
	      }
//	convert dynamic label
	      dynamicLabelText. append ((const char *)(&data [2]), dataLength);

//	if at the end, show the label
	      if (last) {
	         if (!moreXPad) {
	            showLabel (dynamicLabelText);
	            segmentno = -1;
	         }
	         else
	            isLastSegment = true;
	      }
	      else
	         isLastSegment = false;
//	calculate remaining data length
	      remainDataLength = totalDataLength - dataLength;
	   }
	}
	else
	if (((CI & 037) == 03) && moreXPad) {
	   if (remainDataLength > length) {
	      dataLength = length;
	      remainDataLength -= length;
	   }
	   else {
	      dataLength = remainDataLength;
	      moreXPad   = false;
	   }

	   dynamicLabelText. append ((const char *)data, dataLength);
	   if (!moreXPad && isLastSegment) {
	      showLabel (dynamicLabelText);
	   }
	}
}

//
//	Called at the start of the msc datagroupfield,
//	the msc_length was given by the preceding appType "1"
void	padHandler::new_MSC_element (const std::vector<uint8_t> &data) {

	if (data. size() >= (uint16_t)dataGroupLength) { // msc element is single item
	   msc_dataGroupBuffer. clear();
	   build_MSC_segment (data);
	   mscGroupElement = false;
	   return;
	}

	mscGroupElement		= true;
	msc_dataGroupBuffer. clear();
	msc_dataGroupBuffer	= data;
}

//
void	padHandler::add_MSC_element	(const std::vector<uint8_t> &data) {
int32_t	currentLength = msc_dataGroupBuffer. size();
//
//	just to ensure that, when a "12" appType is missing, the
//	data of "13" appType elements is not endlessly collected.
	if (currentLength == 0) {
	   return;
	}

	msc_dataGroupBuffer. insert (std::end (msc_dataGroupBuffer),
	                             std::begin (data), std::end (data));
	if (msc_dataGroupBuffer. size() >= (uint32_t)dataGroupLength) {
	   build_MSC_segment (msc_dataGroupBuffer);
	   msc_dataGroupBuffer. clear();
	}
}

void	padHandler::build_MSC_segment (const std::vector<uint8_t> &data) {
//	we have a MOT segment, let us look what is in it
//	according to DAB 300 401 (page 37) the header (MSC data group)
//	is
int32_t	size	= data. size() < (uint32_t)dataGroupLength ? data. size() :
	                                            dataGroupLength;

	if (size < 2) {
	   return;
	}

const uint8_t	groupType	=  data [0] & 0xF;
uint16_t	transportId	= 0;	// default
int16_t		segmentNumber	= -1;		// default
bool		lastFlag	= false;	// default
uint16_t	index;

	if ((data [0] & 0x40) != 0) {
	   bool res	= check_crc_bytes (data. data(), size - 2);
	   if (!res) {
	      return;
	   }
	}

	if ((groupType != 3) && (groupType != 4)) {
	   return;		// do not know yet
	}
//	extensionflag
	bool	extensionFlag	= (data [0] & 0x80) != 0;
//	if the segmentflag is on, then a lastflag and segmentnumber are
//	available, i.e. 2 bytes more.
//	Theoretically, the segment number can be as large as 16384
	index			= extensionFlag ? 4 : 2;
	bool	segmentFlag	=  (data [0] & 0x20) != 0;
	if ((segmentFlag) != 0) {
	   lastFlag		= data [index] & 0x80;
	   segmentNumber	= ((data [index] & 0x7F) << 8) |
	                                               data [index + 1];
	   index += 2;
	}

//	if the user access flag is on there is a user accessfield
	if ((data [0] & 0x10) != 0) {
	   int16_t lengthIndicator = data [index] & 0x0F;
	   if ((data [index] & 0x10) != 0) { //transportid flag
	      transportId = data [index + 1] << 8 |
	                    data [index + 2];
	      index += 3;
	   }
	   index += (lengthIndicator - 2);
	}

	uint32_t segmentSize	= ((data [index + 0] & 0x1F) << 8) |
	                            data [index + 1];

//	handling MOT in the PAD, we only deal here with type 3/4
	switch (groupType) {
	   case 3:
	      if (!currentSlide ||
	          (currentSlide -> get_transportId() != transportId))
	          currentSlide. reset (new motObject (cb,
	                                              SId,
	                                              false,
	                                              transportId,
	                                              &data [index + 2],
	                                              segmentSize,
	                                              lastFlag));
	      break;

	   case 4: {
	      if (!currentSlide ||
	          (currentSlide -> get_transportId() != transportId))
	         return;
	      currentSlide -> addBodySegment (&data [index + 2],
	                                      segmentNumber,
	                                      segmentSize,
	                                      lastFlag);
	      }
	      break;

	   default:		// cannot (should not) happen
	      break;
	}
}
//
//	DL+ (ETSI TS 102 980): der Text des letzten vollstaendigen Labels
//	wird als Codepoint-Folge gemerkt, die Start-/Laengenmarker der Tags
//	zaehlen Zeichen.
static
void	utf8ToCodepoints (const std::string &s, std::vector<uint32_t> &out) {
	out. clear ();
	size_t i = 0;
	while (i < s. size ()) {
	   uint8_t c = (uint8_t)s [i];
	   uint32_t cp;
	   int extra;
	   if (c < 0x80) { cp = c; extra = 0; }
	   else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
	   else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
	   else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
	   else { cp = 0xFFFD; extra = 0; }
	   i ++;
	   for (int k = 0; k < extra && i < s. size (); k ++, i ++)
	      cp = (cp << 6) | ((uint8_t)s [i] & 0x3F);
	   out. push_back (cp);
	}
}

void	padHandler::add_toDL2 (const std::string &text) {
	utf8ToCodepoints (text, dlCodepoints);
}

//	Zeichen start .. start + length (einschliesslich) des Labels
std::string padHandler::extractText (uint16_t start, uint16_t length) {
std::string res;
	if (start + length >= dlCodepoints. size ())
	   return "";
	for (int i = start; i <= start + length; i ++)
	   appendUtf8 (res, dlCodepoints [i]);
	return res;
}

//	DL+-Kommando: Byte 0 = CId (4 Bit, muss 0 sein) | CB (4 Bit:
//	IT, IR, NT[1:0]); danach NT+1 Tags zu je 3 Byte (contentType,
//	Startmarker, Laengenmarker; je 7 Bit).
void	padHandler::add_toDL2 (const uint8_t *data, int dataLength,
	                              uint8_t field_2, uint8_t field_3) {
	(void)field_2;
	int DL_size	= (int)((uint8_t)field_3) + 1;
	uint8_t CId	= (data [0] >> 4) & 0x0f;
	if (CId != 0)		// should not happen
	   return;
	uint8_t CB	= data [0] & 0x0f;
	bool	IT	= (CB & 0x08) != 0;	// item toggle
	bool	IR	= (CB & 0x04) != 0;	// item running
	uint8_t NT	= CB & 0x03;		// number of tags - 1
	if (dataLength <= DL_size)
	   return;
	if (dataLength < 1 + 3 * (NT + 1))
	   return;
	std::vector<std::pair<uint8_t, std::string>> tags;
	for (int i = 0; i <= NT; i ++) {
	   uint8_t contentType	= data [1 + 3 * i + 0] & 0x7F;
	   uint8_t startMarker	= data [1 + 3 * i + 1] & 0x7F;
	   uint8_t lengthMarker = data [1 + 3 * i + 2] & 0x7F;
	   std::string ss = dlCodepoints. empty () ? std::string ()
	                       : extractText (startMarker, lengthMarker);
	   tags. emplace_back (contentType, ss);
	}
	emitBe (cb -> dlPlus, IT, IR, tags);
}
