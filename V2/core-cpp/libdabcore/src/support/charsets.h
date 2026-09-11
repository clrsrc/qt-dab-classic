// DAB Classic v3: portiert aus Qt-DAB sources/support/charsets.h
// (Jan van Katwijk / Przemyslaw Wegrzyn, GPLv2+), Qt entfernt:
// Ergebnis ist std::string in UTF-8 statt QString.
#
/*
 *    Copyright (C) 2016 .. 2026
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
 *
 *	This charset handling was kindly added by Przemyslaw Wegrzyn
 *	all rights acknowledged
 */
#pragma once

#include <string>
#include <cstdint>

/*
 * Codes assigned to character sets, as defined
 * in ETSI TS 101 756 v1.6.1, section 5.2.
 */
typedef enum {
    EbuLatin	= 0x00, // Complete EBU Latin based repertoire - see annex C
    UnicodeUcs2	= 0x06,
    UnicodeUtf8 = 0x0F
} CharacterSet;

/**
 * Converts the character string to UTF-8, using a given character set.
 *
 * @param buffer    buffer to convert (null-terminated if size == -1)
 * @param charset   character set used in buffer
 * @param size      number of bytes, -1 = strlen
 * @return converted UTF-8 string
 */
std::string toStringUsingCharset (const char* buffer, CharacterSet charset, int size = -1);

//	Hilfsfunktion: einen Unicode-Codepoint als UTF-8 anhaengen
void	appendUtf8 (std::string &s, uint32_t codepoint);
