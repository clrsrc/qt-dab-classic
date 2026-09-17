// DAB Classic v3: portiert aus Qt-DAB sources/output/portaudio/audiosink.h
// (Jan van Katwijk, GPLv2+): QString/QStringList/QComboBox gestrichen,
// Ring 65 536 Float (~0,7 s) statt 16 * 32 768 (~5,4 s). PortAudio-Callback
// unveraendert.
// Geraetewahl (N1, 17.09.2026): Liste nur aus dem WASAPI-Host-API (sonst
// erscheint jedes Geraet unter MME/DirectSound/WASAPI/WDM-KS vierfach und
// der PortAudio-Index ist zwischen Sitzungen nicht stabil). Kennung je Geraet
// ist die WASAPI-Endpoint-ID (PaWasapi_GetIMMDevice -> IMMDevice::GetId), wie
// im Crossmixer. "" = Standardgeraet des Systems. Ein Waechter-Thread fragt
// alle 2 s die Endpoints ab (IMMDeviceEnumerator) und liest bei Aenderung die
// PortAudio-Liste neu ein (Pa_Terminate/Pa_Initialize, kurze Luecke), damit
// der Standard einem Wechsel in Windows folgt und ein spaeter angestecktes,
// gemerktes Geraet uebernommen wird.
#
#pragma once
#ifdef	DABCORE_AUDIO_PORTAUDIO

#include	<vector>
#include	<string>
#include	<atomic>
#include	<mutex>
#include	<thread>
#include	<condition_variable>
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
	std::vector<dabcore::AudioDeviceInfo> devices () override;
	std::string	currentDevice		() override;
	bool		selectDevice		(const std::string &id) override;
	void		refreshDevices		() override;
	void		setChangeHandler	(std::function<void()> h) override;
	uint32_t	takeMissed		() override;
	void		flush			() override;
	const char	*name			() const override { return "portaudio"; }
	bool		ok			() const { return portAudio; }
private:
	struct Entry {
	   dabcore::AudioDeviceInfo info;
	   PaDeviceIndex paDevice;
	};
	bool		openDevice		(PaDeviceIndex paDevice);	// m_ gehalten
	void		closeStream		();				// m_ gehalten
	void		enumerate		();				// m_ gehalten
	PaDeviceIndex	resolve			(bool *found = nullptr);	// m_ gehalten
	bool		reinitLocked		();				// m_ gehalten
	std::string	idOf			(PaDeviceIndex dev, PaHostApiIndex api, const char *apiName);
	bool		OutputrateIsSupported	(PaDeviceIndex, int32_t);
	void		watch			();
	std::string	endpointSignature	();
	int32_t		CardRate;
	int16_t		latency;
	bool		portAudio;
	std::atomic<bool>	writerRunning;
	int		paCallbackReturn;
	int16_t		bufSize;
	PaStream	*ostream;
	RingBuffer<float>	_O_Buffer;
	PaStreamParameters	outputParameters;
	std::atomic<uint32_t>	theMissed;
	std::mutex	m_;			// Pa-Init, Liste, Stream, Auswahl
	std::vector<Entry>	list_;		// Ergebnis von enumerate()
	std::string	wantedId_;		// "" = Standardgeraet
	PaDeviceIndex	currentPaDevice;	// offen bzw. beim Start zu oeffnen
	bool		missingLogged_;
	std::function<void()>	changeHandler_;
	std::thread	watcher_;
	std::mutex	watchM_;
	std::condition_variable	watchCv_;
	bool		quit_;
protected:
static	int		paCallback_o	(const void	*input,
	                                 void		*output,
	                                 unsigned long	framesperBuffer,
	                                 const PaStreamCallbackTimeInfo *timeInfo,
					 PaStreamCallbackFlags statusFlags,
	                                 void		*userData);
};
#endif
