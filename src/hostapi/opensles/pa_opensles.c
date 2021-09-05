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
#include <malloc.h>
#include <string.h>
#include <time.h>

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
            PaUtil_DebugPrint(( "Expression '" #expr "' failed in '" __FILE__ "', line: " PA_STRINGIZE( __LINE__ ) "\n" )); \
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

typedef struct OpenslesOutputStream
{
    SLObjectItf audioPlayer;
    SLObjectItf outputMixObject;
    SLPlayItf playerItf;
    SLAndroidSimpleBufferQueueItf outputBufferQueueItf;
    /* SLPrefetchStatusItf prefetchStatusItf; */
    SLAndroidConfigurationItf outputConfigurationItf;

    sem_t outputSem;

    void **outputBuffers;
    int currentOutputBuffer;

    unsigned bytesPerSample;
}
OpenslesOutputStream;

typedef struct OpenslesInputStream
{
    SLObjectItf audioRecorder;
    SLRecordItf recorderItf;
    SLAndroidSimpleBufferQueueItf inputBufferQueueItf;
    SLAndroidConfigurationItf inputConfigurationItf;

    sem_t inputSem;

    void **inputBuffers;
    int currentInputBuffer;

    unsigned bytesPerSample;
}
OpenslesInputStream;

typedef struct OpenslesStream
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

    SLboolean isBlocking;
    SLboolean hasOutput;
    SLboolean hasInput;

    // Used between callback thread and main thread while it's running
    volatile SLboolean isStopped;
    volatile SLboolean isActive;
    volatile SLboolean doStop;
    volatile SLboolean doAbort;

    PaStreamCallbackFlags cbFlags;
    PaUnixThread streamThread;

    unsigned long framesPerHostCallback;

    OpenslesOutputStream *outputStream;
    OpenslesInputStream *inputStream;

}
OpenslesStream;

static PaError InitializeOutputStream( PaOpenslesHostApiRepresentation *openslesHostApi, OpenslesStream *stream,
                                       SLint32 androidPlaybackStreamType, double sampleRate );
static PaError InitializeInputStream( PaOpenslesHostApiRepresentation *openslesHostApi, OpenslesStream *stream,
                                      SLint32 androidRecordingPreset, double sampleRate );
static void StreamProcessingCallback( void *userData );
static void NotifyBufferFreeCallback( SLAndroidSimpleBufferQueueItf bufferQueueItf, void *userData );
/* static void PrefetchStatusCallback( SLPrefetchStatusItf prefetchStatusItf, void *userData, SLuint32 event ); */
static PaError End( OpenslesStream *stream, SLboolean markStopped );

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

    if( framesPerBuffer == paFramesPerBufferUnspecified ) {
        if( outputParameters ) {
            framesPerHostBuffer = (unsigned long) (outputParameters->suggestedLatency * sampleRate);
        }
        else {
            framesPerHostBuffer = (unsigned long) (inputParameters->suggestedLatency * sampleRate);
        }
    }
    else {
        framesPerHostBuffer = framesPerBuffer;
    }

    stream = (OpenslesStream*)PaUtil_AllocateMemory( sizeof(OpenslesStream) );

    if( !stream )
    {
        result = paInsufficientMemory;
        goto error;
    }
    stream->inputStream = 0;
    stream->outputStream = 0;

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
    stream->cbFlags = 0;
    stream->isStopped = SL_BOOLEAN_TRUE;
    stream->isActive = SL_BOOLEAN_FALSE;

    if( !stream->isBlocking )
        PaUnixThreading_Initialize();

    if( inputChannelCount > 0 )
    {
        stream->inputStream = (OpenslesInputStream *)PaUtil_AllocateMemory( sizeof(OpenslesInputStream) );
        if( !stream->inputStream ) {
            result = paInsufficientMemory;
            goto error;
        }

        stream->hasInput = SL_BOOLEAN_TRUE;
        stream->inputStream->bytesPerSample = sizeof(SLint16);
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
        stream->outputStream = (OpenslesOutputStream *)PaUtil_AllocateMemory( sizeof(OpenslesOutputStream) );
        if( !stream->outputStream ) {
            result = paInsufficientMemory;
            goto error;
        }

        stream->hasOutput = SL_BOOLEAN_TRUE;
        stream->outputStream->bytesPerSample = sizeof(SLint16);
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
    if( stream ) {
        if( stream->inputStream )
            PaUtil_FreeMemory( stream->inputStream );
        if( stream->outputStream )
            PaUtil_FreeMemory( stream->outputStream );
        PaUtil_FreeMemory( stream );
    }
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

    (*openslesHostApi->slEngineItf)->CreateOutputMix( openslesHostApi->slEngineItf, &stream->outputStream->outputMixObject, 0, NULL, NULL );
    (*stream->outputStream->outputMixObject)->Realize( stream->outputStream->outputMixObject, SL_BOOLEAN_FALSE );
    SLDataLocator_OutputMix outputLocator = { SL_DATALOCATOR_OUTPUTMIX, stream->outputStream->outputMixObject };
    SLDataSink audioSink = { &outputLocator, &formatPcm };

    if( !stream->isBlocking )
    {
        const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION };
        const SLboolean req[] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
        const unsigned interfaceCount = 2;
        slResult = (*openslesHostApi->slEngineItf)->CreateAudioPlayer(openslesHostApi->slEngineItf, &stream->outputStream->audioPlayer,
                                                                      &audioSrc, &audioSink, interfaceCount, ids, req);
    }
    else
    {
        const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION };
        const SLboolean req[] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
        const unsigned interfaceCount = 2;
        slResult = (*openslesHostApi->slEngineItf)->CreateAudioPlayer( openslesHostApi->slEngineItf, &stream->outputStream->audioPlayer,
                                                                       &audioSrc, &audioSink, interfaceCount, ids, req );
    }
    if( slResult != SL_RESULT_SUCCESS )
    {
        (*stream->outputStream->outputMixObject)->Destroy( stream->outputStream->outputMixObject );
        result = paUnanticipatedHostError;
        goto error;
    }

