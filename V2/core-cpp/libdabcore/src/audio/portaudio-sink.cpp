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
// Windows-Header VOR portaudio-sink.h: dab-constants.h zieht "using namespace
// std" nach, danach ist "byte" in rpcndr.h/wtypes.h mehrdeutig (std::byte).
#ifdef _WIN32
#include	<windows.h>
#include	<mmdeviceapi.h>
#include	<pa_win_wasapi.h>
#endif
#include	"portaudio-sink.h"
#include	<cstdio>
#include	<cstring>
#include	<chrono>

#ifdef _WIN32
// Endpoint-ID (WCHAR) nach UTF-8; PortAudio liefert die Namen selbst
// schon als UTF-8.
static std::string wideToUtf8 (const wchar_t *w) {
	if (w == nullptr) return {};
	int n = WideCharToMultiByte (CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if (n <= 1) return {};
	std::string out ((size_t)n - 1, '\0');
	WideCharToMultiByte (CP_UTF8, 0, w, -1, out. data (), n, nullptr, nullptr);
	return out;
}
#endif

//	WASAPI im Shared Mode nimmt ohne dieses Flag nur die Mischrate des
//	Geraets an (oft 44,1 kHz); mit paWinWasapiAutoConvert wandelt Windows
//	selbst nach 48 kHz, und Geraet wie Pruefung (Pa_IsFormatSupported)
//	akzeptieren unsere feste Rate. Fuer andere Host-APIs nullptr.
static void *wasapiInfo (PaDeviceIndex dev) {
#ifdef _WIN32
	static PaWasapiStreamInfo info;
	const PaDeviceInfo *d = Pa_GetDeviceInfo (dev);
	if (d == nullptr || Pa_GetHostApiInfo (d -> hostApi) -> type != paWASAPI)
	   return nullptr;
	memset (&info, 0, sizeof info);
	info. size		= sizeof info;
	info. hostApiType	= paWASAPI;
	info. version		= 1;
	info. flags		= paWinWasapiAutoConvert;
	return &info;
#else
	(void)dev;
	return nullptr;
#endif
}

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
	currentPaDevice		= paNoDevice;
	missingLogged_		= false;
	quit_			= false;
	if (Pa_Initialize() != paNoError) {
	   fprintf (stderr, "Initializing Pa for output failed\n");
	   return;
	}

	portAudio	= true;
	std::lock_guard<std::mutex> lk (m_);
	enumerate ();
	currentPaDevice	= resolve ();
#ifdef _WIN32
	watcher_ = std::thread ([this] { watch (); });
#endif
}

	PortAudioSink::~PortAudioSink () {
	{
	   std::lock_guard<std::mutex> lk (watchM_);
	   quit_ = true;
	}
	watchCv_. notify_all ();
	if (watcher_. joinable ())
	   watcher_. join ();
	std::lock_guard<std::mutex> lk (m_);
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

bool	PortAudioSink::openDevice (PaDeviceIndex outputDevice) {
PaError err;

	if (!portAudio)
	   return false;
	if (outputDevice < 0 || outputDevice >= Pa_GetDeviceCount ()) {
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
	outputParameters. hostApiSpecificStreamInfo = wasapiInfo (outputDevice);
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
	std::lock_guard<std::mutex> lk (m_);
	if (ostream != nullptr && !Pa_IsStreamStopped (ostream))
	   return true;
	PaDeviceIndex dev = currentPaDevice != paNoDevice ? currentPaDevice : resolve ();
	if (dev == paNoDevice) {
	   fprintf (stderr, "kein PortAudio-Ausgabegeraet\n");
	   return false;
	}
	return openDevice (dev);
}

void	PortAudioSink::stop () {
	std::lock_guard<std::mutex> lk (m_);
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
bool	PortAudioSink::OutputrateIsSupported (PaDeviceIndex device, int32_t Rate) {
PaStreamParameters outputParameters;

	outputParameters. device		= device;
	outputParameters. channelCount		= 2;	/* I and Q	*/
	outputParameters. sampleFormat		= paFloat32;
	outputParameters. suggestedLatency	= 0;
	outputParameters. hostApiSpecificStreamInfo = wasapiInfo (device);

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

//	Stabile Kennung eines Geraets: unter Windows/WASAPI die Endpoint-ID
//	("{0.0.0.00000000}.{guid}", dieselbe wie im Crossmixer), sonst
//	"<hostapi>:<name>".
std::string PortAudioSink::idOf (PaDeviceIndex dev, PaHostApiIndex api, const char *apiName) {
#ifdef _WIN32
	if (Pa_GetHostApiInfo (api) -> type == paWASAPI) {
	   void *p = nullptr;
	   if (PaWasapi_GetIMMDevice (dev, &p) == paNoError && p != nullptr) {
	      auto *imm = static_cast<IMMDevice *>(p);
	      LPWSTR w = nullptr;
	      std::string id;
	      if (SUCCEEDED (imm -> GetId (&w)) && w != nullptr) {
	         id = wideToUtf8 (w);
	         CoTaskMemFree (w);
	      }
	      if (!id. empty ())
	         return id;
	   }
	}
#endif
	(void)api;
	const PaDeviceInfo *info = Pa_GetDeviceInfo (dev);
	return std::string (apiName) + ":" + (info != nullptr ? info -> name : "");
}

//	Liste aufbauen: nur Ausgabegeraete mit 48 kHz Stereo; unter Windows nur
//	aus dem WASAPI-Host-API (jedes Geraet genau einmal, Standardgeraet des
//	Systems bekannt), auf anderen Systemen alle Host-APIs.
void	PortAudioSink::enumerate () {
	list_. clear ();
	if (!portAudio)
	   return;
	PaHostApiIndex only = paHostApiNotFound;
	PaDeviceIndex defaultDev = Pa_GetDefaultOutputDevice ();
#ifdef _WIN32
	only = Pa_HostApiTypeIdToHostApiIndex (paWASAPI);
	if (only >= 0)
	   defaultDev = Pa_GetHostApiInfo (only) -> defaultOutputDevice;
#endif
	const int n = Pa_GetDeviceCount ();
	for (PaDeviceIndex i = 0; i < n; i ++) {
	   const PaDeviceInfo *info = Pa_GetDeviceInfo (i);
	   if (info == nullptr || info -> maxOutputChannels <= 0)
	      continue;
	   if (only >= 0 && info -> hostApi != only)
	      continue;
	   if (!OutputrateIsSupported (i, CardRate))
	      continue;
	   const PaHostApiInfo *api = Pa_GetHostApiInfo (info -> hostApi);
	   Entry e;
	   e. paDevice	= i;
	   e. info. name	= info -> name;
	   e. info. id	= idOf (i, info -> hostApi, api != nullptr ? api -> name : "");
	   e. info. isDefault	= (i == defaultDev);
	   list_. push_back (e);
	}
}

//	Geraet, das fuer wantedId_ zu benutzen ist: die id, sonst (nicht
//	angesteckt oder "" = Standard) das Standardgeraet des Systems.
PaDeviceIndex	PortAudioSink::resolve (bool *found) {
	if (found != nullptr)
	   *found = wantedId_. empty ();
	PaDeviceIndex def = paNoDevice;
	for (const auto &e : list_) {
	   if (!wantedId_. empty () && e. info. id == wantedId_) {
	      if (found != nullptr)
	         *found = true;
	      return e. paDevice;
	   }
	   if (e. info. isDefault)
	      def = e. paDevice;
	}
	if (def == paNoDevice && !list_. empty ())
	   def = list_. front (). paDevice;
	if (!wantedId_. empty () && !missingLogged_) {
	   missingLogged_ = true;
	   fprintf (stderr, "Audiogeraet nicht angeschlossen, Standard verwendet\n");
	}
	return def;
}

std::vector<dabcore::AudioDeviceInfo>	PortAudioSink::devices () {
	std::lock_guard<std::mutex> lk (m_);
	std::vector<dabcore::AudioDeviceInfo> res;
	for (const auto &e : list_)
	   res. push_back (e. info);
	return res;
}

std::string	PortAudioSink::currentDevice () {
	std::lock_guard<std::mutex> lk (m_);
	PaDeviceIndex dev = currentPaDevice != paNoDevice ? currentPaDevice : resolve ();
	for (const auto &e : list_)
	   if (e. paDevice == dev)
	      return e. info. id;
	return {};
}

bool	PortAudioSink::selectDevice (const std::string &id) {
	if (!portAudio)
	   return false;
	std::lock_guard<std::mutex> lk (m_);
	if (id != wantedId_)
	   missingLogged_ = false;
	wantedId_ = id;
	bool found = false;
	PaDeviceIndex dev = resolve (&found);
	if (dev == paNoDevice)
	   return false;
	if (dev != currentPaDevice) {
	   if (writerRunning || ostream != nullptr)
	      openDevice (dev);
	   else
	      currentPaDevice = dev;
	}
	return found;
}

//	PortAudio kennt nach Pa_Initialize nur die damals vorhandenen Geraete;
//	neu einlesen heisst Pa_Terminate + Pa_Initialize (der Stream wird dabei
//	geschlossen und auf dem passenden Geraet wieder geoeffnet).
bool	PortAudioSink::reinitLocked () {
	if (!portAudio)
	   return false;
	const bool wasRunning = writerRunning || ostream != nullptr;
	closeStream ();
	Pa_Terminate ();
	if (Pa_Initialize () != paNoError) {
	   fprintf (stderr, "Pa_Initialize nach Geraetewechsel fehlgeschlagen\n");
	   portAudio = false;
	   list_. clear ();
	   currentPaDevice = paNoDevice;
	   return false;
	}
	enumerate ();
	currentPaDevice = paNoDevice;
	PaDeviceIndex dev = resolve ();
	if (dev == paNoDevice)
	   return false;
	if (wasRunning)
	   openDevice (dev);
	else
	   currentPaDevice = dev;
	return true;
}

void	PortAudioSink::refreshDevices () {
	std::lock_guard<std::mutex> lk (m_);
	reinitLocked ();
}

void	PortAudioSink::setChangeHandler (std::function<void()> h) {
	std::lock_guard<std::mutex> lk (m_);
	changeHandler_ = std::move (h);
}

//	Kennzeichen des Systemzustands: Standard-Endpoint + alle aktiven
//	Render-Endpoints. Aendert es sich, ist neu einzulesen.
std::string	PortAudioSink::endpointSignature () {
#ifdef _WIN32
	std::string sig;
	IMMDeviceEnumerator *en = nullptr;
	if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
	                              __uuidof (IMMDeviceEnumerator), (void **)&en)) || en == nullptr)
	   return sig;
	IMMDevice *def = nullptr;
	if (SUCCEEDED (en -> GetDefaultAudioEndpoint (eRender, eMultimedia, &def)) && def != nullptr) {
	   LPWSTR w = nullptr;
	   if (SUCCEEDED (def -> GetId (&w)) && w != nullptr) {
	      sig += wideToUtf8 (w);
	      CoTaskMemFree (w);
	   }
	   def -> Release ();
	}
	sig += "|";
	IMMDeviceCollection *coll = nullptr;
	if (SUCCEEDED (en -> EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &coll)) && coll != nullptr) {
	   UINT cnt = 0;
	   coll -> GetCount (&cnt);
	   for (UINT i = 0; i < cnt; i ++) {
	      IMMDevice *d = nullptr;
	      if (SUCCEEDED (coll -> Item (i, &d)) && d != nullptr) {
	         LPWSTR w = nullptr;
	         if (SUCCEEDED (d -> GetId (&w)) && w != nullptr) {
	            sig += wideToUtf8 (w);
	            sig += ";";
	            CoTaskMemFree (w);
	         }
	         d -> Release ();
	      }
	   }
	   coll -> Release ();
	}
	en -> Release ();
	return sig;
#else
	return {};
#endif
}

//	Waechter (nur Windows): alle 2 s die Endpoints vergleichen; bei
//	Aenderung Liste neu einlesen, Ausgabe umhaengen und den Kern
//	benachrichtigen (audio_devices). Der Handler laeuft ohne m_, weil er
//	selbst devices()/currentDevice() ruft.
void	PortAudioSink::watch () {
#ifdef _WIN32
	const bool com = SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED));
	std::string last = endpointSignature ();
	for (;;) {
	   {
	      std::unique_lock<std::mutex> lk (watchM_);
	      if (watchCv_. wait_for (lk, std::chrono::seconds (2), [this] { return quit_; }))
	         break;
	   }
	   std::string now = endpointSignature ();
	   if (now. empty () || now == last)
	      continue;
	   last = now;
	   std::function<void()> h;
	   {
	      std::lock_guard<std::mutex> lk (m_);
	      fprintf (stderr, "Audiogeraete geaendert, Liste neu eingelesen\n");
	      reinitLocked ();
	      h = changeHandler_;
	   }
	   if (h)
	      h ();
	}
	if (com)
	   CoUninitialize ();
#endif
}
#endif
