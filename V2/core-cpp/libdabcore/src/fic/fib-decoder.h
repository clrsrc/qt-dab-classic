// DAB Classic v3: portiert aus Qt-DAB sources/frontend/fic-handling/fib-decoder.h (Jan van Katwijk, GPLv2+), Qt entfernt: QObject/Signale -> ReceiverCallbacks, QString -> std::string (UTF-8), QMutex -> std::mutex; FIG-Auswertung unveraendert.
#
/*
 *    Copyright (C) 2016 .. 2025
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
 *    along with Qt-TAB; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#
#pragma once
#include	<string>
#include	<set>
#include	<chrono>
//
#include	<cstdint>
#include	<cstdio>

#include	<mutex>
#include	<atomic>
#include	<vector>
#include	"ensemble.h"
#include	"receiver-callbacks.h"
class	fibConfig;

class	fibDecoder{
public:
			fibDecoder		(ReceiverCallbacks *);
			~fibDecoder		();

	void		clearEnsemble		();
	void		connectChannel		();
	void		disconnectChannel	();
	bool		syncReached		();
//
//	The real interface 
	uint32_t	getSId			(int);
	uint8_t		serviceType		(int);
	int		getNrComps		(const uint32_t);
//
//	not well chosen name, "subChannels" are meant
	int		nrChannels		();
	int		getServiceComp		(const std::string &);
	int		getServiceComp		(const uint32_t, const int);
	int		getServiceComp_SCIds	(const uint32_t, const int);
	bool		isPrimary		(const std::string &);
	void		audioData		(const int, audiodata &);
	void		packetData		(const int, packetdata &);
	uint16_t	getAnnouncing		(uint16_t);
	std::vector<int>	getFrequency	(const std::string &);
	void		getChannelInfo		(channel_data *, const int);
	bool		nonTIIFrame		();	
	void		getCIFcount		(int16_t &, int16_t &);
	uint32_t	julianDate		();
	int		freeSpace		();
	std::vector<contentType> contentPrint		();
	bool		is_SPI			(const uint32_t);
	bool		is_TPEG			(const uint32_t);
	std::vector<basicService> getServices	();
//	EWS: name of the (primary) service carried in a subchannel
	std::string		serviceNameOnSubChannel	(int subChId);
protected:
	void		processFIB		(uint8_t *, uint16_t);
private:
	ReceiverCallbacks *cb;
	bool		channelConnected;	// v1: connect/disconnect addToEnsemble
	std::string	shortLabel		(const std::string &utf8,
	                                         uint8_t *d, int flagOffset);
	ensemble	theEnsemble;
	fibConfig	*currentConfig;
	fibConfig	*nextConfig;
	void		adjustTime		(int32_t *dateTime);

	void		process_FIG0		(uint8_t *);
	void		process_FIG1		(uint8_t *);
	void		FIG0Extension0		(uint8_t *);
//	Rekonfiguration (Review M1): bekannte Dienste erneut melden
	void		reannounceServices	();
	void		FIG0Extension1		(uint8_t *);
	void		FIG0Extension2		(uint8_t *);
	void		FIG0Extension3		(uint8_t *);
//	void		FIG0Extension4		(uint8_t *);
	void		FIG0Extension5		(uint8_t *);
//	void		FIG0Extension6		(uint8_t *);
	void		FIG0Extension7		(uint8_t *);
	void		FIG0Extension8		(uint8_t *);
	void		FIG0Extension9		(uint8_t *);
	void		FIG0Extension10		(uint8_t *);
//	void		FIG0Extension11		(uint8_t *);
//	void		FIG0Extension12		(uint8_t *);
	void		FIG0Extension13		(uint8_t *);
	void		FIG0Extension14		(uint8_t *);
	void		FIG0Extension15		(uint8_t *);
//	void		FIG0Extension16		(uint8_t *);
	void		FIG0Extension17		(uint8_t *);
	void		FIG0Extension18		(uint8_t *);
	void		FIG0Extension19		(uint8_t *);
	void		FIG0Extension20		(uint8_t *);
	void		FIG0Extension21		(uint8_t *);
//	void		FIG0Extension22		(uint8_t *);
//	void		FIG0Extension23		(uint8_t *);
//	void		FIG0Extension24		(uint8_t *);
//	void		FIG0Extension25		(uint8_t *);
//	void		FIG0Extension26		(uint8_t *);

	int16_t		HandleFIG0Extension1	(uint8_t *,
	                                         int16_t,
	                                         const uint8_t,
	                                         const uint8_t,
	                                         const uint8_t);
	int16_t		HandleFIG0Extension2	(uint8_t *,
	                                         int16_t,
	                                         const uint8_t,
	                                         const uint8_t,
	                                         const uint8_t);
	int16_t		HandleFIG0Extension3	(uint8_t *,
	                                         int16_t,
	                                         const uint8_t,
	                                         const uint8_t,
	                                         const uint8_t);
	int16_t		HandleFIG0Extension5	(uint8_t *,
	                                         uint16_t,
	                                         const uint8_t,
	                                         const uint8_t,
	                                         const uint8_t);
	int16_t		HandleFIG0Extension8	(uint8_t *,
	                                         int16_t,
	                                         const uint8_t,
	                                         const uint8_t,
	                                         const uint8_t);
	int16_t		HandleFIG0Extension13	(uint8_t *,
	                                         int16_t,
	                                         const uint8_t,
	                                         const uint8_t,
	                                         const uint8_t);
	int16_t		HandleFIG0Extension20	(uint8_t *,
	                                         uint16_t,
	                                         const uint8_t,
	                                         const uint8_t,
	                                         const uint8_t);
	int16_t		HandleFIG0Extension21	(uint8_t*,
	                                         uint16_t,
	                                         const uint8_t,
	                                         const uint8_t,
	                                         const uint8_t);

	void		FIG1Extension0		(uint8_t *);
	void		FIG1Extension1		(uint8_t *);
//	void		FIG1Extension2		(uint8_t *);
//	void		FIG1Extension3		(uint8_t *);
	void		FIG1Extension4		(uint8_t *);
	void		FIG1Extension5		(uint8_t *);
	void		FIG1Extension6		(uint8_t *);

	std::recursive_mutex	fibLocker;
	std::atomic<int>	CIFcount;
	std::atomic<int16_t>	CIFcount_hi;
	std::atomic<int16_t>	CIFcount_lo;
	uint32_t	mjd;			// julianDate

	void		handleAnnouncement	(uint16_t SId,
	                                         uint16_t flags,
	                                         uint8_t SubChId);
	uint8_t		prevAlarmFlag;
//	EWF: alarm announcement state from FIG 0/19 (cluster 0xFF / ASw bit 0)
	bool		ewfAlarmActive;
	int		ewfAlarmSubChId;
	uint32_t	lastFig19Key;
	uint32_t	fig19Repeats;
	std::set<std::string>	seenFig15;
//	EWS (ETSI TS 104 089) alert state from FIG 0/15
	struct ewsAlertState {
	   bool		active;		// Trigger or Sustain phase seen
	   uint8_t	phase;
	   uint8_t	subChId;
	   uint8_t	stage;
	   uint8_t	stageRaw;	// rohes Status-Byte (Last/Stage/IId) der ersten Trigger-Instanz, Warntag 2026: 0x01
	   uint8_t	iid;
	   std::vector<std::string>	locations;	// of the current alert set
	   bool		setComplete;
	};
	ewsAlertState	theEws;
	bool		ewsHeartbeatSeen;
	uint32_t	ewsPreTriggerKey;
	uint32_t	ewsOtherEnsembleKey;
	int32_t		lastEwsAliveCIF;	// CIF-Zaehler statt Wanduhr (deterministisch im Replay)
	std::string		readLocationCode	(uint8_t *d, int &bitOffset,
	                                         int endBit, uint8_t &NFF);
	void		ewsAliveThrottled	(int subChId);
	void		logf			(const char *level, const char *fmt, ...);
	static std::string joinCodes		(const std::vector<std::string> &);
};


