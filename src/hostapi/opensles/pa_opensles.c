/*
 * $Id$
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 *
 * Android OpenSL ES implementation by Sanne Raymaekers
 * Copyright (c) 2016-2017 Sanne Raymaekers <sanne.raymaekers@gmail.com>
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2002 Ross Bencina, Phil Burk
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * The text above constitutes the entire PortAudio license; however,
 * the PortAudio community also makes the following non-binding requests:
 *
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version. It is also
 * requested that these non-binding requests be included along with the
 * license above.
 */

/**
 @file
 @ingroup hostapi_src
 @brief opensles implementation of support for a host API.
*/

#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#include <android/api-level.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"
#include "pa_unix_util.h"
#include "pa_debugprint.h"

#include "pa_opensles.h"

#define ENSURE(expr, errorText) \
    do { \
        PaError err; \
        if( UNLIKELY( (err = (expr)) < paNoError ) ) \
        { \
            PaUtil_DebugPrint(( "Expression '" #expr "' failed in '" __FILE__ "', line: " STRINGIZE( __LINE__ ) "\n" )); \
            PaUtil_SetLastHostErrorInfo( paInDevelopment, err, errorText ); \
            result = err; \
            goto error; \
        } \
    } while(0);

static void Terminate( struct PaUtilHostApiRepresentation *hostApi );
static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate );
static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           const PaStreamParameters *inputParameters,
                           const PaStreamParameters *outputParameters,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PaStreamCallback *streamCallback,
                           void *userData );
static PaError CloseStream( PaStream* stream );
static PaError StartStream( PaStream *stream );
static PaError StopStream( PaStream *stream );
static PaError AbortStream( PaStream *stream );
static PaError IsStreamStopped( PaStream *s );
static PaError IsStreamActive( PaStream *stream );
static PaTime GetStreamTime( PaStream *stream );
static double GetStreamCpuLoad( PaStream* stream );
static PaError ReadStream( PaStream* stream, void *buffer, unsigned long frames );
static PaError WriteStream( PaStream* stream, const void *buffer, unsigned long frames );
static signed long GetStreamReadAvailable( PaStream* stream );
static signed long GetStreamWriteAvailable( PaStream* stream );
static unsigned long GetApproximateLowBufferSize();

static unsigned long nativeBufferSize = 0;
static unsigned numberOfBuffers = 2;

/*  PaOpenslesHostApiRepresentation - host api datastructure specific to this implementation */
typedef struct
{
    PaUtilHostApiRepresentation inheritedHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilStreamInterface blockingStreamInterface;

    PaUtilAllocationGroup *allocations;

    SLObjectItf sl;
    SLEngineItf slEngineItf;
}
PaOpenslesHostApiRepresentation;

PaError Opensles_InitializeEngine(PaOpenslesHostApiRepresentation *openslesHostApi)
{
    SLresult slResult;
    PaError result = paNoError;
    const SLEngineOption engineOption[] = {{ SL_ENGINEOPTION_THREADSAFE, SL_BOOLEAN_TRUE }};

    slResult = slCreateEngine( &openslesHostApi->sl , 1, engineOption, 0, NULL, NULL);
    result = slResult == SL_RESULT_SUCCESS ? paNoError : paUnanticipatedHostError;
    if( result != paNoError ) goto error;

    slResult = (*openslesHostApi->sl)->Realize( openslesHostApi->sl, SL_BOOLEAN_FALSE );
    result = slResult == SL_RESULT_SUCCESS ? paNoError : paUnanticipatedHostError;
    if( result != paNoError ) goto error;

    slResult = (*openslesHostApi->sl)->GetInterface( openslesHostApi->sl, SL_IID_ENGINE,
                                                     (void *) &openslesHostApi->slEngineItf );
    result = slResult == SL_RESULT_SUCCESS ? paNoError : paUnanticipatedHostError;
error:
    return result;
}

/* expects samplerate to be in milliHertz */
static PaError IsOutputSampleRateSupported(PaOpenslesHostApiRepresentation *openslesHostApi, double sampleRate)
{
    SLresult slResult;
    SLObjectItf audioPlayer;
    SLObjectItf outputMixObject;

    (*openslesHostApi->slEngineItf)->CreateOutputMix( openslesHostApi->slEngineItf,
                                                      &outputMixObject, 0, NULL, NULL );
    (*outputMixObject)->Realize( outputMixObject, SL_BOOLEAN_FALSE );

    SLDataLocator_OutputMix outputLocator = { SL_DATALOCATOR_OUTPUTMIX, outputMixObject };
    SLDataSink audioSink = { &outputLocator, NULL };
    SLDataLocator_AndroidSimpleBufferQueue outputBQLocator = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2 };
    SLDataFormat_PCM  formatPcm = { SL_DATAFORMAT_PCM, 1, sampleRate,
                                     SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                                     SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN };
    SLDataSource audioSrc = { &outputBQLocator, &formatPcm };

    slResult = (*openslesHostApi->slEngineItf)->CreateAudioPlayer( openslesHostApi->slEngineItf,
                                                                   &audioPlayer, &audioSrc,
                                                                   &audioSink, 0, NULL, NULL );
    if( slResult != SL_RESULT_SUCCESS )
    {
        (*outputMixObject)->Destroy( outputMixObject );
        return paInvalidSampleRate;
    }
    else
    {
        (*audioPlayer)->Destroy( audioPlayer );
        (*outputMixObject)->Destroy( outputMixObject );
        return paNoError;
    }
}

/* expects samplerate to be in milliHertz */
static PaError IsInputSampleRateSupported(PaOpenslesHostApiRepresentation *openslesHostApi, double sampleRate)
{
    SLresult slResult;
    SLObjectItf audioRecorder;

    SLDataLocator_IODevice inputLocator = {SL_DATALOCATOR_IODEVICE,
                                           SL_IODEVICE_AUDIOINPUT,
                                           SL_DEFAULTDEVICEID_AUDIOINPUT, NULL};
    SLDataSource audioSrc = {&inputLocator, NULL};
    SLDataLocator_AndroidSimpleBufferQueue inputBQLocator = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM formatPcm = {SL_DATAFORMAT_PCM, 1, sampleRate,
                                  SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                                  SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN};
    SLDataSink audioSink = {&inputBQLocator, &formatPcm};

    slResult = (*openslesHostApi->slEngineItf)->CreateAudioRecorder(openslesHostApi->slEngineItf,
                                                                    &audioRecorder, &audioSrc,
                                                                    &audioSink, 0, NULL, NULL);
    if( slResult != SL_RESULT_SUCCESS )
    {
        return paInvalidSampleRate;
    }
    else
    {
        (*audioRecorder)->Destroy( audioRecorder );
        return paNoError;
    }
}

