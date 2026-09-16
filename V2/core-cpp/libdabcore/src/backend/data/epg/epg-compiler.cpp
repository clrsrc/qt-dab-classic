// DAB Classic v3: portiert aus Qt-DAB sources/backend/data/epg/epg-compiler.cpp
// (Jan van Katwijk, GPLv2+), siehe epg-compiler.h. Nur das Qt-Plumbing ist
// ersetzt (QDomDocument -> XmlNode, QString -> std::string/UTF-8, QDateTime ->
// mktime/localtime, QString::fromUtf8/fromLatin1 -> decodeSmartly). Die
// Auswertung selbst ist unveraendert, auch dort, wo v1 von TS 102 371
// abweicht (process_485 liest zweimal dasselbe Byte, process_474 addiert
// den LTO-Stundenwert als Minuten, "oresentationTime", programmeEvent als
// "link", programmeGroup-Id als "version"), damit die erzeugten XML-Dateien
// zu den v1-Dateien formatgleich bleiben.
#
/*
 *    Copyright (C) 2017 .. 2025
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
#include	"epg-compiler.h"
#include	"time-converter.h"
#include	<cstdlib>
#include	<ctime>
#include	<cstring>
//	miniparser for epg
//	input:	a vector delivered by the MOT handler
//	output:	the XML text (v1: a QDomDocument)

static
uint8_t	bitTable [] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};

//	Lesezugriff mit Bereichspruefung: v1 liest v [i] ungeprueft; ausserhalb
//	des Vektors liefern wir 0 (aendert bei gueltigen Objekten nichts).
static inline
uint8_t	at (const std::vector<uint8_t> &v, int i) {
	return (i >= 0 && (size_t)i < v. size ()) ? v [i] : 0;
}

static inline
int	getBit (const std::vector<uint8_t> &v, int bitnr) {
int bytenr	= bitnr / 8;

	bitnr 	= bitnr % 8;
	return (at (v, bytenr) & bitTable [bitnr]) != 0 ? 1 : 0;
}

//	v1: uint16_t res in einer uint32_t-Funktion (17-Bit-MJD wird auf
//	16 Bit gekuerzt) - unveraendert uebernommen.
static inline
uint32_t getBits (const std::vector<uint8_t> &v, int bitnr, int length) {
uint16_t res	= 0;
	for (int i = 0; i < length; i ++) {
	   res <<= 1;
	   res |= getBit (v, bitnr + i);
	}
	return res;
}

static inline
int	setLength (const std::vector<uint8_t> &v, int &index) {
int length	= at (v, index + 1);

	if (length == 0xFE) {
	   length = (at (v, index + 2) << 8) | at (v, index + 3);
	   index	+= 4;
	}
	else
	if (length == 0xFF) {
	   length = (at (v, index + 2) << 16) |
	            (at (v, index + 3) << 8) | at (v, index + 4);
	   index	+= 5;
	}
	else
	   index	+= 2;
	int endPoint = index + length;
	if (endPoint > (int)v. size ())	// Bereichspruefung (nicht in v1)
	   endPoint = v. size ();
	return	endPoint;
}

//	--- Ersatz fuer QString-Konvertierungen -------------------------------
//
//	QString (QChar (byte)) bzw. QString::fromLatin1: Byte -> Codepoint
static
void	appendLatin1 (std::string &s, uint8_t c) {
	if (c < 0x80)
	   s. push_back ((char)c);
	else {
	   s. push_back ((char)(0xC0 | (c >> 6)));
	   s. push_back ((char)(0x80 | (c & 0x3F)));
	}
}

static
std::string latin1ToUtf8 (const std::string &text) {
std::string res;
	for (uint8_t c : text)
	   appendLatin1 (res, c);
	return res;
}

//	QString::toLatin1: Codepoints > 0xFF werden zu '?'
static
std::string utf8ToLatin1 (const std::string &utf8) {
std::string res;
	for (size_t i = 0; i < utf8. size (); ) {
	   uint8_t c = utf8 [i];
	   uint32_t cp;
	   int n;
	   if (c < 0x80) { cp = c; n = 1; }
	   else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
	   else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
	   else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
	   else { res. push_back ('?'); i ++; continue; }
	   for (int j = 1; j < n; j ++) {
	      if (i + j >= utf8. size ()) { cp = 0xFFFD; break; }
	      cp = (cp << 6) | (utf8 [i + j] & 0x3F);
	   }
	   i += n;
	   res. push_back (cp <= 0xFF ? (char)cp : '?');
	}
	return res;
}

//	QString::fromUtf8 liefert U+FFFD fuer ungueltige Sequenzen;
//	decodeSmartly (v1) faellt dann auf Latin-1 zurueck.
static
bool	validUtf8 (const std::string &s) {
	for (size_t i = 0; i < s. size (); ) {
	   uint8_t c = s [i];
	   int n; uint32_t cp;
	   if (c < 0x80) { i ++; continue; }
	   else if (c >= 0xC2 && c <= 0xDF) { n = 2; cp = c & 0x1F; }
	   else if (c >= 0xE0 && c <= 0xEF) { n = 3; cp = c & 0x0F; }
	   else if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07; }
	   else return false;
	   if (i + n > s. size ()) return false;
	   for (int j = 1; j < n; j ++) {
	      uint8_t d = s [i + j];
	      if ((d & 0xC0) != 0x80) return false;
	      cp = (cp << 6) | (d & 0x3F);
	   }
	   if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000))
	      return false;		// overlong
	   if (cp >= 0xD800 && cp <= 0xDFFF) return false;
	   if (cp > 0x10FFFF) return false;
	   if (cp == 0xFFFD) return false;	// v1: contains (QChar (0xFFFD))
	   i += n;
	}
	return true;
}

//	QString::fromUtf8: ungueltige Bytes werden zu U+FFFD
static std::string fromUtf8 (const std::string &s) {
	if (validUtf8 (s))
	   return s;
	std::string res;
	for (size_t i = 0; i < s. size (); ) {
	   size_t n = 1;
	   uint8_t c = s [i];
	   if (c >= 0xC2 && c <= 0xDF) n = 2;
	   else if (c >= 0xE0 && c <= 0xEF) n = 3;
	   else if (c >= 0xF0 && c <= 0xF4) n = 4;
	   else if (c >= 0x80) { res += "�"; i ++; continue; }
	   if (i + n <= s. size () && validUtf8 (s. substr (i, n))) {
	      res. append (s, i, n);
	      i += n;
	   }
	   else {
	      res += "�";
	      i ++;
	   }
	}
	return res;
}

// try UTF-8 first, fall back to Latin-1 if invalid sequences found
static std::string decodeSmartly (const std::string &text) {
	if (validUtf8 (text))
	   return text;
	// UTF-8 decoding produced replacement chars -> try Latin-1
	return latin1ToUtf8 (text);
}

#define	EPG_TAG			0X02
#define	SERVICE_TAG		0X03
//
	epgCompiler::epgCompiler	(std::function<void(const std::string &)> logger) {
	this -> theErrorLogger = logger;
	this -> lto = 0;
}

	epgCompiler::~epgCompiler	() {
}

int	epgCompiler::process_epg	(std::string &xml,
	                                 const std::vector<uint8_t> &v,
	                                 int lto) {
	if (v. size () < 2)
	   return noType;
uint8_t	tag	= v [0];
int	index	= 0;
	this	-> lto = lto;
	this	-> lastScopeSid = 0;
	for (int i = 0; i < 16; i ++)
	   stringTable [i] = "";
	int endPoint = setLength (v, index);

	if (tag == EPG_TAG) {
	   XmlNode epg ("epg");
	   epg. setAttribute ("system", "DAB");
//	V3: Zeiten stehen in der Ortszeit des Systems (process_474); v1-Dateien
//	ohne dieses Attribut tragen UTC + LTO-Minuten.
	   epg. setAttribute ("tz", "local");
	   while (index < endPoint) {
	      switch (at (v, index)) {
	         case 0x04:		// process tokenTable
	            (void)process_tokenTable (v, index);
	            break;

	         case 0x06: {		// default language
	            XmlNode child = process_defaultLanguage (v, index);
	            epg. appendChild (child);
	            break;
	         }
	         case 0x20: {		// process ProgramGroups
	            XmlNode t = process_programmeGroups (v, index);
	            epg. appendChild (t);
	            break;
	         }
	         case 0x21: {		// process_schedule
	            XmlNode child = process_schedule (v,  index);
	            epg. appendChild (child);
	            break;
	         }
	         case 0x05:		// obsolete
	            (void)process_obsolete (v, index);
	            break;

	         default:
	            process_forgotten ("epg", v, index);
	            break;
	      }
	   }
	   xml = epg. toString ();
	   return scheduleType;
	}
	if (tag == SERVICE_TAG)	{	// superfluous test
	   XmlNode serviceInformation ("serviceInformation");
	   while (index < endPoint) {
	      switch (at (v, index)) {
	         case 0x04:		// process tokenTable
	            (void)process_tokenTable (v, index);
	            break;

	         case 0x06: {	// default language
	            XmlNode child = process_defaultLanguage (v, index);
	            serviceInformation. appendChild (child);
	            break;
	         }
	         case 0x26: {	// ensemble
	            XmlNode t = process_ensemble (v, index);
	            serviceInformation. appendChild (t);
	            break;
	         }
	         case 0x28: {	// process_service
	            XmlNode t = process_service (v, index);
	            serviceInformation. appendChild (t);
	            break;
	         }
	         case 0x80: {	// version
	            std::string s = process_483 (v, index);
	            if (s != "")
	               serviceInformation. setAttribute ("Version", s);
	            break;
	         }
	         case 0x81: {	// creation time
	   	    std::string s =  process_474 (v, index);
	            serviceInformation. setAttribute ("creationTime", s);
	            break;
	         }
	         case 0x82: {	// originator
	            std::string s = process_440 (v, index);
	            serviceInformation. setAttribute ("originator", s);
	            break;
	         }
	         case 0x83: {	// serviceProvider
	            std::string s =  process_440 (v, index);
	            serviceInformation. setAttribute ("serviceprovider", s);
	            break;
	         }
	         case 0x84: {	// not used
	            ignore (v, index);
	            break;
	         }
	         case 0x85: {	// alphabet
	            ignore (v, index);
	            break;
	         }
	         default:
	            process_forgotten ("serviceInformation:", v, index);
	            break;
	      }
	   }
	   xml = serviceInformation. toString ();
	   return serviceInformationType;
	}
	return noType;
}

XmlNode epgCompiler::process_defaultLanguage (const std::vector<uint8_t> &v,
	                                             int  &index) {
int endPoint	= setLength (v, index);
XmlNode child ("defaultLanguage");
	std::string res;
	for (int i = index; i < endPoint; i ++)
	   appendLatin1 (res, v [i]);

	child. appendText (res);
	index = endPoint;
	return child;
}

XmlNode  epgCompiler::process_shortName (const std::vector<uint8_t> &v,
	                                     int &index) {
int endPoint = setLength (v, index);
std::string s;
XmlNode child ("shortName");
	if (at (v, index) == 0x80) {	// xml:lang
	   ignore (v, index);
	}
	s = fetchString (v, index, endPoint);
	child. appendText (s);
	index = endPoint;
	return child;
}

XmlNode epgCompiler::process_mediumName (const std::vector<uint8_t> &v,
	                                    int &index) {
int endPoint = setLength (v, index);
XmlNode child ("mediumName");
	if (at (v, index) == 0x80)	// xml:lang
	   ignore (v, index);
	std::string res = fetchString (v, index, endPoint);
	if (res == "")
	   res = " ";
	child. appendText (res);
	index = endPoint;
	return child;
}

XmlNode epgCompiler::process_longName (const std::vector<uint8_t> &v,
	                                   int &index) {
int endPoint = setLength (v, index);
XmlNode child ("longName");
	if (at (v, index) == 0x80){	// xml:lang
	   ignore (v, index);
	}
	std::string res = fetchString (v, index, endPoint, false);
	if (res == "")
	   res = " ";
	child. appendText (res);
	index = endPoint;
	return child;
}

XmlNode epgCompiler::process_mediaDescription (const std::vector<uint8_t> &v,
	                                         int &index) {
int endPoint = setLength (v, index);
XmlNode t ("mediaDescription");
std::string res;
	switch (at (v, index)) {
	   case 0x1A: {
	      XmlNode tt = process_shortDescription (v, index);
	      t. appendChild (tt);
	      break;
	   }
	   case 0x1B: {
	      XmlNode tt = process_longDescription (v, index);
	      t. appendChild (tt);
	      break;
	   }
	   case 0x2B: {
	      XmlNode tt = process_multimedia (v, index);
	      t. appendChild (tt);
	      break;
	   }
	   default:
	      process_forgotten ("mediaDescription", v, index);
	      break;
        }
	index = endPoint;
	return t;
}
//
//	TS 102 371
XmlNode	epgCompiler::process_genre (const std::vector<uint8_t> &v,
	                                   int &index) {
int endPoint	= setLength (v, index);
static
const char *genres [] = {
"IntentionCS", "FormatCS", "ContentCS", "OriginationCS",
"IntendedAudienceCS", "ContentAlertCS", "KediaTupeCS", "AtmosphereCS"};
XmlNode t ("genre");
std::string s;
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x80: {	//	href attribute see TS 102 371 /5.4.5.4
	         int localEnd = setLength (v, index);
	         for (int i = index; i < localEnd; i ++) {
	            if (v [i] < 8)
	               s += std::string (genres [v [i]]) + ".";
	            else
	               appendLatin1 (s, v [i]);
	         }
	         t. setAttribute ("href", s);
	         index = localEnd;
	         break;
	      }
	      case 0x81: {	// type
	         int localEnd = setLength (v, index);
	         uint8_t xx = at (v, index);
	         s = xx == 0x01 ? "main" :
	             xx == 0x02 ? "secondary" : "other";
	         t. setAttribute ("type", s);
	         index =  localEnd;
	         break;
	      }
	      case 0x01: {
	         std::string s;
	         if (at (v, index + 1) == 1) {
//	Review G2: Token 0..19, wie bei den anderen Tabellenzugriffen
	            uint8_t tok = at (v, index + 2);
	            if (tok < 20)
	               s = stringTable [tok];
	         }
	         else {
	            std::string text;
	            for (int i = 0; i < at (v, index + 1); i++)
	               text. push_back ((char)at (v, index + 2 + i));
	            s = fromUtf8 (text);
	         }
	         index = endPoint;
	         t. appendText (s);
	         break;
	      }
	      default:
	         process_forgotten ("genre", v, index);
	         break;
	   }
	}
	index = endPoint;
	return t;
}

XmlNode epgCompiler::process_keyWords (const std::vector<uint8_t> &v,
	                                  int &index) {
int endPoint = setLength (v, index);
XmlNode child ("keyWords");
std::string res;
	if (at (v, index) == 0x80)	//  xml:lang
	   ignore (v, index);
	res = fetchString (v, index, endPoint);
	child. appendText (res);
	index = endPoint;
	return child;
}

XmlNode epgCompiler::process_memberOf (const std::vector<uint8_t> &v,
	                                  int &index) {
int endPoint = setLength (v, index);
XmlNode t ("memberOf");
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case  0x80: {	// 4.7.1, id
	         int localEnd = setLength (v, index);
	         std::string s = fetchString (v, index, endPoint);
	         t. setAttribute ("id", s);
	         index = localEnd;
	         break;
	      }
	      case 0x81: { 	// 4.7.2  shortId
	         std::string s = process_472 (v, index);
	         t. setAttribute ("shortId", s);
	         break;
	      }
	      case  0x82: {	// 5.8.2 index
	         std::string s = process_482 (v, index);
	         t. setAttribute ("index", s);
	         break;
	      }
	      default:
	         process_forgotten ("member_of", v, index);
	         break;
	   }
	}
	index = endPoint;
	return t;
}

XmlNode epgCompiler::process_link (const std::vector<uint8_t> &v,
	                              int &index) {
int endPoint = setLength (v, index);
XmlNode t ("link");
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x80: {	//uri
	         int localEnd = setLength (v, index);
	         std::string text;
	         for (int i = index; i < localEnd; i ++)
	            text. push_back ((char)v [i]);
	         std::string res = fromUtf8 (text);
	         t. setAttribute ("uri", res);
	         index = localEnd;
	         break;
	      }
	      case 0x81: {	// mime value 473
	         int localEnd = setLength (v, index);
	         std::string text;
	         for (int i = index; i < localEnd; i ++)
                    text. push_back ((char)v [i]);
                 std::string res = fromUtf8 (text);
	         t. setAttribute ("mime_value", res);
	         index = localEnd;
	         break;
	      }
	      case 0x82: {	// xml:lang	481
	         ignore (v, index);
	         break;
	      }
	      case 0x83: {	// description 440
	         int localEnd = setLength (v, index);
	         std::string res = fetchString (v, index, endPoint);
	         t. setAttribute ("description", res);
	         index = localEnd;
	         break;
	      }
	      case 0x84: {	// expiry time
	         std::string s = process_474 (v, index);
	         t. setAttribute ("expiryTime", s);
	         break;
	      }
	      default:
	         process_forgotten ("process_link", v, index);
	         break;
	   }
	}
	index = endPoint;
	return t;
}

XmlNode	epgCompiler::process_location (const std::vector<uint8_t> &v,
	                                      int &index) {
int endPoint	= setLength (v, index);
XmlNode location ("location");
	switch (at (v, index)) {
	   case 0x2D: {		// bearer
	      XmlNode t = process_bearer (v, index);
	      location. appendChild (t);
	      break;
	   }
	   case 0x2c: {		// time
	      XmlNode t  = process_time (v, index);
	      location. appendChild (t);
	      break;
	   }
	   case 0x2F:  {	// relative time
	      XmlNode t = process_relativeTime (v, index);
	      location. appendChild (t);
	      break;
	   }
	   default:
	      process_forgotten ("location", v, index);
	      location. setAttribute ("unknown time key", "XXX");
	      break;
	}
	index = endPoint;
	return location;
}

XmlNode epgCompiler::process_shortDescription (const std::vector<uint8_t> &v,
	                                           int &index) {
int endPoint = setLength (v, index);
XmlNode child ("shortDescription");
	if (at (v, index) == 0x80) // xml:lang
	   ignore (v, index);
	std::string s = fetchString (v, index, endPoint);
	child. appendText (s);
	index = endPoint;
	return child;
}

XmlNode epgCompiler::process_longDescription (const std::vector<uint8_t> &v,
	                                         int &index) {
int endPoint = setLength (v, index);
XmlNode child ("longDescription");
	if (at (v, index) == 0x80) // xml:lang
	   ignore (v, index);
	std::string s = fetchString (v, index, endPoint);
	child. appendText (s);
	index = endPoint;
	return child;
}

XmlNode epgCompiler::process_programme (const std::vector<uint8_t> &v,
	                                   int  &index) {
int	endPoint = setLength (v, index);
XmlNode program ("programme");
bool	recommended	= false;
bool	broadcasting	= false;
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x10: { 	// shortName
	         XmlNode child = process_shortName (v, index);
	         program. appendChild (child);
	         break;
	      }
	      case 0x11: {	// mediumName
	         XmlNode child = process_mediumName (v, index);
	         program. appendChild (child);
	         break;
	      }
	      case 0x12: {	// longName
	         XmlNode child = process_longName (v, index);
	         program. appendChild (child);
	         break;
	      }
	      case 0x13: {	// media description
	         XmlNode child = process_mediaDescription (v, index);
	         program. appendChild (child);
	         break;
	      }
	      case 0x14: {	// genre
	         XmlNode child = process_genre (v, index);
	         program. appendChild (child);
	         break;
	      }
	      case 0x16: {	// keyWords
	         XmlNode child = process_keyWords (v, index);
	         program. appendChild (child);
	         break;
	      }
	      case 0x17: {	// memberOf
	         XmlNode t =  process_memberOf (v, index);
	         program. appendChild (t);
	         break;
	      }
	      case 0x18: {	// link
	         XmlNode link = process_link (v, index);
	         program. appendChild (link);
	         break;
	      }
	      case 0x19: {	// location
	         XmlNode child = process_location (v, index);
	         program. appendChild (child);
	         break;
	      }
	      case 0x36: {	// onDemand
	         XmlNode child = process_onDemand (v, index);
	         program. appendChild (child);
	         break;
	      }
	      case 0x2A: {	// presentation language
	         ignore (v, index);
	         break;
	      }
	      case 0x39: {	// alias
	         ignore (v, index);
	         break;
	      }
	      case 0x3A: {	// phoneme
	         ignore (v, index);
	         break;
	      }
	      case 0x80: {	// 4.7.1 Id
	          std::string s = process_471 (v, index);
	          program. setAttribute ("id", s);
	          break;
	      }
	      case 0x81: {	// 472, shortId
	         std::string s = process_472 (v, index);
	         program. setAttribute ("shortId", s);
	         break;
	      }
	      case 0x82: {	// 4.8.3 version
	         std::string s = process_483 (v, index);
	         program.  setAttribute ("version", s);
	         break;
	      }
	      case 0x83: {	// 4.6, recommendation
	        recommended	= true;
	        std::string s = process_recommendation (v, index);
	        program. setAttribute ("recommendation", s);
	        break;
	      }
	      case 0x84: {	// 4.6, broadcast
	         broadcasting	= true;
	         std::string s = process_broadcast (v, index);
	         program. setAttribute ("broadcast", s);
	         break;
	      }
	      case 0x86: {	// 481, xml:lang
	         ignore (v, index);
	         break;
	      }
	      default:
	         process_forgotten ("programme", v, index);
	         break;
	   }
	}
	index	= endPoint;
	if (!broadcasting)
	   program. setAttribute ("broadcast", "on-air");
	if (!recommended)
	   program. setAttribute ("recommendation", "no");
	return program;
}

XmlNode epgCompiler::process_programmeGroups (const std::vector<uint8_t> &v,
	                                          int &index) {
int endPoint = setLength (v, index);
XmlNode t ("programGroups");
std::string	s;
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x23: {	// programmegroup
	         XmlNode c = process_programmeGroup (v, index);
	         t. appendChild (c);
	         break;
	      }
	      case 0x80: {	// version
	         std::string s = process_483 (v, index);
	         t.  setAttribute ("version", s);
	         break;
	      }
	      case 0x81: {	// creation time
	   	 std::string s =  process_474 (v, index);
	         t. setAttribute ("creationTime", s);
	         break;
	      }
	      case 0x82: {	// originator
	         std::string s = process_440 (v, index);
	         t. setAttribute ("originator", s);
	         break;
	      }

	      default:
	         process_forgotten ("programGroups", v, index);
	   }
	}
	if (!t. hasAttribute ("version"))
	   t. setAttribute ("version", 1);
	index = endPoint;
	return t;
}

XmlNode epgCompiler::process_schedule (const std::vector<uint8_t> &v,
	                                  int &index) {
int endPoint	= setLength (v, index);
XmlNode schedule ("schedule");
	while (index < endPoint) {
	   std::string s;
	   switch (at (v, index)) {
	      case 0x11: {	// mediumname
	         XmlNode child = process_mediumName (v, index);
	         schedule. appendChild (child);
	         break;
	      }
	      case 0x12: {	// longName
	         XmlNode child = process_longName (v, index);
	         schedule. appendChild (child);
	         break;
	      }
	      case 0x13: {	// mediaDescription
	         XmlNode child = process_mediaDescription (v, index);
	         schedule. appendChild (child);
	         break;
	      }
	      case 0x17: {	// memberOf
	         XmlNode t =  process_memberOf (v, index);
	         schedule. appendChild (t);
	         break;
	      }
	      case 0x19: {	// location
	         XmlNode t = process_location (v, index);
	         schedule. appendChild (t);
	         break;
	      }
	      case 0x1C: {	// programme
	         XmlNode t =  process_programme (v, index);
	         schedule. appendChild (t);
	         break;
	      }
	      case 0x24: {	// scope
	         XmlNode t = process_scope (v, index);
	         schedule. appendChild (t);
	         break;
	      }
	      case 0x2A: {	// presentation language
	         ignore (v, index);
	         break;
	      }
	      case 0x80: {	// version
	         std::string s = process_483 (v, index);
	         schedule. setAttribute ("version", s);
	         break;
	      }
	      case 0x81: {	// creation time
	   	 std::string s =  process_474 (v, index);
	         schedule. setAttribute ("creationTime", s);
	         break;
	      }
	      case 0x82: {	// originator
	         std::string s = process_440 (v, index);
	         schedule. setAttribute ("originator", s);
	         break;
	      }
	      case 0x83: {	// alphabet
	         ignore (v, index);
	         break;
	      }
	      default:
	         process_forgotten ("schedule", v, index);
	         break;
	   }
	}
	if (!schedule. hasAttribute ("version"))
	   schedule. setAttribute ("version", 1);
	index	= endPoint;
	return schedule;
}

XmlNode epgCompiler::process_programmeGroup (const std::vector<uint8_t> &v,
	                                        int &index) {
int endPoint = setLength (v, index);
XmlNode t ("programGroup");
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x10: {		// shortName
	         XmlNode child = process_shortName (v, index);
	         t. appendChild (child);
	         break;
	      }
	      case 0x11: {		// mediumName
	         XmlNode child = process_mediumName (v, index);
	         t. appendChild (child);
	         break;
	      }
	      case 0x12: {		// longName
	         XmlNode child = process_longName (v, index);
	         t. appendChild (child);
	         break;
	      }
	      case 0x13: {		// mediaDescription
	         XmlNode child = process_mediaDescription (v, index);
	         t. appendChild (child);
	         break;
	      }
	      case 0x14: {		// genre
	         XmlNode child = process_genre (v, index);
	         t. appendChild (child);
	         break;
	      }
	      case 0x16: {		// keyWords
	         XmlNode child = process_keyWords (v, index);
	         t. appendChild (child);
	         break;
	      }
	      case 0x17: {		// memberOf
	         XmlNode child = process_memberOf (v, index);
	         t. appendChild (child);
	         break;
	      }
	      case 0x18: {		// link
	         XmlNode child = process_link (v, index);
	         t. appendChild (child);
	         break;
	      }
	      case 0x80: {		// id (v1 schreibt das Attribut "version")
	         std::string s = process_471 (v, index);
	         t. setAttribute ("version", s);
	         break;
	      }
	      case 0x81: {		// shortId
	   	 std::string s =  process_472 (v, index);
	         t. setAttribute ("shortId", s);
	         break;
	      }
	      case 0x82: {		// version
	         std::string s = process_483 (v, index);
	         t. setAttribute ("version", s);
	         break;
	      }
	      case 0x83: {		// type
	         std::string s = process_groupType (v, index);
	         t. setAttribute ("type", s);
	         break;
	      }
	      case 0x64: {	//  numOfOtems 484
	         std::string s = process_484 (v, index);
	         t. setAttribute ("numOfItems", s);
	         break;
	      }
	      default:
	         process_forgotten ("programGroup", v, index);
	   }
	}
	if (!t. hasAttribute ("version"))
	   t. setAttribute ("version", 1);
	index = endPoint;
	return t;
}

XmlNode epgCompiler::process_scope (const std::vector<uint8_t> &v,
	                               int &index) {
int endPoint	= setLength (v, index);
XmlNode scope ("scope");
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x25: {	// serviceScope
	         XmlNode x = process_serviceScope (v, index);
	         scope. appendChild (x);
	         break;
	      }
	      case 0x80: { //	startTime
	         std::string startTime = process_474 (v, index);
	         scope. setAttribute ("startTime", startTime);
	         break;
	      }
	      case 0x81: {	// stopTime
	         std::string stopTime = process_474 (v, index);
	         scope. setAttribute ("stopTime", stopTime);
	         break;
	      }
	      default:
	         process_forgotten ("scope", v, index);
	         break;
	   }
	}
	index = endPoint;
	return scope;
}

XmlNode epgCompiler:: process_serviceScope (const std::vector<uint8_t> &v,
	                                       int &index) {
int endPoint	= setLength (v, index);
XmlNode serviceScope ("serviceScope");
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x80: {	// Id, 476
	         std::string res = process_476 (v, index);
	         serviceScope. setAttribute ("id", res);
	         size_t p = res. rfind (':');
	         if (p != std::string::npos)
	            lastScopeSid = (uint32_t) std::strtoul (res. c_str () + p + 1, nullptr, 16);
	         break;
	      }
	      default:
	         process_forgotten ("serviceScope", v, index);
	         break;
	   }
	}
	index = endPoint;
	return serviceScope;
}

XmlNode epgCompiler::process_ensemble (const std::vector<uint8_t> &v,
	                                  int &index) {
int endPoint	= setLength (v, index);
XmlNode ensemble ("ensemble");
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x10: {	// shortName
	         XmlNode child = process_shortName (v, index);
	         ensemble. appendChild (child);
	         break;
	      }
	      case 0x11: {	// mediumName
	         XmlNode child = process_mediumName (v, index);
	         ensemble. appendChild (child);
	         break;
	      }
	      case 0x12: {	// longname
	         XmlNode child = process_longName (v, index);
	         ensemble. appendChild (child);
	         break;
	      }
	      case 0x13: {	// media description
	         XmlNode child = process_mediaDescription (v, index);
	         ensemble. appendChild (child);
	         break;
	      }
	      case 0x16: {	// keyWords
	         XmlNode child = process_keyWords (v, index);
	         ensemble. appendChild (child);
	         break;
	      }
	      case 0x18: {	// link
	         XmlNode child = process_link (v, index);
	         ensemble. appendChild (child);
	         break;
	      }
	      case 0x37: {	// not used anymore
	         ignore (v, index);
	         break;
	      }
	      case 0x28: {	// service
	         XmlNode child = process_service (v, index);
	         ensemble. appendChild (child);
	         break;
	      }
	      case 0x80: {	// id
	         int localEnd = setLength (v, index);
	         int ecc = at (v, index);
	         int Eid = (at (v, index + 1) << 8) | at (v, index + 2);
	         char buf [16];
	         snprintf (buf, sizeof (buf), "%x", ecc);
	         ensemble. setAttribute ("ecc", buf);
	         snprintf (buf, sizeof (buf), "%x", Eid);
	         ensemble. setAttribute ("Eid", buf);
	         index = localEnd;
	         break;
	      }
	      case 0x81: {	// not used
	         ignore (v, index);
	         break;
	      }
	      default:
	         process_forgotten ("ensemble", v, index);
	         break;
	   }
	}
	index = endPoint;
	return ensemble;
}

XmlNode epgCompiler::process_service (const std::vector<uint8_t> &v,
	                                 int &index) {
int endPoint	= setLength (v, index);
XmlNode service ("service");

	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x10: {	// shortName
	         XmlNode child = process_shortName (v, index);
	         service. appendChild (child);
	         break;
	      }
	      case 0x11: {	// mediumName
	         XmlNode child = process_mediumName (v, index);
	         service. appendChild (child);
	         break;
	      }
	      case 0x12: {	// longname
	         XmlNode child = process_longName (v, index);
	         service. appendChild (child);
	         break;
	      }
	      case 0x13: {	// media description
	         XmlNode child = process_mediaDescription (v, index);
	         service. appendChild (child);
	         break;
	      }
	      case 0x14: {	// genre
	         XmlNode child = process_genre (v, index);
	         service. appendChild (child);
	         break;
	      }
	      case 0x16: {	// keyWords
	         XmlNode child = process_keyWords (v, index);
	         service. appendChild (child);
	         break;
	      }
	      case 0x2A: {	// presentation language
	         ignore (v, index);
	         break;
	      }
	      case 0x29: {	// bearer
	         XmlNode child = process_bearer (v, index);
	         service. appendChild (child);
	         break;
	      }
	      case 0x31: {	// radiodns
	         XmlNode child =  process_radiodns (v, index);
	         service. appendChild (child);
	         break;
	      }
	      case 0x32: {	// geolocation
	         XmlNode child = process_geolocation (v, index);
	         service. appendChild (child);
	         break;
	      }
	      case 0x39: {	//alias
	         ignore (v, index);
	         break;
	      }
	      case 0x3A: {	// phoneme
	         ignore (v, index);
	         break;
	      }
	      case 0x80: {	// version
	         std::string s = process_483 (v, index);
	         service. setAttribute ("version", s);
	         break;
	      }
	      default:
	         process_forgotten ("service", v, index);
	         break;
	   }
	}
	return service;
}

XmlNode	epgCompiler::process_bearer (const std::vector<uint8_t> &v,
	                                    int &index) {
int endPoint = setLength (v, index);
XmlNode bearer ("bearer");
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x80: {	// id, 476
	         std::string res = process_476 (v, index);
	         bearer. setAttribute ("id", res);
	         break;
	      }
	      case 0x82: {	// url, 440
	         int localEnd = setLength (v, index);
	         std::string s = fetchString (v, index, localEnd);
	         bearer. setAttribute ("url", s);
	         index = localEnd;
	         break;
	      }
	      default:
	         process_forgotten ("bearer", v, index);
	         break;
	   }
	}
	index = endPoint;
	return bearer;
}

XmlNode epgCompiler::process_multimedia (const std::vector<uint8_t> &v,
	                                    int &index) {
int endPoint = setLength (v, index);
XmlNode multimedia ("multimedia");
std::string s;
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x80: {	// mime value	473
	         int localEnd = setLength (v, index);
	         std::string text;
	         for (int i = index; i < localEnd; i ++)
	            text . push_back ((char)v [i]);
                 std::string res  = fromUtf8 (text);
                 multimedia. setAttribute ("mime_value", res);
                 index = localEnd;
	         break;
	      }
	      case 0x81: {	// xml:lang	ignore
	         ignore (v, index);
	         break;
	      }
	      case 0x82: { 	//440,  url
	         int localEnd = setLength (v, index);
	         std::string text;
	         for (int i = index; i < localEnd; i ++)
	            text . push_back ((char)v [i]);
	         s = fromUtf8 (text);
	         multimedia. setAttribute ("url", s);
	         index = localEnd;
	         break;
	      }
	      case 0x83: {	// 46, type
	         int localEnd = setLength (v, index);
	         switch (at (v, index)) {
	            case 0x02:	// logo_unrestricted
	               s = "logo unrestricted";
	               break;
	            case 0x03:	// Not used
	            case 0x05:
	            default:
	               s = "notUsed";
	               break;
	            case 0x04:
	               s = "logo_colour_square";
	               break;
	            case 0x06:
	               s = "logo_colour_rectangle";
	               break;
	         }
	         multimedia. setAttribute ("type", s);
	         index = localEnd;
	         break;
	      }

	      case 0x84: {	// width	485
	         std::string s = process_485 (v, index);
	         multimedia. setAttribute ("width", s);
	         break;
	      }

	      case 0x85: {	// height	485
	         std::string s = process_485 (v, index);
	         multimedia. setAttribute ("height", s);
	         break;
	      }
	      default:
	         process_forgotten ("multimedia", v, index);
	         break;
	   }
	}
//	if (!multimedia. hasAttribute ("height"))
//	   multimedia. setAttribute ("height", 0);
//	if (!multimedia. hasAttribute ("width"))
//	   multimedia. setAttribute ("width", 0);
	index = endPoint;
	return multimedia;
}

XmlNode epgCompiler::process_time (const std::vector<uint8_t> &v,
	                              int &index) {
int endPoint = setLength (v, index);
XmlNode time ("time");
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x80: {	// time
	         std::string s = process_474 (v, index);
	         time. setAttribute ("time", s);
	         break;
	      }
	      case 0x81: {	// duration
	         std::string s = process_475 (v, index);
	         time. setAttribute ("duration", s);
	         break;
	      }
	      case 0x82: {	// actual time
	         std::string s = process_474 (v, index);
	         time. setAttribute ("actualTime", s);
	         break;
	      }
	      case 0x83: {	// actal duration
	         std::string s = process_475 (v, index);
	         time. setAttribute ("actualDuration", s);
	         break;
	      }
	      default:
	         process_forgotten ("time", v, index);
	         break;
	   }
	}
	index = endPoint;
	return time;
}

XmlNode epgCompiler::process_relativeTime (const std::vector<uint8_t> &v,
	                                      int &index) {
int endPoint = setLength (v, index);
XmlNode t ("relativeTime");
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x80: {	// time
	         std::string s = process_474 (v, index);
	         t. setAttribute ("time", s);
	         break;
	      }
	      case 0x81: {	// duration
	         std::string s = process_475 (v, index);
	         t. setAttribute ("duration", s);
	         break;
	      }
	      case 0x82: {	// actual time
	         std::string s = process_474 (v, index);
	         t. setAttribute ("actualTime", s);
	         break;
	      }
	      case 0x83: {	// actal duration
	         std::string s = process_475 (v, index);
	         t. setAttribute ("actualDuration", s);
	         break;
	      }
	      default:
	         process_forgotten ("time", v, index);
	         break;
	   }
	}
	index = endPoint;
	return t;
}

XmlNode epgCompiler::process_programmeEvent (const std::vector<uint8_t> &v,
	                                        int &index) {
int endPoint = setLength (v, index);
XmlNode programmeEvent ("link");		// v1: createElement ("link")
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x10: {	// shortName
	         XmlNode child = process_shortName (v, index);
	         programmeEvent. appendChild (child);
	         break;
	      }
	      case 0x11: {	// mediumname
	         XmlNode child = process_mediumName (v, index);
	         programmeEvent. appendChild (child);
	         break;
	      }
	      case 0x12: {	//longName
	         XmlNode child = process_longName (v, index);
	         programmeEvent. appendChild (child);
	         break;
	      }
	      case 0x13: {	// mediaDescription
	         XmlNode s = process_mediaDescription (v, index);
	         programmeEvent. appendChild (s);
	         break;
	      }
	      case 0x14: {	// genre
	         XmlNode s = process_genre (v, index);
	         programmeEvent. appendChild (s);
	         break;
	      }
	      case 0x16: {	//keywords
	         XmlNode s = process_keyWords (v, index);
	         programmeEvent. appendChild (s);
	         break;
	      }
	      case 0x17: {	// memberOf
	         XmlNode s = process_memberOf (v, index);
	         programmeEvent. appendChild (s);
	         break;
	      }
	      case 0x18: {	// link
	         XmlNode s = process_link (v, index);
	         programmeEvent. appendChild (s);
	         break;
	      }
	      case 0x19: {	// location
	         XmlNode s = process_location (v, index);
	         programmeEvent. appendChild (s);
	         break;
	      }
	      case 0x2A: {	// presentationLanguage
	         ignore (v, index);
	         break;
	      }
	      case 0x36: {	// onDemand
	         XmlNode s = process_onDemand (v, index);
	         programmeEvent. appendChild (s);
	         break;
	      }
	      case 0x39: {	// alias
	         ignore (v, index);
	         break;
	      }
	      case 0x3A: {	// phoneme
	         ignore (v, index);
	         break;
	      }
//
//	the attributes
	      case 0x80: {	// Id
	         std::string s = process_471 (v, index);
	         programmeEvent. setAttribute ("id", s);
	         break;
	      }
	      case 0x81: {	//shortId
	         std::string s = process_472 (v, index);
	         programmeEvent. setAttribute ("shortId", s);
	         break;
	      }
	      case 0x82: {	// version
	         std::string s = process_483 (v, index);
	         programmeEvent. setAttribute ("version", s);
	         break;
	      }
	      case 0x83: {	// recommendation
	        std::string s = process_recommendation (v, index);
	        programmeEvent. setAttribute ("recommendation", s);
	        break;
	      }
	      case 0x84: {	// broadcast
	         std::string s = process_broadcast (v, index);
	         programmeEvent. setAttribute ("broadcast", s);
	         break;
	      }
	      case 0x86: {	//xml:lang 481
	         ignore (v, index);
	         break;
	      }
	      default:
	         process_forgotten ("programma event", v, index);
	         break;
	   }
	}
	index = endPoint;
	return programmeEvent;
}

XmlNode epgCompiler::process_radiodns (const std::vector<uint8_t> &v,
	                                  int &index) {
int endPoint = setLength (v, index);
XmlNode radiodns ("radiodns");
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x80: {	// fqdn
	         ignore (v, index);
	         break;
	      }
	      case 0x81: {	// service identifier
	         ignore (v, index);
	         break;
	      }
	      default:
	         process_forgotten ("radiodns", v, index);
	         break;
	   }
	}
	index = endPoint;
	return radiodns;
}

XmlNode epgCompiler::process_geolocation (const std::vector<uint8_t> &v,
	                                     int &index) {
int endPoint = setLength (v, index);
XmlNode geolocation ("geolocation");
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x33: { 	// country
	         XmlNode t = process_country (v, index);
	         geolocation. appendChild (t);
	         break;
	      }
	      case 0x34: {	// point
//	         XmlNode t = process_point (v, index);
//	         geolocation. appendChild (t);
	         ignore (v, index);	// not yet implemented
	         break;
	      }
	      case 0x35: {	// polygon
//	         XmlNode t = process_polygon (v, index);
//	         geolocation. appendChild (t);
	         ignore (v, index);	// not yet implemented
	         break;
	      }
	      case 0x80: { 	// xml:id
	         ignore (v, index);
	         break;
	      }
	      case 0x81: {	// ref
	         ignore (v, index);
	         break;
	      }
	      default:
	         process_forgotten ("geolocation", v, index);
	         break;
	   }
	}
	index = endPoint;
	return geolocation;
}

XmlNode epgCompiler::process_country (const std::vector<uint8_t> &v,
	                                 int &index) {
int endPoint = setLength (v, index);
XmlNode child ("country");
	std::string res =  fetchString (v, index, endPoint);
        child. appendText (res);
	index = endPoint;
	return child;
}

XmlNode epgCompiler::process_point (const std::vector<uint8_t> &v,
	                               int &index) {
int endPoint = setLength (v, index);
XmlNode child ("point");
	index = endPoint;
	return child;
}

XmlNode epgCompiler::process_polygon (const std::vector<uint8_t> &v,
	                                 int &index) {
int endPoint = setLength (v, index);
XmlNode t ("polygon");
	index = endPoint;
	return t;
}

XmlNode epgCompiler::process_onDemand (const std::vector<uint8_t> &v,
	                                  int &index) {
int endPoint = setLength (v, index);
XmlNode onDemand ("onDemand");
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x2D: {	// bearer
	         XmlNode t = process_bearer (v, index);
	         onDemand. appendChild (t);
	         break;
	      }
	      case 0x37: {	// presentationTime
	         XmlNode t = process_presentationTime (v, index);
	         onDemand. appendChild (t);
	         break;
	      }
	      case 0x38: {	//acquisitionTime
	         XmlNode t = process_acquisitionTime (v, index);
	         onDemand. appendChild (t);
	         break;
	      }
	      default:
	         process_forgotten ("onDemand", v, index);
	         break;
	   }
	}
	index = endPoint;
	return onDemand;
}

XmlNode epgCompiler::process_presentationTime (const std::vector<uint8_t> &v,
	                                         int &index) {
int endPoint = setLength (v, index);
XmlNode t ("oresentationTime");		// v1-Schreibweise beibehalten
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x80: {	// start
	         std::string s = process_474 (v, index);
	         t. setAttribute ("start", s);
	         break;
	      }
	      case 0x81: {	// end
	         std::string s = process_474 (v, index);
	         t. setAttribute ("end", s);
	         break;
	      }
	      case 0x82: {	// duration
	         std::string s = process_475 (v, index);
	         t. setAttribute ("duration", s);
	         break;
	      }
	      default:
	         process_forgotten ("presentationTime", v, index);
	         break;
	   }
	}
	index = endPoint;
	return t;
}

XmlNode epgCompiler::process_acquisitionTime (const std::vector<uint8_t> &v,
	                                         int &index) {
int endPoint = setLength (v, index);
XmlNode t ("acquisitionTime");
	while (index < endPoint) {
	   switch (at (v, index)) {
	      case 0x80: {	// start
	         std::string s = process_474 (v, index);
	         t. setAttribute ("start", s);
	         break;
	      }
	      case 0x81: {	// end
	         std::string s = process_474 (v, index);
	         t. setAttribute ("end", s);
	         break;
	      }
	      default:
	         process_forgotten ("acquisitionTime", v, index);
	         break;
	   }
	}
	index = endPoint;
	return t;
}

std::string	epgCompiler::process_440	(const std::vector<uint8_t> &v, int &index) {
int endPoint	= setLength (v, index);
std::string res 	= fetchString (v, index, endPoint);
	index = endPoint;
	return res;
}

//	CRId datatype
std::string	epgCompiler::process_471	(const std::vector<uint8_t> &v, int &index) {
int endPoint	= setLength (v, index);
std::string s	= fetchString (v, index, endPoint);
	index	= endPoint;
	return s;
}

std::string epgCompiler::process_472 (const std::vector<uint8_t> &v, int &index) {
int endPoint = setLength (v, index);
uint32_t res = (at (v, index) << 16) | (at (v, index + 1) << 8) | at (v, index + 2);
	index = endPoint;
	return std::to_string (res);
}

static
std::string	twoDigits (int16_t v) {
	if (v >= 10)
	   return std::to_string (v);
	else
	   return '0' + std::to_string (v);
}
//
//	ETSI TS 102 371: 4.7.4 time point
//	Die uebertragenen Stunden/Minuten sind UTC. v1 nahm sie per mktime als
//	Ortszeit und addierte dann noch den LTO (Stunden!) als Minuten – am
//	Bundesmux ergab das UTC + 2 min statt Ortszeit. V3: Sendezeit als UTC
//	(timegm/_mkgmtime), keine LTO-Addition, Ausgabe in der Ortszeit des
//	Systems (localtime, inkl. Sommerzeit); Format yyyy-M-dTHH:mm wie v1.
//	Die Wurzel <epg> traegt dafuer tz="local" (Unterscheidung zu v1-Dateien).
std::string epgCompiler::process_474 (const std::vector<uint8_t> &v, int &index) {
int endPoint = setLength (v, index);
	uint32_t mjd	= getBits (v, 8 * index + 1, 17);
	uint16_t dateOut [4];
	convertTime (mjd, dateOut);
	int16_t Y	= dateOut [0];
	int16_t M	= dateOut [1];
	int16_t D	= dateOut [2];
//	we need to know whether it is today or not
//	int ltoFlag	= getBit (v, 8 * index + 19);
//	int utcFlag	= getBit (v, 8 * index + 20);
//	int ltoBase	= utcFlag == 1 ? 48 : 32;
	int hours	= getBits (v, 8 * index + 21, 5);
	int minutes	= getBits (v, 8 * index + 26, 6);
	struct tm tmIn;
	memset (&tmIn, 0, sizeof (tmIn));
	tmIn. tm_year	= Y - 1900;
	tmIn. tm_mon	= M - 1;
	tmIn. tm_mday	= D;
	tmIn. tm_hour	= hours;
	tmIn. tm_min	= minutes;
	tmIn. tm_sec	= 0;
	tmIn. tm_isdst	= 0;
#ifdef _WIN32
	time_t t2 = _mkgmtime (&tmIn);
#else
	time_t t2 = timegm (&tmIn);
#endif
	struct tm tmOut;
#ifdef _WIN32
	localtime_s (&tmOut, &t2);
#else
	localtime_r (&t2, &tmOut);
#endif
	Y	= tmOut. tm_year + 1900;
	M	= tmOut. tm_mon + 1;
	D	= tmOut. tm_mday;
	hours = tmOut. tm_hour;
	minutes = tmOut. tm_min;
	index = endPoint;
	std::string res = std::to_string (Y) + "-" +
	                       std::to_string (M) + "-" +
	                       std::to_string (D) + "T" +
	                       twoDigits (hours) + ":" +
	                       twoDigits (minutes);
	return res;
}

//	ETSI TS 102 371: 4.7.5 Duration type
std::string	epgCompiler::process_475	(const std::vector<uint8_t> &v,
	                                              int &index) {
int endPoint	= setLength (v, index);

	int duration	= (at (v, index) << 8) | at (v, index + 1);
	int minutes	= (duration / 60) % 60;
	int hours	= duration / 3600;
	std::string res	= "PT";
	if (hours > 0)
	   res = res + twoDigits (hours) + "H";
	res += twoDigits (minutes) + "M";
	index = endPoint;
	return res;
}

//	BearerURI type
std::string	epgCompiler::process_476	(const std::vector<uint8_t> &v, int &index) {
int endPoint	= setLength (v, index);
	uint8_t ecc = at (v, index + 1);
	uint16_t eid = (at (v, index + 2) << 8) | at (v, index + 3);
	uint32_t SId = 0;
//	Review 16.09.2026 G3: Bit-Test wie getBit () oben mit "&"; v1 hatte
//	"%" (prueft das untere Nibble = SCIdS statt Bit 4 = lange SId) - bei
//	Sekundaerkomponenten wurde die SId dadurch 32 statt 16 Bit gelesen
//	und der serviceScope traf einen falschen Dienst.
	int upTo = (at (v, index) & bitTable [3]) ? 4 : 2;
	for (int i = 0; i < upTo; i ++) {
	   SId = SId << 8;
	   SId |= at (v, index + 4 + i);
	}
	char buf [40];
	snprintf (buf, sizeof (buf), "%x:%x:%x", ecc, eid, SId);
	std::string result = buf;
	index = endPoint;
	return result;
}
//	index
std::string	epgCompiler::process_482	(const std::vector<uint8_t> &v, int &index) {
int endPoint	= setLength (v, index);
uint16_t res	= (at (v, index) << 8) | at (v, index + 1);
	index	= endPoint;
	return std::to_string (res);
}
//	version
std::string	epgCompiler::process_483	(const std::vector<uint8_t> &v, int &index) {
int endPoint	= setLength (v, index);
	int numbers = (at (v, index) << 8) | at (v, index + 1);
	index	= endPoint;
	return std::to_string (numbers);
}
//	numof Items
std::string	epgCompiler::process_484	(const std::vector<uint8_t> &v, int  &index) {
int endPoint	= setLength (v, index);
	int numbers = (at (v, index) << 8) | at (v, index + 1);
	index	= endPoint;
	return std::to_string (numbers);
}

//	v1 liest zweimal v [index] (statt v [index + 1]) - beibehalten
std::string epgCompiler::process_485 (const std::vector<uint8_t> &v, int &index) {
int endPoint	= setLength (v, index);
	int size	= (at (v, index) << 8) | at (v, index);
	index = endPoint;
	return std::to_string (size);
}
//
//	strong table handling
//
void	epgCompiler::process_tokenTable	(const std::vector<uint8_t> &v,
	                                 int  &index) {
int endPoint	= setLength (v, index);
//	token table element, section 4.9
	while (index < endPoint)
	   process_token (v, index);
	index = endPoint;
}

void	epgCompiler::process_token (const std::vector<uint8_t> &v,
	                                             int  &index) {
uint8_t tag	= at (v, index);
int endPoint	= setLength (v, index);
//int	length	= endPoint - index;
	std::string text;
	for (int i = index; i < endPoint; i ++) {
	   text. push_back ((char)v [i]);
	}
	if (tag < 20) {
	   stringTable [tag] = fromUtf8 (text);
	}
	index = endPoint;
}

void	epgCompiler::process_obsolete (const std::vector<uint8_t> &v,
	                              int &index) {
int endPoint	= setLength (v, index);
	index	= endPoint;
}

void	epgCompiler::process_forgotten (const std::string &s,
	                               const std::vector<uint8_t> &v,
	                               int &index) {
int key	= at (v, index);
int endPoint	= setLength (v, index);
	(void)s;
	if (theErrorLogger)
	   theErrorLogger ("Translating epg, unknown key " +
	                                    std::to_string (key));
	index = endPoint;
}

std::string epgCompiler::process_broadcast  (const std::vector<uint8_t> &v,
	                                int &index) {
int endPoint	= setLength (v, index);
	uint8_t valueByte = at (v, index);
	index	= endPoint;
	return (valueByte == 0x01) ? "on-air" :  "off-air";
}

std::string	epgCompiler::process_recommendation (const std::vector<uint8_t> &v,
	                                    int &index) {
int endPoint	= setLength (v, index);
	int valueByte = at (v, index);
	index = endPoint;
	return valueByte == 0x01 ? "no" : "yes";
}

std::string epgCompiler::process_groupType	(const std::vector<uint8_t> &v,
	                                 int &index) {
int endPoint	= setLength (v, index);
static
const char *typeList [] = {
	"series", "show", "programConcept", "magazine", "programCompilation",
	"otherCollection", "otherChoise", "topic"};

	uint8_t valueByte = at (v, index);
	index = endPoint;
	if ((0x02 <= valueByte) && (valueByte <= 0x09))
	   return std::string (typeList [valueByte - 2]);
	else
	   return "unknown Type";
}
//
//	for the "not Used" and the not yet implemented
void	epgCompiler::ignore	(const std::vector<uint8_t> &v, int &index) {
int endPoint	= setLength (v, index);
	index	= endPoint;
}

//	v1: Token aus der stringTable werden per QString::toLatin1 eingefuegt
//	(Zeichen > 0xFF -> '?'), das Ergebnis geht durch decodeSmartly.
std::string	epgCompiler::fetchString (const std::vector<uint8_t> &v,
	                                  int &index, int endPoint, bool p) {
std::string res = "";
	(void)p;
	if (at (v, index) != 1) {	// should not happen, but it does
	   std::string text;
	   for (int i = index; i < endPoint; i ++) {
	      if (v [i] < 20) {
	         std::string ss = utf8ToLatin1 (stringTable [v [i]]);
	         for (int j = 0; j < (int)ss. size () && ss [j] != 0; j ++)
	            text. push_back (ss [j]);
	      }
	      else
	         text. push_back ((char)v [i]);
	   }
	   res = decodeSmartly (text);
	}
	else {
	   std::string text;
	   int localEnd = setLength (v, index);
	   for (int i = index; i < localEnd; i ++) {
	      if (v [i] < 20) {
	         std::string ss = utf8ToLatin1 (stringTable [v [i]]);
	         for (int j = 0; j < (int)ss. size () && ss [j] != 0; j ++)
	            text. push_back (ss [j]);
	      }
	      else
	         text. push_back ((char)v [i]);
	   }
	   res = decodeSmartly (text);
	}
	index = endPoint;
	return res;
}
