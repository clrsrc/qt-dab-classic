// DAB Classic v3: portiert aus Qt-DAB sources/output/portaudio/audiosink.cpp
// (Jan van Katwijk, GPLv2+), siehe portaudio-sink.h.
#
/*
 *    Copyright (C) 2014 .. 2025
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

//
//	Output implementation using portaudio
//
#ifdef	DABCORE_AUDIO_PORTAUDIO
#include	"portaudio-sink.h"
#include	<cstdio>

	PortAudioSink::PortAudioSink	(int16_t latency):
	                           _O_Buffer (65536) {
	this	-> latency	= latency;
	if (latency <= 0)
	   this -> latency = 1;

	this	-> CardRate	= 48000;
	portAudio		= false;
	writerRunning		= false;
	ostream			= nullptr;
	theMissed		= 0;
	numofDevices		= 0;
	currentIndex		= -1;
	currentPaDevice		= -1;
	if (Pa_Initialize() != paNoError) {
	   fprintf (stderr, "Initializing Pa for output failed\n");
	   return;
	}

	portAudio	= true;
	numofDevices	= Pa_GetDeviceCount();
	outTable. resize (numofDevices + 1);
	for (int i = 0; i < numofDevices; i ++)
	   outTable [i] = -1;
}

	PortAudioSink::~PortAudioSink () {
	closeStream ();
	if (portAudio)
	   Pa_Terminate();
}

void	PortAudioSink::closeStream () {
	if ((ostream != nullptr) && !Pa_IsStreamStopped (ostream)) {
	   paCallbackReturn = paAbort;
	   (void) Pa_AbortStream (ostream);
	   while (!Pa_IsStreamStopped (ostream))
	      Pa_Sleep (1);
	   writerRunning = false;
	}

	if (ostream != nullptr)
	   Pa_CloseStream (ostream);
	ostream = nullptr;
}

bool	PortAudioSink::openDevice (int outputDevice) {
PaError err;

	if (!portAudio)
	   return false;
	if (outputDevice < 0 || outputDevice >= numofDevices) {
	   fprintf (stderr, "invalid device (%d) selected\n", outputDevice);
	   return false;
	}
	closeStream ();

	outputParameters. device		= outputDevice;
	outputParameters. channelCount		= 2;
	outputParameters. sampleFormat		= paFloat32;
	outputParameters. suggestedLatency	=
	                          Pa_GetDeviceInfo (outputDevice) ->
	                                      defaultHighOutputLatency;
	bufSize	= (int)((float)outputParameters. suggestedLatency * latency);
//
//	A small buffer causes more callback invocations, sometimes
//	causing underflows and intermittent output.
	outputParameters. hostApiSpecificStreamInfo = nullptr;
//
	err = Pa_OpenStream (&ostream,
	                     nullptr,
	                     &outputParameters,
	                     CardRate,
	                     bufSize,
	                     0,
	                     this	-> paCallback_o,
	                     this
	      );

	if (err != paNoError) {
	   fprintf (stderr, "Open ostream error: %s\n", Pa_GetErrorText (err));
	   ostream = nullptr;
	   return false;
	}
	_O_Buffer. FlushRingBuffer ();
	paCallbackReturn = paContinue;
	err = Pa_StartStream (ostream);
	if (err != paNoError) {
	   fprintf (stderr, "Open startstream error: %s\n", Pa_GetErrorText (err));
	   return false;
	}
	writerRunning	= true;
	currentPaDevice	= outputDevice;
	return true;
}

bool	PortAudioSink::start () {
	if (!portAudio)
	   return false;
	if (ostream != nullptr && !Pa_IsStreamStopped (ostream))
	   return true;
	int dev = currentPaDevice >= 0 ? currentPaDevice : Pa_GetDefaultOutputDevice ();
	if (dev < 0) {
	   fprintf (stderr, "kein PortAudio-Ausgabegeraet\n");
	   return false;
	}
	return openDevice (dev);
}

void	PortAudioSink::stop () {
	if (ostream == nullptr || Pa_IsStreamStopped (ostream))
	   return;

	(void)Pa_StopStream	(ostream);
	while (!Pa_IsStreamStopped (ostream))
	   Pa_Sleep (1);
	writerRunning		= false;
	_O_Buffer. FlushRingBuffer ();
}
//
//	helper
bool	PortAudioSink::OutputrateIsSupported (int16_t device, int32_t Rate) {
PaStreamParameters outputParameters;

	outputParameters. device		= device;
	outputParameters. channelCount		= 2;	/* I and Q	*/
	outputParameters. sampleFormat		= paFloat32;
	outputParameters. suggestedLatency	= 0;
	outputParameters. hostApiSpecificStreamInfo = nullptr;

	return Pa_IsFormatSupported (nullptr, &outputParameters, Rate) ==
	                                          paFormatIsSupported;
}
/*
 * 	... and the callback
 */