static PaError IsOutputChannelCountSupported(PaOpenslesHostApiRepresentation *openslesHostApi, SLuint32 numOfChannels)
{

    if( numOfChannels > 2 || numOfChannels == 0 )
        return paInvalidChannelCount;

    SLresult slResult;
    SLObjectItf audioPlayer;
    SLObjectItf outputMixObject;
    const SLuint32 channelMasks[] = { SL_SPEAKER_FRONT_CENTER, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT };

    (*openslesHostApi->slEngineItf)->CreateOutputMix( openslesHostApi->slEngineItf, &outputMixObject, 0, NULL, NULL );
    (*outputMixObject)->Realize( outputMixObject, SL_BOOLEAN_FALSE );

    SLDataLocator_OutputMix outputLocator = { SL_DATALOCATOR_OUTPUTMIX, outputMixObject };
    SLDataSink audioSink = { &outputLocator, NULL };
    SLDataLocator_AndroidSimpleBufferQueue outputBQLocator = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2 };
    SLDataFormat_PCM  formatPcm = { SL_DATAFORMAT_PCM, numOfChannels, SL_SAMPLINGRATE_16,
                                     SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                                     channelMasks[numOfChannels - 1], SL_BYTEORDER_LITTLEENDIAN };
    SLDataSource audioSrc = { &outputBQLocator, &formatPcm };

    slResult = (*openslesHostApi->slEngineItf)->CreateAudioPlayer( openslesHostApi->slEngineItf,
                                                                   &audioPlayer, &audioSrc,
                                                                   &audioSink, 0, NULL, NULL );
    if( slResult != SL_RESULT_SUCCESS )
    {
        (*outputMixObject)->Destroy( outputMixObject );
        return paInvalidChannelCount;
    }
    else
    {
        (*audioPlayer)->Destroy( audioPlayer );
        (*outputMixObject)->Destroy( outputMixObject );
        return paNoError;
    }
}

static PaError IsInputChannelCountSupported(PaOpenslesHostApiRepresentation *openslesHostApi, SLuint32 numOfChannels)
{
    SLresult slResult;
    SLObjectItf audioRecorder;
    const SLuint32 channelMasks[] = { SL_SPEAKER_FRONT_CENTER, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT };

    SLDataLocator_IODevice inputLocator = {SL_DATALOCATOR_IODEVICE,
                                           SL_IODEVICE_AUDIOINPUT,
                                           SL_DEFAULTDEVICEID_AUDIOINPUT, NULL};
    SLDataSource audioSrc = {&inputLocator, NULL};

    SLDataLocator_AndroidSimpleBufferQueue inputBQLocator = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM formatPcm = {SL_DATAFORMAT_PCM, numOfChannels, SL_SAMPLINGRATE_16,
                                  SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                                  channelMasks[numOfChannels - 1], SL_BYTEORDER_LITTLEENDIAN};
    SLDataSink audioSink = {&inputBQLocator, &formatPcm};

    slResult = (*openslesHostApi->slEngineItf)->CreateAudioRecorder(openslesHostApi->slEngineItf,
                                                                    &audioRecorder, &audioSrc,
                                                                    &audioSink, 0, NULL, NULL);

    if( slResult != SL_RESULT_SUCCESS )
    {

        return paInvalidChannelCount;
    }
    else
    {
        (*audioRecorder)->Destroy( audioRecorder );
        return paNoError;
    }
}

PaError PaOpenSLES_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    int i, deviceCount;
    PaOpenslesHostApiRepresentation *openslesHostApi;
    PaDeviceInfo *deviceInfoArray;

    openslesHostApi = (PaOpenslesHostApiRepresentation*)PaUtil_AllocateMemory( sizeof(PaOpenslesHostApiRepresentation) );
    if( !openslesHostApi )
    {
        result = paInsufficientMemory;
        goto error;
    }

    openslesHostApi->allocations = PaUtil_CreateAllocationGroup();
    if( !openslesHostApi->allocations )
    {
        result = paInsufficientMemory;
        goto error;
    }

    *hostApi = &openslesHostApi->inheritedHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paInDevelopment;
    (*hostApi)->info.name = "android OpenSLES";
    (*hostApi)->info.defaultOutputDevice = 0;
    (*hostApi)->info.defaultInputDevice = 0;
    (*hostApi)->info.deviceCount = 0;

    ENSURE( Opensles_InitializeEngine(openslesHostApi), "Initializing engine failed" );

    deviceCount = 1;
    (*hostApi)->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
        openslesHostApi->allocations, sizeof(PaDeviceInfo*) * deviceCount );

    if( !(*hostApi)->deviceInfos )
    {
        result = paInsufficientMemory;
        goto error;
    }

    /* allocate all device info structs in a contiguous block */
    deviceInfoArray = (PaDeviceInfo*)PaUtil_GroupAllocateMemory(
        openslesHostApi->allocations, sizeof(PaDeviceInfo) * deviceCount );
    if( !deviceInfoArray )
    {
        result = paInsufficientMemory;
        goto error;
    }

    for( i=0; i < deviceCount; ++i )
    {
        PaDeviceInfo *deviceInfo = &deviceInfoArray[i];
        deviceInfo->structVersion = 2;
        deviceInfo->hostApi = hostApiIndex;
        /* android selects it's own device, so we'll just expose a default device */
        deviceInfo->name = "default";

        const SLuint32 channelsToTry[] = { 2, 1 };
        const SLuint32 channelsToTryLength = 2;
        deviceInfo->maxOutputChannels = 0;
        deviceInfo->maxInputChannels = 0;
        for( i = 0; i < channelsToTryLength; ++i )
        {
            if( IsOutputChannelCountSupported(openslesHostApi, channelsToTry[i]) == paNoError )
            {
                deviceInfo->maxOutputChannels = channelsToTry[i];
                break;
            }
        }
        for( i = 0; i < channelsToTryLength; ++i )
        {
            if( IsInputChannelCountSupported(openslesHostApi, channelsToTry[i]) == paNoError )
            {
                deviceInfo->maxInputChannels = channelsToTry[i];
                break;
            }
        }

        /* check samplerates in order of preference */
        const SLuint32 sampleRates[] = { SL_SAMPLINGRATE_48, SL_SAMPLINGRATE_44_1,
                                         SL_SAMPLINGRATE_32, SL_SAMPLINGRATE_24,
                                         SL_SAMPLINGRATE_16};
        const SLuint32 numberOfSampleRates = 5;
        deviceInfo->defaultSampleRate = 0;
        for( i = 0; i < numberOfSampleRates; ++i )
        {
            if( IsOutputSampleRateSupported(openslesHostApi, sampleRates[i]) == paNoError
                && IsInputSampleRateSupported(openslesHostApi, sampleRates[i]) == paNoError )
            {
                /* opensl defines sampling rates in milliHertz, so we divide by 1000 */
                deviceInfo->defaultSampleRate = sampleRates[i] / 1000;
                break;
            }
        }
        if( deviceInfo->defaultSampleRate == 0 )
            goto error;

        /* If the user has set nativeBufferSize by querying the optimal buffer size via java,
         * use the user-defined value since that will offer the lowest possible latency
         */
        if( nativeBufferSize != 0 )
        {
            deviceInfo->defaultLowInputLatency = (double) nativeBufferSize / deviceInfo->defaultSampleRate;
            deviceInfo->defaultLowOutputLatency = (double) nativeBufferSize / deviceInfo->defaultSampleRate;
            deviceInfo->defaultHighInputLatency = (double) nativeBufferSize * 4 / deviceInfo->defaultSampleRate;
            deviceInfo->defaultHighOutputLatency = (double) nativeBufferSize * 4 / deviceInfo->defaultSampleRate;
        }
        else
        {
            deviceInfo->defaultLowInputLatency = (double) GetApproximateLowBufferSize() / deviceInfo->defaultSampleRate;
            deviceInfo->defaultLowOutputLatency = (double) GetApproximateLowBufferSize() / deviceInfo->defaultSampleRate;
            deviceInfo->defaultHighInputLatency = (double) GetApproximateLowBufferSize() * 4 / deviceInfo->defaultSampleRate;
            deviceInfo->defaultHighOutputLatency = (double) GetApproximateLowBufferSize() * 4 / deviceInfo->defaultSampleRate;
        }

        (*hostApi)->deviceInfos[i] = deviceInfo;
        ++(*hostApi)->info.deviceCount;

    }

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface( &openslesHostApi->callbackStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive,
                                      GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyRead, PaUtil_DummyWrite,
                                      PaUtil_DummyGetReadAvailable, PaUtil_DummyGetWriteAvailable );

    PaUtil_InitializeStreamInterface( &openslesHostApi->blockingStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive,
                                      GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      ReadStream, WriteStream, GetStreamReadAvailable, GetStreamWriteAvailable );

    return result;

