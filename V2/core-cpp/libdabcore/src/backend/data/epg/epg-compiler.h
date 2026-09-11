// DAB Classic v3: portiert aus Qt-DAB sources/backend/data/epg/epg-compiler.h
// (Jan van Katwijk, GPLv2+): QObject gestrichen, QDomDocument/QDomElement ->
// XmlNode, QString -> std::string (UTF-8), errorLogger -> Log-Callback.
// Die Binaer-EPG-Auswertung (ETSI TS 102 371) ist Bit-fuer-Bit wie v1,
// einschliesslich der dortigen Eigenheiten (siehe Kommentare in epg-compiler.cpp).
#
/*
 *    Copyright (C) 2013 .. 2024
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
 *    along with Qt-TAB; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#pragma once

#include	<stdio.h>
#include	<stdint.h>
#include	<stdlib.h>
#include	<functional>
#include	<string>
#include	<vector>
#include	"xml-node.h"

#define		noType			0
#define		scheduleType		1
#define		serviceInformationType	2

class	epgCompiler {
public:
		epgCompiler	(std::function<void(const std::string &)> logger = nullptr);
		~epgCompiler	();

//	Liefert den Dokumenttyp; xml enthaelt danach den Text des Dokuments
//	(QDomDocument::toString (1)-Format, siehe xml-node.h). lto wie v1:
//	der LTO-Wert aus FIG 0/9 (ganze Stunden), den v1 in process_474
//	als Minuten addiert.
int	process_epg	(std::string &xml,
	                 const std::vector<uint8_t> &v, int lto);
private:
	std::string	stringTable [20];
	std::function<void(const std::string &)> theErrorLogger;
//
//	element handlers
	XmlNode	process_defaultLanguage	(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_shortName	(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_mediumName	(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_longName	(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_mediaDescription	(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_genre		(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_keyWords	(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_memberOf	(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_link		(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_location	(const std::vector<uint8_t> &v, int &index);

	XmlNode	process_shortDescription (const std::vector<uint8_t> &v, int &index);
	XmlNode	process_longDescription	(const std::vector<uint8_t> &v, int &index);

	XmlNode	process_programme	(const std::vector<uint8_t> &v, int &index);
	XmlNode 	process_programmeGroups	(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_schedule	(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_programmeGroup	(const std::vector<uint8_t> &v, int &index);
	XmlNode 	process_scope		(const std::vector<uint8_t> &v, int &index);
	XmlNode 	process_serviceScope	(const std::vector<uint8_t> &v, int &index);
	XmlNode 	process_ensemble	(const std::vector<uint8_t> &v, int &index);
	XmlNode 	process_service		(const std::vector<uint8_t> &v, int &index);

	XmlNode	process_bearer		(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_multimedia	(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_time		(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_programmeEvent	(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_relativeTime	(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_radiodns	(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_geolocation	(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_country		(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_point		(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_polygon		(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_onDemand	(const std::vector<uint8_t> &v, int &index);
	XmlNode	process_presentationTime (const std::vector<uint8_t> &v, int &index);
	XmlNode	process_acquisitionTime	(const std::vector<uint8_t> &v, int &index);

//
//	attribute handlers
	std::string	process_440		(const std::vector<uint8_t> &v, int &index);
	std::string	process_471		(const std::vector<uint8_t> &v, int &index);
	std::string	process_472		(const std::vector<uint8_t> &v, int &index);
	std::string	process_474		(const std::vector<uint8_t> &v, int &index);
	std::string	process_475		(const std::vector<uint8_t> &v, int &index);
	std::string	process_476		(const std::vector<uint8_t> &v, int &index);
	std::string	process_482		(const std::vector<uint8_t> &v, int &index);
	std::string	process_483		(const std::vector<uint8_t> &v, int &index);
	std::string	process_484		(const std::vector<uint8_t> &v, int &index);
	std::string	process_485		(const std::vector<uint8_t> &v, int &index);

	void	process_tokenTable	(const std::vector<uint8_t> &v, int &index);
	void	process_token		(const std::vector<uint8_t> &v, int &index);

	void	process_obsolete	(const std::vector<uint8_t> &v, int &index);

	std::string	fetchString		(const std::vector<uint8_t> &v,
	                                  int &index, int endPoint, bool p = false);
	void	ignore			(const std::vector<uint8_t> &v,
	                                  int &index);
	void    process_forgotten	(const std::string &s,
	                                const std::vector<uint8_t> &v,
	                                int &index);

	std::string process_broadcast	(const std::vector<uint8_t> &v,
	                                 int &index);
	std::string	process_recommendation	(const std::vector<uint8_t> &v,
	                                 int &index);
	std::string process_groupType	(const std::vector<uint8_t> &v,
	                                 int &index);

	int lto;
};
