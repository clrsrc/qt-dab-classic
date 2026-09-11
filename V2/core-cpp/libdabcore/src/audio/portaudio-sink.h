// DAB Classic v3: portiert aus Qt-DAB sources/output/portaudio/audiosink.h
// (Jan van Katwijk, GPLv2+): QString/QStringList/QComboBox gestrichen,
// Geraetewahl ueber Index in der 48-kHz-Geraeteliste, Ring 65 536 Float
// (~0,7 s) statt 16 * 32 768 (~5,4 s). PortAudio-Callback unveraendert.
#
/*
 *    Copyright (C)  2014 .. 2023
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *    Lazy Chair Computing
 *
 *    This file is part of the Qt-DAB.
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
#ifdef	DABCORE_AUDIO_PORTAUDIO

#include	<vector>
#include	<string>
#include	<atomic>
#include	"dab-constants.h"
#include	<portaudio.h>
#include	<cstdio>
#include	"audio-sink.h"
#include	"ringbuffer.h"

class	PortAudioSink : public IAudioSink {
public:
	                PortAudioSink		(int16_t latency = 1);
			~PortAudioSink		() override;
	bool		start			() override;
	void		stop			() override;
	void		write			(const float *, uint32_t) override;
	std::vector<std::string> devices	() override;
	int		currentDevice		() override;
	bool		selectDevice		(int index) override;
	uint32_t	takeMissed		() override;
	const char	*name			() const override { return "portaudio"; }
	bool		ok			() const { return portAudio; }
private:
	bool		openDevice		(int paDevice);
	void		closeStream		();
	bool		OutputrateIsSupported	(int16_t, int32_t);
	std::string	outputChannelwithRate	(int16_t, int32_t);
	int32_t		CardRate;
	int16_t		latency;
	bool		portAudio;
	bool		writerRunning;
	int16_t		numofDevices;
	int		paCallbackReturn;
	int16_t		bufSize;
	PaStream	*ostream;
	RingBuffer<float>	_O_Buffer;
	PaStreamParameters	outputParameters;
	std::atomic<uint32_t>	theMissed;
	std::vector<int16_t>	outTable;	// Listenindex -> PortAudio-Geraet
	int		currentIndex;
	int		currentPaDevice;
protected:
static	int		paCallback_o	(const void	*input,
	                                 void		*output,
	                                 unsigned long	framesperBuffer,
	                                 const PaStreamCallbackTimeInfo *timeInfo,
					 PaStreamCallbackFlags statusFlags,
	                                 void		*userData);
};
#endif