error:
    if( openslesHostApi )
    {
        if( openslesHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( openslesHostApi->allocations );
            PaUtil_DestroyAllocationGroup( openslesHostApi->allocations );
        }

        PaUtil_FreeMemory( openslesHostApi );
    }
    return result;
}

static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaOpenslesHostApiRepresentation *openslesHostApi = (PaOpenslesHostApiRepresentation*)hostApi;

    if( openslesHostApi->sl )
    {
        (*openslesHostApi->sl)->Destroy(openslesHostApi->sl);
    }

    if( openslesHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( openslesHostApi->allocations );
        PaUtil_DestroyAllocationGroup( openslesHostApi->allocations );
    }

    PaUtil_FreeMemory( openslesHostApi );
}

static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate )
{
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    PaOpenslesHostApiRepresentation *openslesHostApi = (PaOpenslesHostApiRepresentation*) hostApi;

    if( inputParameters )
    {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        if( inputSampleFormat & paCustomFormat )
            return paSampleFormatNotSupported;

        if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        if( inputChannelCount > hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels )
            return paInvalidChannelCount;

        if( inputParameters->hostApiSpecificStreamInfo )
        {
#if __ANDROID_API__ >= 14
            SLint32 androidRecordingPreset = ( (PaOpenslesStreamInfo*)outputParameters->hostApiSpecificStreamInfo )->androidRecordingPreset;
            if( androidRecordingPreset != SL_ANDROID_RECORDING_PRESET_NONE
                && androidRecordingPreset != SL_ANDROID_RECORDING_PRESET_GENERIC
                && androidRecordingPreset != SL_ANDROID_RECORDING_PRESET_CAMCORDER
                && androidRecordingPreset != SL_ANDROID_RECORDING_PRESET_VOICE_RECOGNITION )
                return paIncompatibleHostApiSpecificStreamInfo;
#endif
        }
    }
    else
    {
        inputChannelCount = 0;
    }

    if( outputParameters )
    {
        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;

        if( outputSampleFormat & paCustomFormat )
            return paSampleFormatNotSupported;

        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        if( outputChannelCount > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels )
            return paInvalidChannelCount;

        if( outputParameters->hostApiSpecificStreamInfo )
        {
#if __ANDROID_API__ >= 14
            SLint32 androidPlaybackStreamType = ( (PaOpenslesStreamInfo*)outputParameters->hostApiSpecificStreamInfo )->androidPlaybackStreamType;
            if( androidPlaybackStreamType != SL_ANDROID_STREAM_VOICE
                && androidPlaybackStreamType != SL_ANDROID_STREAM_SYSTEM
                && androidPlaybackStreamType != SL_ANDROID_STREAM_RING
                && androidPlaybackStreamType != SL_ANDROID_STREAM_MEDIA
                && androidPlaybackStreamType != SL_ANDROID_STREAM_ALARM
                && androidPlaybackStreamType != SL_ANDROID_STREAM_NOTIFICATION )
                return paIncompatibleHostApiSpecificStreamInfo;
#endif
        }
    }
    else
    {
        outputChannelCount = 0;
    }

    if( outputChannelCount > 0 )
    {
        if( IsOutputSampleRateSupported( openslesHostApi, sampleRate * 1000 ) != paNoError )
            return paInvalidSampleRate;
    }
    if( inputChannelCount > 0 )
    {
        if( IsInputSampleRateSupported( openslesHostApi, sampleRate * 1000 ) != paNoError )
            return paInvalidSampleRate;
    }

    return paFormatIsSupported;
}

typedef struct OpenslesStream
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

    SLObjectItf audioPlayer;
    SLObjectItf outputMixObject;
    SLPlayItf playerItf;
    SLAndroidSimpleBufferQueueItf outputBufferQueueItf;
    /* SLPrefetchStatusItf prefetchStatusItf; */
    SLVolumeItf volumeItf;
    SLAndroidConfigurationItf outputConfigurationItf;

    SLObjectItf audioRecorder;
    SLRecordItf recorderItf;
    SLAndroidSimpleBufferQueueItf inputBufferQueueItf;
    SLAndroidConfigurationItf inputConfigurationItf;

    SLboolean isBlocking;
    SLboolean isStopped;
    SLboolean isActive;
    SLboolean doStop;
    SLboolean doAbort;
    SLboolean hasOutput;
    SLboolean hasInput;

    int callbackResult;
    sem_t outputSem;
    sem_t inputSem;

    PaStreamCallbackFlags cbFlags;
    PaUnixThread streamThread;

    void **outputBuffers;
    int currentOutputBuffer;
    void **inputBuffers;
    int currentInputBuffer;

    unsigned long framesPerHostCallback;
    unsigned bytesPerFrame;
}
OpenslesStream;