#if __ANDROID_API__ >= 14
    (*stream->outputStream->audioPlayer)->GetInterface( stream->outputStream->audioPlayer, SL_IID_ANDROIDCONFIGURATION, &stream->outputStream->outputConfigurationItf );
    (*stream->outputStream->outputConfigurationItf)->SetConfiguration( stream->outputStream->outputConfigurationItf, SL_ANDROID_KEY_STREAM_TYPE,
                                                   &androidPlaybackStreamType, sizeof(androidPlaybackStreamType) );
#endif

    slResult = (*stream->outputStream->audioPlayer)->Realize( stream->outputStream->audioPlayer, SL_BOOLEAN_FALSE );
    if( slResult != SL_RESULT_SUCCESS )
    {
        (*stream->outputStream->audioPlayer)->Destroy( stream->outputStream->audioPlayer );
        (*stream->outputStream->outputMixObject)->Destroy( stream->outputStream->outputMixObject );
        result = paUnanticipatedHostError;
        goto error;
    }

    (*stream->outputStream->audioPlayer)->GetInterface( stream->outputStream->audioPlayer, SL_IID_PLAY, &stream->outputStream->playerItf );
    (*stream->outputStream->audioPlayer)->GetInterface( stream->outputStream->audioPlayer, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &stream->outputStream->outputBufferQueueItf );

    stream->outputStream->outputBuffers = (void **) PaUtil_AllocateMemory( numberOfBuffers * sizeof(SLint16 *) );
    for( i = 0; i < numberOfBuffers; ++i )
    {
        stream->outputStream->outputBuffers[i] = (void*) PaUtil_AllocateMemory( stream->framesPerHostCallback * stream->outputStream->bytesPerSample
                                                                  * stream->bufferProcessor.outputChannelCount );
        if( !stream->outputStream->outputBuffers[i] )
        {
            for( j = 0; j < i; ++j )
                PaUtil_FreeMemory( stream->outputStream->outputBuffers[j] );
            PaUtil_FreeMemory( stream->outputStream->outputBuffers );
            (*stream->outputStream->audioPlayer)->Destroy( stream->outputStream->audioPlayer );
            (*stream->outputStream->outputMixObject)->Destroy( stream->outputStream->outputMixObject );
            result = paInsufficientMemory;
            goto error;
        }
    }
    stream->outputStream->currentOutputBuffer = 0;

    if( !stream->isBlocking )
    {
        /* (*stream->outputStream->audioPlayer)->GetInterface( stream->outputStream->audioPlayer, SL_IID_PREFETCHSTATUS, &stream->prefetchStatusItf );
         * (*stream->prefetchStatusItf)->SetCallbackEventsMask( stream->prefetchStatusItf,
         *                                                      SL_PREFETCHEVENT_STATUSCHANGE );
         * (*stream->prefetchStatusItf)->SetFillUpdatePeriod( stream->prefetchStatusItf, 200 );
         * Disabled this for now, because the stream gets aborted from android_audioPlayer_bufferQueue_onRefilled_l
         * (*stream->prefetchStatusItf)->RegisterCallback( stream->prefetchStatusItf, PrefetchStatusCallback, (void*) stream );
         */
    }
    (*stream->outputStream->outputBufferQueueItf)->RegisterCallback( stream->outputStream->outputBufferQueueItf, NotifyBufferFreeCallback, &stream->outputStream->outputSem );
    sem_init( &stream->outputStream->outputSem, 0, 0 );

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

    SLDataFormat_PCM  formatPcm = { SL_DATAFORMAT_PCM, stream->bufferProcessor.inputChannelCount,
                                    sampleRate * 1000.0, SL_PCMSAMPLEFORMAT_FIXED_16,
                                    SL_PCMSAMPLEFORMAT_FIXED_16,
                                    channelMasks[stream->bufferProcessor.inputChannelCount - 1],
                                    SL_BYTEORDER_LITTLEENDIAN };

    SLDataLocator_AndroidSimpleBufferQueue inputBQLocator = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
                                                              numberOfBuffers };
    SLDataSink audioSink = {&inputBQLocator, &formatPcm};

    const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION };
    const SLboolean req[] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
    const unsigned interfaceCount = 2;
    slResult = (*openslesHostApi->slEngineItf)->CreateAudioRecorder(openslesHostApi->slEngineItf, &stream->inputStream->audioRecorder,
                                                                    &audioSrc, &audioSink, interfaceCount, ids, req);

    if( slResult != SL_RESULT_SUCCESS )
    {

        result = paUnanticipatedHostError;
        goto error;
    }

