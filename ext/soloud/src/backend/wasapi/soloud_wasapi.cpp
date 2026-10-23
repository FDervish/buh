/*
SoLoud audio engine
Copyright (c) 2013-2019 Jari Komppa

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

/*
Altered for buh, so this is not the original SoLoud source:

- The stream is 16-bit stereo, which Windows converts to whatever the device has (any speakers, any rate). SoLoud asked
  for 16-bit samples in the device's own channel count instead, which Windows refuses for more than two channels, so
  5.1 and 7.1 devices (headsets with virtual surround among them) had no sound at all; and it mixed stereo into that
  anyway. Where Windows can't convert, it falls back to the device's own channels and mixes for those.
- Opening the device again after the default one changes does the same, so it no longer spins when the new device has
  a different rate, and it waits a little between tries when there's no device.
- The audio thread initializes COM for itself, and COM is only uninitialized where it was initialized.
- Why the device couldn't be opened is kept in wasapi_last_error.
*/

#include "soloud.h"
#include "soloud_thread.h"

#if !defined(WITH_WASAPI)

namespace SoLoud
{
	result wasapi_init(Soloud *aSoloud, unsigned int aFlags, unsigned int aSamplerate, unsigned int aBuffer)
	{
		return NOT_IMPLEMENTED;
	}
};

#else

#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

// How long the audio thread waits before trying to open a device again
#define WASAPI_RETRY_MILLISECONDS 500

#define SAFE_RELEASE(x) \
    if (0 != (x)) \
    { \
        (x)->Release(); \
        (x) = 0; \
    }

namespace SoLoud
{
	// Why the device couldn't be opened, the last time it couldn't
	unsigned long wasapi_last_error = 0;

	// KSDATAFORMAT_SUBTYPE_PCM, without needing ksuser
	static const GUID WASAPI_SUBTYPE_PCM = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

    struct WASAPIData
    {
        IMMDeviceEnumerator *deviceEnumerator;
        IMMDevice *device;
        IAudioClient *audioClient;
        IAudioRenderClient *renderClient;
        HANDLE bufferEndEvent;
        HANDLE audioProcessingDoneEvent;
        Thread::ThreadHandle thread;
        Soloud *soloud;
        UINT32 bufferFrames;
        int channels;
		DWORD channelMask;
		REFERENCE_TIME duration;
		unsigned int sampleRate;
		volatile bool resetRequired;
		bool comInitialized;
		class MMNotificationClient *notificationClient;
    };

	class MMNotificationClient : public IMMNotificationClient
	{
	public:
		MMNotificationClient(WASAPIData *aData) : IMMNotificationClient(), mRef(1), mData(aData) {}

		// IUnknown methods -- AddRef, Release, and QueryInterface

		ULONG STDMETHODCALLTYPE AddRef()
		{
			return InterlockedIncrement(&mRef);
		}

		ULONG STDMETHODCALLTYPE Release()
		{
			ULONG ulRef = InterlockedDecrement(&mRef);
			if (0 == ulRef)
			{
				delete this;
			}
			return ulRef;
		}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID **ppvInterface)
		{
			if (IID_IUnknown == riid)
			{
				AddRef();
				*ppvInterface = (IUnknown*)this;
			}
			else if (__uuidof(IMMNotificationClient) == riid)
			{
				AddRef();
				*ppvInterface = (IMMNotificationClient*)this;
			}
			else
			{
				*ppvInterface = NULL;
				return E_NOINTERFACE;
			}
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR /*pwstrDeviceId*/, DWORD /*dwNewState*/) { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR /*pwstrDeviceId*/) { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR /*pwstrDeviceId*/) { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR /*pwstrDeviceId*/, const PROPERTYKEY /*key*/) { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole /*role*/, LPCWSTR /*pwstrDefaultDeviceId*/)
		{
			if (flow == eRender)
			{
				// The default rendering device just changed -> notify the thread to
				// recreate the audio graph
				mData->resetRequired = true;
				SetEvent(mData->bufferEndEvent);
			}
			return S_OK;
		}