static PaError InitializeOutputStream( PaOpenslesHostApiRepresentation *openslesHostApi, OpenslesStream *stream,
                                       SLint32 androidPlaybackStreamType, double sampleRate );
static PaError InitializeInputStream( PaOpenslesHostApiRepresentation *openslesHostApi, OpenslesStream *stream,
                                      SLint32 androidRecordingPreset, double sampleRate );
static void StreamProcessingCallback( void *userData );
static void NotifyBufferFreeCallback( SLAndroidSimpleBufferQueueItf bufferQueueItf, void *userData );
/* static void PrefetchStatusCallback( SLPrefetchStatusItf prefetchStatusItf, void *userData, SLuint32 event ); */

static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           const PaStreamParameters *inputParameters,
                           const PaStreamParameters *outputParameters,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PaStreamCallback *streamCallback,
                           void *userData )
{
    PaError result = paNoError;
    PaOpenslesHostApiRepresentation *openslesHostApi = (PaOpenslesHostApiRepresentation*)hostApi;
    OpenslesStream *stream = 0;
    unsigned long framesPerHostBuffer; /* these may not be equivalent for all implementations */
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    PaSampleFormat hostInputSampleFormat, hostOutputSampleFormat;
#if __ANDROID_API__ >= 14
    SLint32 androidPlaybackStreamType = SL_ANDROID_STREAM_MEDIA;
    SLint32 androidRecordingPreset = SL_ANDROID_RECORDING_PRESET_GENERIC;
#else
    SLint32 androidPlaybackStreamType = -1;
    SLint32 androidRecordingPreset = -1;
#endif

    if( inputParameters )
    {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        /* check that input device can support inputChannelCount */
        if( inputChannelCount > hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels )
            return paInvalidChannelCount;

        /* validate inputStreamInfo */
        if( inputParameters->hostApiSpecificStreamInfo )
        {
#if __ANDROID_API__ >= 14
            androidRecordingPreset = ( (PaOpenslesStreamInfo*)outputParameters->hostApiSpecificStreamInfo )->androidRecordingPreset;
            if( androidRecordingPreset != SL_ANDROID_RECORDING_PRESET_NONE
                && androidRecordingPreset != SL_ANDROID_RECORDING_PRESET_GENERIC
                && androidRecordingPreset != SL_ANDROID_RECORDING_PRESET_CAMCORDER
                && androidRecordingPreset != SL_ANDROID_RECORDING_PRESET_VOICE_RECOGNITION )
                return paIncompatibleHostApiSpecificStreamInfo;
#endif
        }

        hostInputSampleFormat = PaUtil_SelectClosestAvailableFormat( paInt16, inputSampleFormat );
    }
    else
    {
        inputChannelCount = 0;
        inputSampleFormat = hostInputSampleFormat = paInt16; /* Surpress 'uninitialised var' warnings. */
    }

    if( outputParameters )
    {
        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;

        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        /* check that output device can support inputChannelCount */
        if( outputChannelCount > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels )
            return paInvalidChannelCount;

        /* validate outputStreamInfo */
        if( outputParameters->hostApiSpecificStreamInfo )
        {
#if __ANDROID_API__ >= 14
            androidPlaybackStreamType = ( (PaOpenslesStreamInfo*)outputParameters->hostApiSpecificStreamInfo )->androidPlaybackStreamType;
            if( androidPlaybackStreamType != SL_ANDROID_STREAM_VOICE
                && androidPlaybackStreamType != SL_ANDROID_STREAM_SYSTEM
                && androidPlaybackStreamType != SL_ANDROID_STREAM_RING
                && androidPlaybackStreamType != SL_ANDROID_STREAM_MEDIA
                && androidPlaybackStreamType != SL_ANDROID_STREAM_ALARM
                && androidPlaybackStreamType != SL_ANDROID_STREAM_NOTIFICATION )
                return paIncompatibleHostApiSpecificStreamInfo;
#endif
        }

        if( IsOutputSampleRateSupported( openslesHostApi, sampleRate * 1000 ) != paNoError )
            return paInvalidSampleRate;

        hostOutputSampleFormat = PaUtil_SelectClosestAvailableFormat( paInt16, outputSampleFormat );
    }
    else
    {
        outputChannelCount = 0;
        outputSampleFormat = hostOutputSampleFormat = paInt16;
    }

    /* validate platform specific flags */
    if( (streamFlags & paPlatformSpecificFlags) != 0 )
        return paInvalidFlag;

    if( framesPerBuffer == paFramesPerBufferUnspecified )
        framesPerHostBuffer = (unsigned long) (outputParameters->suggestedLatency * sampleRate);
    else
        framesPerHostBuffer = framesPerBuffer;

    stream = (OpenslesStream*)PaUtil_AllocateMemory( sizeof(OpenslesStream) );

    if( !stream )
    {
        result = paInsufficientMemory;
        goto error;
    }

    if( streamCallback )
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &openslesHostApi->callbackStreamInterface, streamCallback, userData );
    }
    else
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &openslesHostApi->blockingStreamInterface, streamCallback, userData );
    }

    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, sampleRate );

    result =  PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
              inputChannelCount, inputSampleFormat, hostInputSampleFormat,
              outputChannelCount, outputSampleFormat, hostOutputSampleFormat,
              sampleRate, streamFlags, framesPerBuffer,
              framesPerHostBuffer, paUtilFixedHostBufferSize,
              streamCallback, userData );
    if( result != paNoError )
        goto error;

    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;
    stream->isBlocking = streamCallback ? SL_BOOLEAN_FALSE : SL_BOOLEAN_TRUE;
    stream->framesPerHostCallback = framesPerHostBuffer;
    stream->bytesPerFrame = sizeof(SLint16);
    stream->cbFlags = 0;
    stream->isStopped = SL_BOOLEAN_TRUE;
    stream->isActive = SL_BOOLEAN_FALSE;

    if( !stream->isBlocking )
        PaUnixThreading_Initialize();

    if( inputChannelCount > 0 )
    {
        stream->hasInput = SL_BOOLEAN_TRUE;
        stream->streamRepresentation.streamInfo.inputLatency =
            ((PaTime)PaUtil_GetBufferProcessorInputLatencyFrames(&stream->bufferProcessor)
             + stream->framesPerHostCallback) / sampleRate;
        ENSURE( InitializeInputStream( openslesHostApi, stream, androidRecordingPreset, sampleRate ),
                "Initializing inputstream failed" );
    }
    else
        stream->hasInput = SL_BOOLEAN_FALSE;

    if( outputChannelCount > 0 )
    {
        stream->hasOutput = SL_BOOLEAN_TRUE;
        stream->streamRepresentation.streamInfo.outputLatency =
            ((PaTime)PaUtil_GetBufferProcessorOutputLatencyFrames(&stream->bufferProcessor)
             + stream->framesPerHostCallback) / sampleRate;
        ENSURE( InitializeOutputStream( openslesHostApi, stream, androidPlaybackStreamType, sampleRate ),
                "Initializing outputstream failed" );
    }
    else
        stream->hasOutput = SL_BOOLEAN_FALSE;

    *s = (PaStream*)stream;
    return result;