#if __ANDROID_API__ >= 14
    (*stream->inputStream->audioRecorder)->GetInterface( stream->inputStream->audioRecorder, SL_IID_ANDROIDCONFIGURATION, &stream->inputStream->inputConfigurationItf );
    (*stream->inputStream->inputConfigurationItf)->SetConfiguration( stream->inputStream->inputConfigurationItf, SL_ANDROID_KEY_STREAM_TYPE,
                                                   &androidRecordingPreset, sizeof(androidRecordingPreset) );
#endif

    slResult = (*stream->inputStream->audioRecorder)->Realize( stream->inputStream->audioRecorder, SL_BOOLEAN_FALSE );
    if( slResult != SL_RESULT_SUCCESS )
    {
        (*stream->inputStream->audioRecorder)->Destroy( stream->inputStream->audioRecorder );
        result = paUnanticipatedHostError;
        goto error;
    }

    (*stream->inputStream->audioRecorder)->GetInterface( stream->inputStream->audioRecorder,
                                            SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                            &stream->inputStream->inputBufferQueueItf );
    (*stream->inputStream->audioRecorder)->GetInterface( stream->inputStream->audioRecorder,
                                            SL_IID_RECORD,
                                            &stream->inputStream->recorderItf );

    stream->inputStream->inputBuffers = (void **) PaUtil_AllocateMemory( numberOfBuffers * sizeof(SLint16 *) );
    for( i = 0; i < numberOfBuffers; ++i )
    {
        stream->inputStream->inputBuffers[i] = (void*) PaUtil_AllocateMemory( stream->framesPerHostCallback
                                                                 * stream->inputStream->bytesPerSample
                                                                 * stream->bufferProcessor.inputChannelCount );
        if( !stream->inputStream->inputBuffers[i] )
        {
            for( j = 0; j < i; ++j )
                PaUtil_FreeMemory( stream->inputStream->inputBuffers[j] );
            PaUtil_FreeMemory( stream->inputStream->inputBuffers );
            (*stream->inputStream->audioRecorder)->Destroy( stream->inputStream->audioRecorder );
            result = paInsufficientMemory;
            goto error;
        }
    }
    stream->inputStream->currentInputBuffer = 0;
    (*stream->inputStream->inputBufferQueueItf)->RegisterCallback( stream->inputStream->inputBufferQueueItf,
                                                      NotifyBufferFreeCallback, &stream->inputStream->inputSem );
    sem_init( &stream->inputStream->inputSem, 0, 0 );