int	PortAudioSink::paCallback_o (
		const void*			inputBuffer,
                void*				outputBuffer,
		unsigned long			framesPerBuffer,
		const PaStreamCallbackTimeInfo	*timeInfo,
	        PaStreamCallbackFlags		statusFlags,
	        void				*userData) {
RingBuffer<float>	*outB;
float	*outp		= (float *)outputBuffer;
PortAudioSink *ud	= reinterpret_cast <PortAudioSink *>(userData);
uint32_t	actualSize;
uint32_t	i;
	(void)statusFlags;
	(void)inputBuffer;
	(void)timeInfo;
	if (ud -> paCallbackReturn == paContinue) {
	   outB = &(ud -> _O_Buffer);
	   actualSize = outB -> getDataFromBuffer (outp, 2 * framesPerBuffer);
	   ud -> theMissed	+= 2 * framesPerBuffer - actualSize;
	   for (i = actualSize; i < 2 * framesPerBuffer; i ++)
	      outp [i] = 0;
	}

	return ud -> paCallbackReturn;
}

uint32_t	PortAudioSink::takeMissed	() {
	return theMissed. exchange (0);
}

//	Timeshift (Plan M4 1.3): beim Sprung auf live den Ausgabepuffer
//	verwerfen; der Stream laeuft weiter (Latenz), er bekommt nur nichts
//	Altes mehr. Der Underrun-Zaehler wird mit zurueckgesetzt.
void	PortAudioSink::flush	() {
	_O_Buffer. FlushRingBuffer ();
	theMissed. store (0);
}
//
//	we call this with the amount of floats!!
void	PortAudioSink::write	(const float *b, uint32_t amount) {
	if (!writerRunning)
	   return;
	if (_O_Buffer. GetRingBufferWriteAvailable () < amount)
	   amount = _O_Buffer. GetRingBufferWriteAvailable () & ~01;
	_O_Buffer. putDataIntoBuffer (b, amount);
}

std::string PortAudioSink::outputChannelwithRate (int16_t ch, int32_t rate) {
const PaDeviceInfo *deviceInfo;
std::string name;

	if ((ch < 0) || (ch >= numofDevices))
	   return name;

	deviceInfo = Pa_GetDeviceInfo (ch);
	if (deviceInfo == nullptr)
	   return name;
	if (deviceInfo -> maxOutputChannels <= 0)
	   return name;

	if (OutputrateIsSupported (ch, rate))
	   name = deviceInfo -> name;
	return name;
}

std::vector<std::string>	PortAudioSink::devices () {
uint16_t	ocnt	= 0;
std::vector<std::string> res;

	if (!portAudio)
	   return res;
	for (int i = 0; i <  numofDevices; i ++) {
	   const std::string so = outputChannelwithRate (i, 48000);
	   if (so != "") {
	      res. push_back (so);
	      outTable [ocnt] = i;
	      ocnt ++;
	   }
	}
	return res;
}

int	PortAudioSink::currentDevice () {
	int dev = currentPaDevice >= 0 ? currentPaDevice : Pa_GetDefaultOutputDevice ();
	(void)devices ();
	for (int i = 0; i < numofDevices; i ++)
	   if (outTable [i] == dev)
	      return i;
	return -1;
}

bool	PortAudioSink::selectDevice (int index) {
	if (!portAudio)
	   return false;
	auto list = devices ();
	if (index < 0 || index >= (int)list. size ())
	   return false;
	bool wasRunning = writerRunning;
	int dev = outTable [index];
	if (wasRunning || ostream != nullptr)
	   return openDevice (dev);
	currentPaDevice = dev;
	return true;
}
#endif