error:
    if( stream )
        PaUtil_FreeMemory( stream );
    return result;
}

static PaError InitializeOutputStream(PaOpenslesHostApiRepresentation *openslesHostApi, OpenslesStream *stream, SLint32 androidPlaybackStreamType, double sampleRate)
{
    PaError result = paNoError;
    SLresult slResult;
    int i, j;
    const SLuint32 channelMasks[] = { SL_SPEAKER_FRONT_CENTER, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT };
    SLDataLocator_AndroidSimpleBufferQueue outputBQLocator = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, numberOfBuffers };
    SLDataFormat_PCM  formatPcm = { SL_DATAFORMAT_PCM, stream->bufferProcessor.outputChannelCount,
                                    sampleRate * 1000.0, SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                                    channelMasks[stream->bufferProcessor.outputChannelCount - 1], SL_BYTEORDER_LITTLEENDIAN };
    SLDataSource audioSrc = { &outputBQLocator, &formatPcm };

    (*openslesHostApi->slEngineItf)->CreateOutputMix( openslesHostApi->slEngineItf, &stream->outputMixObject, 0, NULL, NULL );
    (*stream->outputMixObject)->Realize( stream->outputMixObject, SL_BOOLEAN_FALSE );
    SLDataLocator_OutputMix outputLocator = { SL_DATALOCATOR_OUTPUTMIX, stream->outputMixObject };
    SLDataSink audioSink = { &outputLocator, &formatPcm };

    if( !stream->isBlocking )
    {
        const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME, SL_IID_ANDROIDCONFIGURATION };
        const SLboolean req[] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
        const unsigned interfaceCount = 3;
        slResult = (*openslesHostApi->slEngineItf)->CreateAudioPlayer(openslesHostApi->slEngineItf, &stream->audioPlayer,
                                                                      &audioSrc, &audioSink, interfaceCount, ids, req);
    }
    else
    {
        const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME, SL_IID_ANDROIDCONFIGURATION };
        const SLboolean req[] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
        const unsigned interfaceCount = 3;
        slResult = (*openslesHostApi->slEngineItf)->CreateAudioPlayer( openslesHostApi->slEngineItf, &stream->audioPlayer,
                                                                       &audioSrc, &audioSink, interfaceCount, ids, req );
    }
    if( slResult != SL_RESULT_SUCCESS )
    {
        (*stream->outputMixObject)->Destroy( stream->outputMixObject );
        result = paUnanticipatedHostError;
        goto error;
    }

#if __ANDROID_API__ >= 14
    (*stream->audioPlayer)->GetInterface( stream->audioPlayer, SL_IID_ANDROIDCONFIGURATION, &stream->outputConfigurationItf );
    (*stream->outputConfigurationItf)->SetConfiguration( stream->outputConfigurationItf, SL_ANDROID_KEY_STREAM_TYPE,
                                                   &androidPlaybackStreamType, sizeof(androidPlaybackStreamType) );
#endif

    slResult = (*stream->audioPlayer)->Realize( stream->audioPlayer, SL_BOOLEAN_FALSE );
    if( slResult != SL_RESULT_SUCCESS )
    {
        (*stream->audioPlayer)->Destroy( stream->audioPlayer );
        (*stream->outputMixObject)->Destroy( stream->outputMixObject );
        result = paUnanticipatedHostError;
        goto error;
    }

    (*stream->audioPlayer)->GetInterface( stream->audioPlayer, SL_IID_PLAY, &stream->playerItf );
    (*stream->audioPlayer)->GetInterface( stream->audioPlayer, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &stream->outputBufferQueueItf );
    (*stream->audioPlayer)->GetInterface( stream->audioPlayer, SL_IID_VOLUME, &stream->volumeItf );

    stream->outputBuffers = (void **) PaUtil_AllocateMemory( numberOfBuffers * sizeof(SLint16 *) );
    for( i = 0; i < numberOfBuffers; ++i )
    {
        stream->outputBuffers[i] = (void*) PaUtil_AllocateMemory( stream->framesPerHostCallback * stream->bytesPerFrame
                                                                  * stream->bufferProcessor.outputChannelCount );
        if( !stream->outputBuffers[i] )
        {
            for( j = 0; j < i; ++j )
                PaUtil_FreeMemory( stream->outputBuffers[j] );
            PaUtil_FreeMemory( stream->outputBuffers );
            (*stream->audioPlayer)->Destroy( stream->audioPlayer );
            (*stream->outputMixObject)->Destroy( stream->outputMixObject );
            result = paInsufficientMemory;
            goto error;
        }
    }
    stream->currentOutputBuffer = 0;

    if( !stream->isBlocking )
    {
        /* (*stream->audioPlayer)->GetInterface( stream->audioPlayer, SL_IID_PREFETCHSTATUS, &stream->prefetchStatusItf );
         * (*stream->prefetchStatusItf)->SetCallbackEventsMask( stream->prefetchStatusItf,
         *                                                      SL_PREFETCHEVENT_STATUSCHANGE );
         * (*stream->prefetchStatusItf)->SetFillUpdatePeriod( stream->prefetchStatusItf, 200 );
         * Disabled this for now, because the stream gets aborted from android_audioPlayer_bufferQueue_onRefilled_l
         * (*stream->prefetchStatusItf)->RegisterCallback( stream->prefetchStatusItf, PrefetchStatusCallback, (void*) stream );
         */
    }
    (*stream->outputBufferQueueItf)->RegisterCallback( stream->outputBufferQueueItf, NotifyBufferFreeCallback, &stream->outputSem );
    sem_init( &stream->outputSem, 0, 0 );

error:
    return result;
}