error:
    return result;
}

static void StreamProcessingCallback( void *userData )
{
    OpenslesStream *stream = (OpenslesStream*)userData;
    PaStreamCallbackTimeInfo timeInfo = {0,0,0};
    unsigned long framesProcessed = 0;
    int callbackResult = paContinue;
    SLboolean markStopped = SL_BOOLEAN_TRUE;
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

        PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );
        PaUtil_BeginBufferProcessing( &stream->bufferProcessor, &timeInfo, stream->cbFlags );

        if( stream->hasOutput )
        {
            sem_wait( &stream->outputStream->outputSem );
            PaUtil_SetOutputFrameCount( &stream->bufferProcessor, 0 );
            PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor, 0,
                                                 (void*) ((SLint16 **)stream->outputStream->outputBuffers)[stream->outputStream->currentOutputBuffer], 0 );
        }
        if( stream->hasInput )
        {
            sem_wait( &stream->inputStream->inputSem );
            PaUtil_SetInputFrameCount( &stream->bufferProcessor, 0 );
            PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor, 0,
                                                (void*) ((SLint16 **)stream->inputStream->inputBuffers)[stream->inputStream->currentInputBuffer], 0 );
        }

        /* check if StopStream or AbortStream was called */
        if( stream->doStop )
            callbackResult = paComplete;
        else if( stream->doAbort )
            callbackResult = paAbort;

        /* continue processing user buffers if cbresult is pacontinue or if cbresult is  pacomplete and userbuffers aren't empty yet  */
        if( callbackResult == paContinue
            || ( callbackResult == paComplete
                 && !PaUtil_IsBufferProcessorOutputEmpty( &stream->bufferProcessor )) )
            framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor, &callbackResult );

        /* enqueue a buffer only when there are frames to be processed,
         * this will be 0 when paComplete + empty buffers or paAbort
         */
        if( framesProcessed  > 0 )
        {
            if( stream->hasOutput )
            {
                (*stream->outputStream->outputBufferQueueItf)->Enqueue( stream->outputStream->outputBufferQueueItf,
                                                          (void*) stream->outputStream->outputBuffers[stream->outputStream->currentOutputBuffer],
                                                          framesProcessed * stream->outputStream->bytesPerSample
                                                          * stream->bufferProcessor.outputChannelCount );
                stream->outputStream->currentOutputBuffer = (stream->outputStream->currentOutputBuffer + 1) % numberOfBuffers;
            }
            if( stream->hasInput )
            {
                (*stream->inputStream->inputBufferQueueItf)->Enqueue( stream->inputStream->inputBufferQueueItf,
                                                         (void*) stream->inputStream->inputBuffers[stream->inputStream->currentInputBuffer],
                                                         framesProcessed * stream->inputStream->bytesPerSample
                                                         * stream->bufferProcessor.inputChannelCount );
                stream->inputStream->currentInputBuffer = (stream->inputStream->currentInputBuffer + 1) % numberOfBuffers;
            }
        }

        PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );

        if( stream->doAbort )
            goto end;
        if( framesProcessed == 0 && ( stream->doStop || PaUnixThread_StopRequested( &stream->streamThread ) ) )
            goto end;

        if( callbackResult != paContinue )
        {
            // User should still be able to call StopStream
            markStopped = SL_BOOLEAN_FALSE;
            goto end;
        }
    }
 end:
    End( stream, markStopped );
 exit:
    PaUnixThreading_EXIT( paNoError );

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
        sem_destroy( &stream->outputStream->outputSem );
        (*stream->outputStream->audioPlayer)->Destroy( stream->outputStream->audioPlayer );
        (*stream->outputStream->outputMixObject)->Destroy( stream->outputStream->outputMixObject );
    }
    if( stream->hasInput )
    {
        sem_destroy( &stream->inputStream->inputSem );
        (*stream->inputStream->audioRecorder)->Destroy( stream->inputStream->audioRecorder );
    }

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );

    for( i = 0; i < numberOfBuffers; ++i )
    {
        if( stream->hasOutput )
            PaUtil_FreeMemory( stream->outputStream->outputBuffers[i] );
        if( stream->hasInput )
            PaUtil_FreeMemory( stream->inputStream->inputBuffers[i] );
    }

    if( stream->hasOutput )
        PaUtil_FreeMemory( stream->outputStream->outputBuffers );
    if( stream->hasInput )
        PaUtil_FreeMemory( stream->inputStream->inputBuffers );

    PaUtil_FreeMemory( stream->outputStream );
    PaUtil_FreeMemory( stream->inputStream );
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
    if( stream->hasOutput )
        stream->outputStream->currentOutputBuffer = 0;
    if( stream->hasInput )
        stream->inputStream->currentInputBuffer = 0;

    /* Initialize buffers */
    for( i = 0; i < numberOfBuffers; ++i )
    {
        if( stream->hasOutput )
        {
            memset( stream->outputStream->outputBuffers[stream->outputStream->currentOutputBuffer], 0,
                    stream->framesPerHostCallback * stream->outputStream->bytesPerSample
                    * stream->bufferProcessor.outputChannelCount );
            slResult = (*stream->outputStream->outputBufferQueueItf)->Enqueue( stream->outputStream->outputBufferQueueItf,
                                                                 (void*) stream->outputStream->outputBuffers[stream->outputStream->currentOutputBuffer],
                                                                 stream->framesPerHostCallback * stream->outputStream->bytesPerSample
                                                                 * stream->bufferProcessor.outputChannelCount  );
            if( slResult != SL_RESULT_SUCCESS )
                goto error;
            stream->outputStream->currentOutputBuffer = (stream->outputStream->currentOutputBuffer + 1) % numberOfBuffers;
        }
        if( stream->hasInput )
        {
            memset( stream->inputStream->inputBuffers[stream->inputStream->currentInputBuffer], 0,
                    stream->framesPerHostCallback * stream->inputStream->bytesPerSample * stream->bufferProcessor.inputChannelCount );
            slResult = (*stream->inputStream->inputBufferQueueItf)->Enqueue( stream->inputStream->inputBufferQueueItf,
                                                                 (void*) stream->inputStream->inputBuffers[stream->inputStream->currentInputBuffer],
                                                                 stream->framesPerHostCallback * stream->inputStream->bytesPerSample
                                                                 * stream->bufferProcessor.inputChannelCount  );
            if( slResult != SL_RESULT_SUCCESS )
                goto error;
            stream->inputStream->currentInputBuffer = (stream->inputStream->currentInputBuffer + 1) % numberOfBuffers;
        }
    }

    /* Start the processing thread */
    if( !stream->isBlocking )
    {
        PaUnixThread_New(&stream->streamThread, (void*) StreamProcessingCallback,
                         (void *) stream, 0, 0);
    }

    /* Start OpenSL ES devices */
    if( stream->hasOutput )
    {
        slResult = (*stream->outputStream->playerItf)->SetPlayState( stream->outputStream->playerItf, SL_PLAYSTATE_PLAYING );
        if( slResult != SL_RESULT_SUCCESS )
            goto error;
    }
    if( stream->hasInput )
    {
        slResult = (*stream->inputStream->recorderItf)->SetRecordState( stream->inputStream->recorderItf, SL_RECORDSTATE_RECORDING);
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

    if( stream->isBlocking )
    {
        result = End( stream, SL_BOOLEAN_TRUE );
    }
    else
    {
        stream->doStop = SL_BOOLEAN_TRUE;
        PaUnixThread_Terminate( &stream->streamThread, 1, &result );
    }

    return result;
}