	private:
		WASAPIData *mData;
		LONG mRef;
	};

	// The default device, and what it mixes: its rate, channels and speakers
	static HRESULT wasapiGetDevice(WASAPIData *aData, unsigned int *aSamplerate, unsigned int *aChannels, DWORD *aChannelMask)
	{
		SAFE_RELEASE(aData->renderClient);
		SAFE_RELEASE(aData->audioClient);
		SAFE_RELEASE(aData->device);
		HRESULT res = aData->deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &aData->device);
		if (SUCCEEDED(res))
		{
			res = aData->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, 0, reinterpret_cast<void**>(&aData->audioClient));
		}
		WAVEFORMATEX *mixFormat = 0;
		if (SUCCEEDED(res))
		{
			res = aData->audioClient->GetMixFormat(&mixFormat);
		}
		if (FAILED(res))
		{
			return res;
		}
		*aSamplerate = mixFormat->nSamplesPerSec;
		*aChannels = mixFormat->nChannels;
		*aChannelMask = mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat)->dwChannelMask : 0;
		CoTaskMemFree(mixFormat);
		return S_OK;
	}

	// The speakers SoLoud mixes for, by its channel count
	static DWORD wasapiChannelMask(unsigned int aChannels)
	{
		switch (aChannels)
		{
		case 1: return SPEAKER_FRONT_CENTER;
		case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
		case 4: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
		case 6: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
		case 8: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
		}
		return 0;
	}

	// Opens the device got by wasapiGetDevice for 16-bit samples in aData's channels, rate and speakers, converted by
	// Windows to what the device has if aConvert, otherwise as they are, then gets it ready to be given them
	static HRESULT wasapiOpen(WASAPIData *aData, bool aConvert)
	{
		WAVEFORMATEXTENSIBLE format;
		ZeroMemory(&format, sizeof(format));
		format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		format.Format.nChannels = static_cast<WORD>(aData->channels);
		format.Format.nSamplesPerSec = aData->sampleRate;
		format.Format.wBitsPerSample = 16;
		format.Format.nBlockAlign = static_cast<WORD>(aData->channels * 2);
		format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
		format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
		format.Samples.wValidBitsPerSample = 16;
		format.dwChannelMask = aData->channelMask;
		format.SubFormat = WASAPI_SUBTYPE_PCM;
		DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
		if (aConvert)
		{
			flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
		}
		HRESULT res = aData->audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, aData->duration, 0, &format.Format, 0);
		aData->bufferFrames = 0;
		if (SUCCEEDED(res))
		{
			res = aData->audioClient->GetBufferSize(&aData->bufferFrames);
		}
		if (SUCCEEDED(res))
		{
			res = aData->audioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&aData->renderClient));
		}
		if (SUCCEEDED(res))
		{
			res = aData->audioClient->SetEventHandle(aData->bufferEndEvent);
		}
		return res;
	}

	// The default device opened again for the channels SoLoud already mixes, which can't change now
	static HRESULT wasapiReopen(WASAPIData *aData)
	{
		unsigned int samplerate, channels;
		DWORD channelMask;
		HRESULT res = wasapiGetDevice(aData, &samplerate, &channels, &channelMask);
		if (FAILED(res))
		{
			return res;
		}
		res = wasapiOpen(aData, true);
		if (FAILED(res) && samplerate == aData->sampleRate && channels == static_cast<unsigned int>(aData->channels))
		{
			// Without Windows converting, only a device that mixes just what SoLoud does
			res = wasapiGetDevice(aData, &samplerate, &channels, &channelMask);
			if (SUCCEEDED(res))
			{
				res = wasapiOpen(aData, false);
			}
		}
		return res;
	}

    static HRESULT wasapiSubmitBuffer(WASAPIData *aData, UINT32 aFrames)
    {
        BYTE *buffer = 0;
		HRESULT res = aData->renderClient->GetBuffer(aFrames, &buffer);
        if (FAILED(res))
        {
            return res;
        }
		aData->soloud->mixSigned16((short *)buffer, aFrames);
		return aData->renderClient->ReleaseBuffer(aFrames, 0);
    }

    static void wasapiThread(LPVOID aParam)
    {
		bool comInitialized = SUCCEEDED(CoInitializeEx(0, COINIT_MULTITHREADED));
		bool started = false;
		bool resetRequired = false;
        WASAPIData *data = static_cast<WASAPIData*>(aParam);
        while (WAIT_OBJECT_0 != WaitForSingleObject(data->audioProcessingDoneEvent, 0))
        {
			resetRequired = resetRequired || data->resetRequired;
			if (!resetRequired)
			{
				UINT32 padding = 0;
				resetRequired = FAILED(data->audioClient->GetCurrentPadding(&padding));
				if (!resetRequired)
				{
					resetRequired = FAILED(wasapiSubmitBuffer(data, data->bufferFrames - padding));
				}
				if (!resetRequired && !started)
				{
					resetRequired = FAILED(data->audioClient->Start());
					started = true;
				}
				WaitForSingleObject(data->bufferEndEvent, INFINITE);
			}
			else
			{
				// There was an error. Tear down audio graph and recreate it
				data->resetRequired = false;
				if (0 != data->audioClient)
				{
					data->audioClient->Stop();
				}
				bool reinitFailed = FAILED(wasapiReopen(data));
				started = false;
				resetRequired = reinitFailed;
				if (reinitFailed)
				{
					// Until the default device changes, or a while
					WaitForSingleObject(data->bufferEndEvent, WASAPI_RETRY_MILLISECONDS);
				}
			}
        }
		if (comInitialized)
		{
			CoUninitialize();
		}
    }

    static void wasapiCleanup(Soloud *aSoloud)
    {
        if (0 == aSoloud->mBackendData)
        {
            return;
        }
        WASAPIData *data = static_cast<WASAPIData*>(aSoloud->mBackendData);
        SetEvent(data->audioProcessingDoneEvent);
        SetEvent(data->bufferEndEvent);
		if (data->thread)
		{
			Thread::wait(data->thread);
			Thread::release(data->thread);
		}
		if (0 != data->notificationClient)
		{
			data->deviceEnumerator->UnregisterEndpointNotificationCallback(data->notificationClient);
			data->notificationClient->Release();
		}
        CloseHandle(data->bufferEndEvent);
        CloseHandle(data->audioProcessingDoneEvent);
        if (0 != data->audioClient)
        {
            data->audioClient->Stop();
        }
        SAFE_RELEASE(data->renderClient);
        SAFE_RELEASE(data->audioClient);
        SAFE_RELEASE(data->device);
        SAFE_RELEASE(data->deviceEnumerator);
		bool comInitialized = data->comInitialized;
        delete data;
        aSoloud->mBackendData = 0;
		if (comInitialized)
		{
			CoUninitialize();
		}
    }

	static result wasapiFail(HRESULT aResult)
	{
		wasapi_last_error = static_cast<unsigned long>(aResult);
		return UNKNOWN_ERROR;
	}

	result wasapi_init(Soloud *aSoloud, unsigned int aFlags, unsigned int /*aSamplerate*/, unsigned int aBuffer, unsigned int /*aChannels*/)
    {
        WASAPIData *data = new WASAPIData;
        ZeroMemory(data, sizeof(WASAPIData));
		// Not where the thread already has COM another way, which then stays as it was
		data->comInitialized = SUCCEEDED(CoInitializeEx(0, COINIT_MULTITHREADED));
        aSoloud->mBackendData = data;
        aSoloud->mBackendCleanupFunc = wasapiCleanup;

        data->bufferEndEvent = CreateEvent(0, FALSE, FALSE, 0);
        if (0 == data->bufferEndEvent)
        {
            return wasapiFail(HRESULT_FROM_WIN32(GetLastError()));
        }
        data->audioProcessingDoneEvent = CreateEvent(0, FALSE, FALSE, 0);
        if (0 == data->audioProcessingDoneEvent)
        {
            return wasapiFail(HRESULT_FROM_WIN32(GetLastError()));
        }
		HRESULT res = CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
			reinterpret_cast<void**>(&data->deviceEnumerator));
        if (FAILED(res))
        {
            return wasapiFail(res);
        }
		data->notificationClient = new MMNotificationClient(data);
		/*HRESULT result = */data->deviceEnumerator->RegisterEndpointNotificationCallback(data->notificationClient);

		// At the rate the device mixes at, so there's none to convert
		unsigned int channels;
		DWORD channelMask;
		res = wasapiGetDevice(data, &data->sampleRate, &channels, &channelMask);
		if (FAILED(res))
		{
			return wasapiFail(res);
		}
		data->duration = static_cast<REFERENCE_TIME>(static_cast<double>(aBuffer)
			/ (static_cast<double>(data->sampleRate)*(1.0/10000000.0)));

		// Stereo, which Windows puts on whatever speakers there are
		data->channels = 2;
		data->channelMask = wasapiChannelMask(2);
		res = wasapiOpen(data, true);
		if (FAILED(res) && wasapiChannelMask(channels) != 0)
		{
			// Where Windows can't convert, the device's own channels, which SoLoud mixes for instead
			data->channels = channels;
			data->channelMask = channelMask != 0 ? channelMask : wasapiChannelMask(channels);
			unsigned int samplerate;
			res = wasapiGetDevice(data, &samplerate, &channels, &channelMask);
			if (SUCCEEDED(res))
			{
				res = wasapiOpen(data, false);
			}
		}
		if (FAILED(res))
		{
			return wasapiFail(res);
		}
        data->soloud = aSoloud;
        aSoloud->postinit_internal(data->sampleRate, data->bufferFrames * data->channels, aFlags, data->channels);
        data->thread = Thread::createThread(wasapiThread, data);
        if (0 == data->thread)
        {
            return wasapiFail(E_FAIL);
        }
        aSoloud->mBackendString = "WASAPI";
        return 0;
    }
};
#endif