static PaError InitializeInputStream( PaOpenslesHostApiRepresentation *openslesHostApi, OpenslesStream *stream,
                                      SLint32 androidRecordingPreset, double sampleRate )
{
    PaError result = paNoError;
    SLresult slResult = SL_RESULT_SUCCESS;
    int i, j;
    const SLuint32 channelMasks[] = { SL_SPEAKER_FRONT_CENTER, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT };

    SLDataLocator_IODevice inputLocator = {SL_DATALOCATOR_IODEVICE,
                                           SL_IODEVICE_AUDIOINPUT,
                                           SL_DEFAULTDEVICEID_AUDIOINPUT, NULL};
    SLDataSource audioSrc = {&inputLocator, NULL};

    SLDataFormat_PCM  formatPcm = { SL_DATAFORMAT_PCM, stream->bufferProcessor.outputChannelCount,
                                    sampleRate * 1000.0, SL_PCMSAMPLEFORMAT_FIXED_16,
                                    SL_PCMSAMPLEFORMAT_FIXED_16,
                                    channelMasks[stream->bufferProcessor.outputChannelCount - 1],
                                    SL_BYTEORDER_LITTLEENDIAN };

    SLDataLocator_AndroidSimpleBufferQueue inputBQLocator = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
                                                              numberOfBuffers };
    SLDataSink audioSink = {&inputBQLocator, &formatPcm};

    const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION };
    const SLboolean req[] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
    const unsigned interfaceCount = 2;
    slResult = (*openslesHostApi->slEngineItf)->CreateAudioRecorder(openslesHostApi->slEngineItf, &stream->audioRecorder,
                                                                    &audioSrc, &audioSink, interfaceCount, ids, req);

    if( slResult != SL_RESULT_SUCCESS )
    {

        result = paUnanticipatedHostError;
        goto error;
    }

#if __ANDROID_API__ >= 14
    (*stream->audioRecorder)->GetInterface( stream->audioRecorder, SL_IID_ANDROIDCONFIGURATION, &stream->inputConfigurationItf );
    (*stream->inputConfigurationItf)->SetConfiguration( stream->inputConfigurationItf, SL_ANDROID_KEY_STREAM_TYPE,
                                                   &androidRecordingPreset, sizeof(androidRecordingPreset) );
#endif

    slResult = (*stream->audioRecorder)->Realize( stream->audioRecorder, SL_BOOLEAN_FALSE );
    if( slResult != SL_RESULT_SUCCESS )
    {
        (*stream->audioRecorder)->Destroy( stream->audioRecorder );
        result = paUnanticipatedHostError;
        goto error;
    }

    (*stream->audioRecorder)->GetInterface( stream->audioRecorder,
                                            SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                            &stream->inputBufferQueueItf );
    (*stream->audioRecorder)->GetInterface( stream->audioRecorder,
                                            SL_IID_RECORD,
                                            &stream->recorderItf );

    stream->inputBuffers = (void **) PaUtil_AllocateMemory( numberOfBuffers * sizeof(SLint16 *) );
    for( i = 0; i < numberOfBuffers; ++i )
    {
        stream->inputBuffers[i] = (void*) PaUtil_AllocateMemory( stream->framesPerHostCallback
                                                                 * stream->bytesPerFrame
                                                                 * stream->bufferProcessor.inputChannelCount );
        if( !stream->inputBuffers[i] )
        {
            for( j = 0; j < i; ++j )
                PaUtil_FreeMemory( stream->inputBuffers[j] );
            PaUtil_FreeMemory( stream->inputBuffers );
            (*stream->audioRecorder)->Destroy( stream->audioRecorder );
            result = paInsufficientMemory;
            goto error;
        }
    }
    stream->currentInputBuffer = 0;
    (*stream->inputBufferQueueItf)->RegisterCallback( stream->inputBufferQueueItf,
                                                      NotifyBufferFreeCallback, &stream->inputSem );
    sem_init( &stream->inputSem, 0, 0 );

error:
    return result;
}

static void StreamProcessingCallback( void *userData )
{
    OpenslesStream *stream = (OpenslesStream*)userData;
    PaStreamCallbackTimeInfo timeInfo = {0,0,0};
    unsigned long framesProcessed = 0;
    struct timespec timeSpec;

    while( 1 )
    {
        framesProcessed = 0;

        clock_gettime( CLOCK_REALTIME, &timeSpec );
        timeInfo.currentTime = (PaTime)(timeSpec.tv_sec + (timeSpec.tv_nsec / 1000000000.0));
        timeInfo.outputBufferDacTime = (PaTime)(stream->framesPerHostCallback
                                                 / stream->streamRepresentation.streamInfo.sampleRate
                                                 + timeInfo.currentTime);
        timeInfo.inputBufferAdcTime = (PaTime)(stream->framesPerHostCallback
                                                / stream->streamRepresentation.streamInfo.sampleRate
                                                + timeInfo.currentTime);

        /* check if StopStream or AbortStream was called */
        if( stream->doStop )
            stream->callbackResult = paComplete;
        else if( stream->doAbort )
            stream->callbackResult = paAbort;

        PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );
        PaUtil_BeginBufferProcessing( &stream->bufferProcessor, &timeInfo, stream->cbFlags );

        if( stream->hasOutput )
        {
            sem_wait( &stream->outputSem );
            PaUtil_SetOutputFrameCount( &stream->bufferProcessor, 0 );
            PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor, 0,
                                                 (void*) ((SLint16 **)stream->outputBuffers)[stream->currentOutputBuffer], 0 );
        }
        if( stream->hasInput )
        {
            sem_wait( &stream->inputSem );
            PaUtil_SetInputFrameCount( &stream->bufferProcessor, 0 );
            PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor, 0,
                                                (void*) ((SLint16 **)stream->inputBuffers)[stream->currentInputBuffer], 0 );
        }

        /* continue processing user buffers if cbresult is pacontinue or if cbresult is  pacomplete and userbuffers aren't empty yet  */
        if( stream->callbackResult == paContinue
            || ( stream->callbackResult == paComplete
                 && !PaUtil_IsBufferProcessorOutputEmpty( &stream->bufferProcessor )) )
            framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor, &stream->callbackResult );

        /* enqueue a buffer only when there are frames to be processed,
         * this will be 0 when paComplete + empty buffers or paAbort
         */
        if( framesProcessed  > 0 )
        {
            if( stream->hasOutput )
            {
                (*stream->outputBufferQueueItf)->Enqueue( stream->outputBufferQueueItf,
                                                          (void*) stream->outputBuffers[stream->currentOutputBuffer],
                                                          framesProcessed * stream->bytesPerFrame
                                                          * stream->bufferProcessor.outputChannelCount );
                stream->currentOutputBuffer = (stream->currentOutputBuffer + 1) % numberOfBuffers;
            }
            if( stream->hasInput )
            {
                (*stream->inputBufferQueueItf)->Enqueue( stream->inputBufferQueueItf,
                                                         (void*) stream->inputBuffers[stream->currentInputBuffer],
                                                         framesProcessed * stream->bytesPerFrame
                                                         * stream->bufferProcessor.inputChannelCount );
                stream->currentInputBuffer = (stream->currentInputBuffer + 1) % numberOfBuffers;
            }
        }

        PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed);

        if( framesProcessed == 0 && stream->doStop ) /* StopStream was called */
        {
            if( stream->hasOutput )
            {
                (*stream->playerItf)->SetPlayState( stream->playerItf, SL_PLAYSTATE_STOPPED );
                (*stream->outputBufferQueueItf)->Clear( stream->outputBufferQueueItf );

            }
            if( stream->hasInput )
            {
                (*stream->recorderItf)->SetRecordState( stream->recorderItf, SL_RECORDSTATE_STOPPED );
                (*stream->inputBufferQueueItf)->Clear( stream->inputBufferQueueItf );
            }
            return;
        }
        else if( framesProcessed == 0 && !(stream->doAbort || stream->doStop) ) /* if AbortStream or StopStream weren't called, stop from the cb */
        {
            if( stream->hasOutput )
            {
                (*stream->playerItf)->SetPlayState( stream->playerItf, SL_PLAYSTATE_STOPPED );
                (*stream->outputBufferQueueItf)->Clear( stream->outputBufferQueueItf );
            }
            if( stream->hasInput )
            {
                (*stream->recorderItf)->SetRecordState( stream->recorderItf, SL_RECORDSTATE_STOPPED );
                (*stream->inputBufferQueueItf)->Clear( stream->inputBufferQueueItf );
            }

            stream->isActive = SL_BOOLEAN_FALSE;
            stream->isStopped = SL_BOOLEAN_TRUE;
            if( stream->streamRepresentation.streamFinishedCallback != NULL )
                stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
           return;
        }
    }
}

