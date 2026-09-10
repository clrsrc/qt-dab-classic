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
#include	<QStringList>
#include	<set>
#include	<chrono>
//
#include	<cstdint>
#include	<cstdio>
#include	<QObject>
#include	<QByteArray>
#include	"msc-handler.h"
#include	<QMutex>

#include	"ensemble.h"
class	RadioInterface;
class	fibConfig;

class	fibDecoder: public QObject {
Q_OBJECT
public:
			fibDecoder		(RadioInterface *);
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
	int		getServiceComp		(const QString &);
	int		getServiceComp		(const uint32_t, const int);
	int		getServiceComp_SCIds	(const uint32_t, const int);
	bool		isPrimary		(const QString &);
	void		audioData		(const int, audiodata &);
	void		packetData		(const int, packetdata &);
	uint16_t	getAnnouncing		(uint16_t);
	std::vector<int>	getFrequency	(const QString &);
	void		getChannelInfo		(channel_data *, const int);
	bool		nonTIIFrame		();	
	void		getCIFcount		(int16_t &, int16_t &);
	uint32_t	julianDate		();
	int		freeSpace		();
	QList<contentType> contentPrint		();
	bool		is_SPI			(const uint32_t);
	std::vector<basicService> getServices	();
//	EWS: name of the (primary) service carried in a subchannel
	QString		serviceNameOnSubChannel	(int subChId);
protected:
	void		processFIB		(uint8_t *, uint16_t);
private:
	std::vector<serviceId> insert (std::vector<serviceId> &l,
                                          serviceId n, int order);
	RadioInterface	*myRadioInterface;
	ensemble	theEnsemble;
	fibConfig	*currentConfig;
	fibConfig	*nextConfig;
	void		adjustTime		(int32_t *dateTime);

	void		process_FIG0		(uint8_t *);
	void		process_FIG1		(uint8_t *);
	void		FIG0Extension0		(uint8_t *);
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

	QMutex		fibLocker;
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
	   uint8_t	iid;
	   QStringList	locations;	// of the current alert set
	   bool		setComplete;
	};
	ewsAlertState	theEws;
	bool		ewsHeartbeatSeen;
	uint32_t	ewsPreTriggerKey;
	uint32_t	ewsOtherEnsembleKey;
	int64_t		lastEwsAliveMs;
	QString		readLocationCode	(uint8_t *d, int &bitOffset,
	                                         int endBit, uint8_t &NFF);
	void		ewsAliveThrottled	(int subChId);

signals:
	void		addToEnsemble		(const QString &, int, int);
	void		ensembleName		(int, const QString &);
	void		clockTime		(int, int, int, int, int,
	                                                 int, int, int, int);
	void		changeinConfiguration	();
	void		announcement		(int, int);
	void		nrServices		(int);
	void		lto_ecc			(int, int);
	void		setFreqList		();
	void		tell_programType	(int, int);
	void		alarmFlagChanged	(bool active);
	void		ewfAlarm		(bool active, int subChId);
//	EWS (FIG 0/15): phase 0 Pre-trigger, 1 Trigger, 2 Sustain, 3 End
	void		ewsAlert		(int phase, int subChId,
	                                         int stage, int iid,
	                                         const QString &locations);
	void		ewsAlive		(int subChId);
	void		ewsPresent		();
};