static PaError AbortStream( PaStream *s )
{
    PaError result = paNoError;
    OpenslesStream *stream = (OpenslesStream*)s;

    if( stream->isBlocking )
    {
        result = End( stream, SL_BOOLEAN_TRUE );
    }
    else
    {
        stream->doAbort = SL_BOOLEAN_TRUE;
        result = PaUnixThread_Terminate( &stream->streamThread, 0, &result );
    }

    return result;
}


/* Called from StopStream or AbortStream if blocking; the callback if non-blocking */
static PaError End( OpenslesStream *stream, SLboolean markStopped )
{
    PaError result = paNoError;
    SLAndroidSimpleBufferQueueState state;

    PaUtil_ResetCpuLoadMeasurer( &stream->cpuLoadMeasurer );

    if( stream->hasOutput )
    {
        if( stream->isBlocking )
        {
            do {
                (*stream->outputStream->outputBufferQueueItf)->GetState( stream->outputStream->outputBufferQueueItf, &state );
            } while( state.count > 0 );
        }
        (*stream->outputStream->playerItf)->SetPlayState( stream->outputStream->playerItf, SL_PLAYSTATE_STOPPED );
        (*stream->outputStream->outputBufferQueueItf)->Clear( stream->outputStream->outputBufferQueueItf );

    }
    if( stream->hasInput )
    {
        if( stream->isBlocking )
        {
            do {
                (*stream->inputStream->inputBufferQueueItf)->GetState( stream->inputStream->inputBufferQueueItf, &state );
            } while( state.count > 0 );
        }
        (*stream->inputStream->recorderItf)->SetRecordState( stream->inputStream->recorderItf, SL_RECORDSTATE_STOPPED );
        (*stream->inputStream->inputBufferQueueItf)->Clear( stream->inputStream->inputBufferQueueItf );
    }

    stream->isActive = SL_BOOLEAN_FALSE;
    if( markStopped )
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
        sem_wait( &stream->inputStream->inputSem );
        framesToRead = PA_MIN( stream->framesPerHostCallback, frames );
        PaUtil_SetInputFrameCount( &stream->bufferProcessor, framesToRead );
        PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor, 0,
                                            stream->inputStream->inputBuffers[stream->inputStream->currentInputBuffer], 0 );

        (*stream->inputStream->inputBufferQueueItf)->Enqueue( stream->inputStream->inputBufferQueueItf,
                                                 stream->inputStream->inputBuffers[stream->inputStream->currentInputBuffer],
                                                 framesToRead * stream->inputStream->bytesPerSample
                                                 * stream->bufferProcessor.inputChannelCount );
         PaUtil_CopyInput( &stream->bufferProcessor, &userBuffer, framesToRead );
         stream->inputStream->currentInputBuffer = (stream->inputStream->currentInputBuffer + 1) % numberOfBuffers;

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
        sem_wait( &stream->outputStream->outputSem );
        framesToWrite = PA_MIN( stream->framesPerHostCallback, frames );
        PaUtil_SetOutputFrameCount( &stream->bufferProcessor, framesToWrite );
        PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor, 0,
                                             stream->outputStream->outputBuffers[stream->outputStream->currentOutputBuffer], 0 );
        PaUtil_CopyOutput( &stream->bufferProcessor, &userBuffer, framesToWrite);
        (*stream->outputStream->outputBufferQueueItf)->Enqueue( stream->outputStream->outputBufferQueueItf, stream->outputStream->outputBuffers[stream->outputStream->currentOutputBuffer],
                                                  framesToWrite * stream->outputStream->bytesPerSample
                                                  * stream->bufferProcessor.outputChannelCount );
        stream->outputStream->currentOutputBuffer = (stream->outputStream->currentOutputBuffer + 1) % numberOfBuffers;
        frames -= framesToWrite;
    }
    return paNoError;
}

static signed long GetStreamReadAvailable( PaStream* s )
{
    OpenslesStream *stream = (OpenslesStream*)s;
    SLAndroidSimpleBufferQueueState state;

    (*stream->inputStream->inputBufferQueueItf)->GetState( stream->inputStream->inputBufferQueueItf, &state );
    return stream->framesPerHostCallback * ( numberOfBuffers - state.count );
}

static signed long GetStreamWriteAvailable( PaStream* s )
{
    OpenslesStream *stream = (OpenslesStream*)s;
    SLAndroidSimpleBufferQueueState state;

    (*stream->outputStream->outputBufferQueueItf)->GetState( stream->outputStream->outputBufferQueueItf, &state );
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