/* static void PrefetchStatusCallback( SLPrefetchStatusItf prefetchStatusItf, void *userData, SLuint32 event )
 * {
 *     SLuint32 prefetchStatus;
 *     OpenslesStream *stream = (OpenslesStream*) userData;

 *     (*stream->prefetchStatusItf)->GetPrefetchStatus( stream->prefetchStatusItf, &prefetchStatus );
 *     if( event & SL_PREFETCHEVENT_STATUSCHANGE && prefetchStatus == SL_PREFETCHSTATUS_UNDERFLOW )
 *         stream->cbFlags = paOutputUnderflow;
 *     else if( event & SL_PREFETCHEVENT_STATUSCHANGE && prefetchStatus == SL_PREFETCHSTATUS_OVERFLOW )
 *         stream->cbFlags = paOutputOverflow;
 * }
 */

static void NotifyBufferFreeCallback( SLAndroidSimpleBufferQueueItf bufferQueueItf, void *userData )
{
    sem_t *sem = (sem_t*) userData;
    sem_post( sem );
}

static PaError CloseStream( PaStream* s )
{
    PaError result = paNoError;
    OpenslesStream *stream = (OpenslesStream*)s;
    int i;

    if( stream->hasOutput )
    {
        sem_destroy( &stream->outputSem );
        (*stream->audioPlayer)->Destroy( stream->audioPlayer );
        (*stream->outputMixObject)->Destroy( stream->outputMixObject );
    }
    if( stream->hasInput )
    {
        sem_destroy( &stream->inputSem );
        (*stream->audioRecorder)->Destroy( stream->audioRecorder );
    }

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );

    for( i = 0; i < numberOfBuffers; ++i )
    {
        if( stream->hasOutput )
            PaUtil_FreeMemory( stream->outputBuffers[i] );
        if( stream->hasInput )
            PaUtil_FreeMemory( stream->inputBuffers[i] );
    }

    if( stream->hasOutput )
        PaUtil_FreeMemory( stream->outputBuffers );
    if( stream->hasInput )
        PaUtil_FreeMemory( stream->inputBuffers );

    PaUtil_FreeMemory( stream );
    return result;
}

static PaError StartStream( PaStream *s )
{
    SLresult slResult;
    PaError result = paNoError;
    OpenslesStream *stream = (OpenslesStream*)s;
    int i;

    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );

    stream->isStopped = SL_BOOLEAN_FALSE;
    stream->isActive = SL_BOOLEAN_TRUE;
    stream->doStop = SL_BOOLEAN_FALSE;
    stream->doAbort = SL_BOOLEAN_FALSE;
    stream->currentOutputBuffer = 0;
    stream->currentInputBuffer = 0;

    /* Initialize buffers */
    for( i = 0; i < numberOfBuffers; ++i )
    {
        if( stream->hasOutput )
        {
            memset( stream->outputBuffers[stream->currentOutputBuffer], 0,
                    stream->framesPerHostCallback * stream->bytesPerFrame
                    * stream->bufferProcessor.outputChannelCount );
            slResult = (*stream->outputBufferQueueItf)->Enqueue( stream->outputBufferQueueItf,
                                                                 (void*) stream->outputBuffers[stream->currentOutputBuffer],
                                                                 stream->framesPerHostCallback * stream->bytesPerFrame
                                                                 * stream->bufferProcessor.outputChannelCount  );
            if( slResult != SL_RESULT_SUCCESS )
                goto error;
            stream->currentOutputBuffer = (stream->currentOutputBuffer + 1) % numberOfBuffers;
        }
        if( stream->hasInput )
        {
            memset( stream->inputBuffers[stream->currentInputBuffer], 0,
                    stream->framesPerHostCallback * stream->bytesPerFrame * stream->bufferProcessor.inputChannelCount );
            slResult = (*stream->inputBufferQueueItf)->Enqueue( stream->inputBufferQueueItf,
                                                                 (void*) stream->inputBuffers[stream->currentInputBuffer],
                                                                 stream->framesPerHostCallback * stream->bytesPerFrame
                                                                 * stream->bufferProcessor.inputChannelCount  );
            if( slResult != SL_RESULT_SUCCESS )
                goto error;
            stream->currentInputBuffer = (stream->currentInputBuffer + 1) % numberOfBuffers;
        }
    }

    /* Start the processing thread */
    if( !stream->isBlocking )
    {
        stream->callbackResult = paContinue;
        PaUnixThread_New(&stream->streamThread, (void*) StreamProcessingCallback,
                         (void *) stream, 0, 0);
    }

    /* Start OpenSL ES devices */
    if( stream->hasOutput )
    {
        slResult = (*stream->playerItf)->SetPlayState( stream->playerItf, SL_PLAYSTATE_PLAYING );
        if( slResult != SL_RESULT_SUCCESS )
            goto error;
    }
    if( stream->hasInput )
    {
        slResult = (*stream->recorderItf)->SetRecordState( stream->recorderItf, SL_RECORDSTATE_RECORDING);
        if( slResult != SL_RESULT_SUCCESS )
            goto error;
    }

    return result;
error:
    return paUnanticipatedHostError;
}

static PaError StopStream( PaStream *s )
{
    PaError result = paNoError;
    OpenslesStream *stream = (OpenslesStream*)s;
    SLAndroidSimpleBufferQueueState state;

    if( stream->isBlocking )
    {
        if( stream->hasOutput )
        {
            do {
                (*stream->outputBufferQueueItf)->GetState( stream->outputBufferQueueItf, &state);
            } while( state.count > 0 );
            (*stream->playerItf)->SetPlayState( stream->playerItf, SL_PLAYSTATE_STOPPED );
            (*stream->outputBufferQueueItf)->Clear( stream->outputBufferQueueItf );
        }
        if( stream->hasInput )
        {
            do {
                (*stream->inputBufferQueueItf)->GetState( stream->inputBufferQueueItf, &state);
            } while( state.count > 0 );
            (*stream->recorderItf)->SetRecordState( stream->recorderItf, SL_RECORDSTATE_STOPPED );
            (*stream->inputBufferQueueItf)->Clear( stream->inputBufferQueueItf );
        }
        stream->isActive = SL_BOOLEAN_FALSE;
        stream->isStopped = SL_BOOLEAN_TRUE;
    }
    else
    {
        stream->doStop = SL_BOOLEAN_TRUE;
        PaUnixThread_Terminate( &stream->streamThread, 1, &result );
    }

    stream->isActive = SL_BOOLEAN_FALSE;
    stream->isStopped = SL_BOOLEAN_TRUE;
    if( stream->streamRepresentation.streamFinishedCallback != NULL )
        stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );

    return result;
}

static PaError AbortStream( PaStream *s )
{
    PaError result = paNoError;
    OpenslesStream *stream = (OpenslesStream*)s;

    if( !stream->isBlocking )
    {
        stream->doAbort = SL_BOOLEAN_TRUE;
        PaUnixThread_Terminate( &stream->streamThread, 0, &result );
    }

    /* stop immediately so enqueue has no effect */
    if( stream->hasOutput )
        (*stream->playerItf)->SetPlayState( stream->playerItf, SL_PLAYSTATE_STOPPED );
    if( stream->hasInput)
        (*stream->recorderItf)->SetRecordState( stream->recorderItf, SL_RECORDSTATE_STOPPED );

    stream->isActive = SL_BOOLEAN_FALSE;
    stream->isStopped = SL_BOOLEAN_TRUE;
    if( stream->streamRepresentation.streamFinishedCallback != NULL )
        stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );

    return result;
}

static PaError IsStreamStopped( PaStream *s )
{
    OpenslesStream *stream = (OpenslesStream*)s;
    return stream->isStopped;
}

static PaError IsStreamActive( PaStream *s )
{
    OpenslesStream *stream = (OpenslesStream*)s;
    return stream->isActive;
}

static PaTime GetStreamTime( PaStream *s )
{
    return PaUtil_GetTime();
}

static double GetStreamCpuLoad( PaStream* s )
{
    OpenslesStream *stream = (OpenslesStream*)s;

    return PaUtil_GetCpuLoad( &stream->cpuLoadMeasurer );
}

static PaError ReadStream( PaStream* s,
                           void *buffer,
                           unsigned long frames )
{
    OpenslesStream *stream = (OpenslesStream*)s;
    void *userBuffer = buffer;
    unsigned framesToRead;

    while( frames > 0 )
    {
        sem_wait( &stream->inputSem );
        framesToRead = PA_MIN( stream->framesPerHostCallback, frames );
        PaUtil_SetInputFrameCount( &stream->bufferProcessor, framesToRead );
        PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor, 0,
                                            stream->inputBuffers[stream->currentInputBuffer], 0 );

        (*stream->inputBufferQueueItf)->Enqueue( stream->inputBufferQueueItf,
                                                 stream->inputBuffers[stream->currentInputBuffer],
                                                 framesToRead * stream->bytesPerFrame
                                                 * stream->bufferProcessor.inputChannelCount );
         PaUtil_CopyInput( &stream->bufferProcessor, &userBuffer, framesToRead );
         stream->currentInputBuffer = (stream->currentInputBuffer + 1) % numberOfBuffers;

         frames -= framesToRead;
    }

    return paNoError;
}

static PaError WriteStream( PaStream* s,
                            const void *buffer,
                            unsigned long frames )
{
    OpenslesStream *stream = (OpenslesStream*)s;
    const void *userBuffer = buffer;
    unsigned framesToWrite;

    while( frames > 0 )
    {
        sem_wait( &stream->outputSem );
        framesToWrite = PA_MIN( stream->framesPerHostCallback, frames );
        PaUtil_SetOutputFrameCount( &stream->bufferProcessor, framesToWrite );
        PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor, 0,
                                             stream->outputBuffers[stream->currentOutputBuffer], 0 );
        PaUtil_CopyOutput( &stream->bufferProcessor, &userBuffer, framesToWrite);
        (*stream->outputBufferQueueItf)->Enqueue( stream->outputBufferQueueItf, stream->outputBuffers[stream->currentOutputBuffer],
                                                  framesToWrite * stream->bytesPerFrame
                                                  * stream->bufferProcessor.outputChannelCount );
        stream->currentOutputBuffer = (stream->currentOutputBuffer + 1) % numberOfBuffers;
        frames -= framesToWrite;
    }
    return paNoError;
}

static signed long GetStreamReadAvailable( PaStream* s )
{
    OpenslesStream *stream = (OpenslesStream*)s;
    SLAndroidSimpleBufferQueueState state;

    (*stream->inputBufferQueueItf)->GetState( stream->inputBufferQueueItf, &state );
    return stream->framesPerHostCallback * ( numberOfBuffers - state.count );
}

static signed long GetStreamWriteAvailable( PaStream* s )
{
    OpenslesStream *stream = (OpenslesStream*)s;
    SLAndroidSimpleBufferQueueState state;

    (*stream->outputBufferQueueItf)->GetState( stream->outputBufferQueueItf, &state );
    return stream->framesPerHostCallback * ( numberOfBuffers - state.count );
}

static unsigned long GetApproximateLowBufferSize()
{
    if( __ANDROID_API__ <= 14 )
        return 1024;
    else if( __ANDROID_API__ <= 20 )
        return 512;
    else if( __ANDROID_API__ <= 23 )
        return 256;
    else
        return 192;
}

void PaOpenSLES_SetNativeBufferSize( unsigned long bufferSize )
{
    nativeBufferSize = bufferSize;
}

void PaOpenSLES_SetNumberOfBuffers( unsigned buffers )
{
    numberOfBuffers = buffers;
}
