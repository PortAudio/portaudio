/*
 * Portable Audio I/O Library WebAudio implementation
 * Copyright (c) 2022 Adam Hilss
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

/** @file
 @ingroup common_src

 @brief WebAudio implementation of support for a host API.
*/


#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> /* strlen() */

#include <emscripten.h>

#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

#define MAX_CHANNELS 32
#define RENDER_FRAMES 128
#define RENDER_CHANNEL_SIZE (RENDER_FRAMES * sizeof(float))
#define RENDER_BUFFER_SIZE (RENDER_CHANNEL_SIZE * MAX_CHANNELS)
#define SAMPLE_BUFFER_SAMPLES (RENDER_FRAMES * MAX_CHANNELS)

static float g_sample_buffer[SAMPLE_BUFFER_SAMPLES];

/* prototypes for functions declared in this file */

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

PaError PaWebAudio_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex index );

#ifdef __cplusplus
}
#endif /* __cplusplus */


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

/* PaWebAudioHostApiRepresentation - host api datastructure specific to this implementation */

typedef struct
{
    PaUtilHostApiRepresentation inheritedHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilStreamInterface blockingStreamInterface;

    PaUtilAllocationGroup *allocations;
}
PaWebAudioHostApiRepresentation;

double GetSampleRate() {
  return EM_ASM_INT({
    return defaultSampleRate;
  });
}

PaError PaWebAudio_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    static const char* kDefaultDeviceName = "Default audio device";

    PaError result = paNoError;
    int i, deviceCount;
    PaWebAudioHostApiRepresentation *webAudioHostApi;
    PaDeviceInfo *deviceInfoArray;

    webAudioHostApi = (PaWebAudioHostApiRepresentation*)PaUtil_AllocateMemory( sizeof(PaWebAudioHostApiRepresentation) );
    if( !webAudioHostApi )
    {
        result = paInsufficientMemory;
        goto error;
    }

    webAudioHostApi->allocations = PaUtil_CreateAllocationGroup();
    if( !webAudioHostApi->allocations )
    {
        result = paInsufficientMemory;
        goto error;
    }

    *hostApi = &webAudioHostApi->inheritedHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paInDevelopment;            /* IMPLEMENT ME: change to correct type id */
    (*hostApi)->info.name = "WebAudio";

    (*hostApi)->info.defaultInputDevice = 0;
    (*hostApi)->info.defaultOutputDevice = 0;

    (*hostApi)->info.deviceCount = 0;

    deviceCount = 1;

    if( deviceCount > 0 )
    {
        (*hostApi)->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
                webAudioHostApi->allocations, sizeof(PaDeviceInfo*) * deviceCount );
        if( !(*hostApi)->deviceInfos )
        {
            result = paInsufficientMemory;
            goto error;
        }

        /* allocate all device info structs in a contiguous block */
        deviceInfoArray = (PaDeviceInfo*)PaUtil_GroupAllocateMemory(
                webAudioHostApi->allocations, sizeof(PaDeviceInfo) * deviceCount );
        if( !deviceInfoArray )
        {
            result = paInsufficientMemory;
            goto error;
        }

        for( i=0; i < deviceCount; ++i )
        {
            char* deviceName = (char*)PaUtil_GroupAllocateMemory( webAudioHostApi->allocations, strlen(kDefaultDeviceName) + 1 );
            if( !deviceName )
            {
                result = paInsufficientMemory;
                goto error;
            }
            strcpy( deviceName, kDefaultDeviceName );

            PaDeviceInfo *deviceInfo = &deviceInfoArray[i];
            deviceInfo->structVersion = 2;
            deviceInfo->hostApi = hostApiIndex;
            deviceInfo->name = deviceName;

            deviceInfo->maxInputChannels = 2;  /* IMPLEMENT ME */
            deviceInfo->maxOutputChannels = 2;  /* IMPLEMENT ME */

            deviceInfo->defaultLowInputLatency = 0.;  /* IMPLEMENT ME */
            deviceInfo->defaultLowOutputLatency = 0.;  /* IMPLEMENT ME */
            deviceInfo->defaultHighInputLatency = 1.;  /* IMPLEMENT ME */
            deviceInfo->defaultHighOutputLatency = 1.;  /* IMPLEMENT ME */

            deviceInfo->defaultSampleRate = GetSampleRate();

            (*hostApi)->deviceInfos[i] = deviceInfo;
            ++(*hostApi)->info.deviceCount;
        }
    }

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface( &webAudioHostApi->callbackStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive,
                                      GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyRead, PaUtil_DummyWrite,
                                      PaUtil_DummyGetReadAvailable, PaUtil_DummyGetWriteAvailable );

    PaUtil_InitializeStreamInterface( &webAudioHostApi->blockingStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive,
                                      GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      ReadStream, WriteStream, GetStreamReadAvailable, GetStreamWriteAvailable );

    return result;

error:
    if( webAudioHostApi )
    {
        if( webAudioHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( webAudioHostApi->allocations );
            PaUtil_DestroyAllocationGroup( webAudioHostApi->allocations );
        }

        PaUtil_FreeMemory( webAudioHostApi );
    }
    return result;
}


static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaWebAudioHostApiRepresentation *webAudioHostApi = (PaWebAudioHostApiRepresentation*)hostApi;

    if( webAudioHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( webAudioHostApi->allocations );
        PaUtil_DestroyAllocationGroup( webAudioHostApi->allocations );
    }

    PaUtil_FreeMemory( webAudioHostApi );
}


static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate )
{
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;

    if( inputParameters )
    {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        /* all standard sample formats are supported by the buffer adapter,
            this implementation doesn't support any custom sample formats */
        if( inputSampleFormat & paCustomFormat )
            return paSampleFormatNotSupported;

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */

        if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        /* check that input device can support inputChannelCount */
        if( inputChannelCount > hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels )
            return paInvalidChannelCount;

        /* validate inputStreamInfo */
        if( inputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
    }
    else
    {
        inputChannelCount = 0;
    }

    if( outputParameters )
    {
        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;

        /* all standard sample formats are supported by the buffer adapter,
            this implementation doesn't support any custom sample formats */
        if( outputSampleFormat & paCustomFormat )
            return paSampleFormatNotSupported;

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */

        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        /* check that output device can support outputChannelCount */
        if( outputChannelCount > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels )
            return paInvalidChannelCount;

        /* validate outputStreamInfo */
        if( outputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
    }
    else
    {
        outputChannelCount = 0;
    }

    /*
        IMPLEMENT ME:

            - if a full duplex stream is requested, check that the combination
                of input and output parameters is supported if necessary

            - check that the device supports sampleRate

        Because the buffer adapter handles conversion between all standard
        sample formats, the following checks are only required if paCustomFormat
        is implemented, or under some other unusual conditions.

            - check that input device can support inputSampleFormat, or that
                we have the capability to convert from inputSampleFormat to
                a native format

            - check that output device can support outputSampleFormat, or that
                we have the capability to convert from outputSampleFormat to
                a native format
    */

    if ( sampleRate != hostApi->deviceInfos[ outputParameters->device ]->defaultSampleRate )
        return paInvalidSampleRate;

    return paFormatIsSupported;
}

/* PaWebAudioStream - a stream data structure specifically for this implementation */

typedef struct PaWebAudioStream
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

    bool isActive;
    bool enableInput;
    bool enableOutput;
}
PaWebAudioStream;

static PaWebAudioStream* g_web_audio_stream = NULL;

/* see pa_hostapi.h for a list of validity guarantees made about OpenStream parameters */

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
    PaWebAudioHostApiRepresentation *webAudioHostApi = (PaWebAudioHostApiRepresentation*)hostApi;
    PaWebAudioStream *stream = 0;
    unsigned long framesPerHostBuffer = RENDER_FRAMES;
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    PaSampleFormat hostInputSampleFormat, hostOutputSampleFormat;


    if( inputParameters )
    {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */

        if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        /* check that input device can support inputChannelCount */
        if( inputChannelCount > hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels )
            return paInvalidChannelCount;

        /* validate inputStreamInfo */
        if( inputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */

        /* IMPLEMENT ME - establish which  host formats are available */
        hostInputSampleFormat =
            PaUtil_SelectClosestAvailableFormat( paFloat32 /* native formats */, inputSampleFormat );
    }
    else
    {
        inputChannelCount = 0;
        inputSampleFormat = hostInputSampleFormat = paFloat32; /* Suppress 'uninitialised var' warnings. */
    }

    if( outputParameters )
    {
        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */

        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        /* check that output device can support inputChannelCount */
        if( outputChannelCount > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels )
            return paInvalidChannelCount;

        /* validate outputStreamInfo */
        if( outputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */

        hostOutputSampleFormat =
            PaUtil_SelectClosestAvailableFormat( paFloat32 /* native formats */, outputSampleFormat );
    }
    else
    {
        outputChannelCount = 0;
        outputSampleFormat = hostOutputSampleFormat = paFloat32; /* Suppress 'uninitialized var' warnings. */
    }

    EM_ASM({
      openAudio($0 > 0);
    }, inputChannelCount);

    /*
        IMPLEMENT ME:

        ( the following two checks are taken care of by PaUtil_InitializeBufferProcessor() FIXME - checks needed? )

            - check that input device can support inputSampleFormat, or that
                we have the capability to convert from outputSampleFormat to
                a native format

            - check that output device can support outputSampleFormat, or that
                we have the capability to convert from outputSampleFormat to
                a native format

            - if a full duplex stream is requested, check that the combination
                of input and output parameters is supported

            - check that the device supports sampleRate

            - alter sampleRate to a close allowable rate if possible / necessary

            - validate suggestedInputLatency and suggestedOutputLatency parameters,
                use default values where necessary
    */

    /* validate platform specific flags */
    if( (streamFlags & paPlatformSpecificFlags) != 0 )
        return paInvalidFlag; /* unexpected platform specific flag */

    stream = (PaWebAudioStream*)PaUtil_AllocateMemory( sizeof(PaWebAudioStream) );
    if( !stream )
    {
        result = paInsufficientMemory;
        goto error;
    }

    if( streamCallback )
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &webAudioHostApi->callbackStreamInterface, streamCallback, userData );
    }
    else
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &webAudioHostApi->blockingStreamInterface, streamCallback, userData );
    }

    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, sampleRate );


    /* we assume a fixed host buffer size in this example, but the buffer processor
        can also support bounded and unknown host buffer sizes by passing
        paUtilBoundedHostBufferSize or paUtilUnknownHostBufferSize instead of
        paUtilFixedHostBufferSize below. */

    result =  PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
              inputChannelCount, inputSampleFormat, hostInputSampleFormat,
              outputChannelCount, outputSampleFormat, hostOutputSampleFormat,
              sampleRate, streamFlags, framesPerBuffer,
              framesPerHostBuffer, paUtilFixedHostBufferSize,
              streamCallback, userData );
    if( result != paNoError )
        goto error;

    /*
        IMPLEMENT ME: initialise the following fields with estimated or actual
        values.
    */
    stream->streamRepresentation.streamInfo.inputLatency =
            (PaTime)PaUtil_GetBufferProcessorInputLatencyFrames(&stream->bufferProcessor) / sampleRate; /* inputLatency is specified in _seconds_ */
    stream->streamRepresentation.streamInfo.outputLatency =
            (PaTime)PaUtil_GetBufferProcessorOutputLatencyFrames(&stream->bufferProcessor) / sampleRate; /* outputLatency is specified in _seconds_ */
    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;

    stream->isActive = 0;

    *s = (PaStream*)stream;
    g_web_audio_stream = stream;

    return result;

error:
    if( stream )
        PaUtil_FreeMemory( stream );

    return result;
}

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

float* EMSCRIPTEN_KEEPALIVE GetSampleBuffer() {
  return g_sample_buffer;
}

/*
    AudioCallback() illustrates the kind of processing which may
    occur in a host implementation.

*/
void WebAudioCallback( void *inputBuffer, void *outputBuffer, PaWebAudioStream *stream )
{
    PaTime now = emscripten_get_now() / 1000.0;
    PaStreamCallbackTimeInfo timeInfo = {now,now,now}; /* IMPLEMENT ME */
    int callbackResult;
    unsigned long framesProcessed;

    PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );

    /*
        IMPLEMENT ME:
            - generate timing information
            - handle buffer slips
    */

    unsigned long frames = RENDER_FRAMES;

    PaUtil_BeginBufferProcessing( &stream->bufferProcessor, &timeInfo, 0 /* IMPLEMENT ME: pass underflow/overflow flags when necessary */ );

    if (stream->bufferProcessor.inputChannelCount > 0)
    {
      PaUtil_SetInputFrameCount( &stream->bufferProcessor, frames );
      PaUtil_SetNonInterleavedInputChannel( &stream->bufferProcessor,
              0,
              inputBuffer);
      PaUtil_SetNonInterleavedInputChannel( &stream->bufferProcessor,
              1,
              inputBuffer + RENDER_CHANNEL_SIZE);
    }

    if (stream->bufferProcessor.outputChannelCount > 0)
    {
      PaUtil_SetOutputFrameCount( &stream->bufferProcessor, frames );
      PaUtil_SetNonInterleavedOutputChannel( &stream->bufferProcessor,
              0,
              outputBuffer);
      PaUtil_SetNonInterleavedOutputChannel( &stream->bufferProcessor,
              1,
              outputBuffer + RENDER_CHANNEL_SIZE);
    }

    callbackResult = paContinue;
    framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor, &callbackResult );

    PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );

    if( callbackResult == paContinue )
    {
        /* nothing special to do */
    }
    else if( callbackResult == paAbort )
    {
        /* IMPLEMENT ME - finish playback immediately  */

        /* once finished, call the finished callback */
        if( stream->streamRepresentation.streamFinishedCallback != 0 )
            stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
    }
    else
    {
        /* User callback has asked us to stop with paComplete or other non-zero value */

        /* IMPLEMENT ME - finish playback once currently queued audio has completed  */

        /* once finished, call the finished callback */
        if( stream->streamRepresentation.streamFinishedCallback != 0 )
            stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
    }
}

int EMSCRIPTEN_KEEPALIVE AudioCallback() {
    if (g_web_audio_stream && g_web_audio_stream->isActive)
    {
        WebAudioCallback(g_sample_buffer, g_sample_buffer, g_web_audio_stream);
    }
    else
    {
        memset(g_sample_buffer, 0, RENDER_BUFFER_SIZE);
    }

    return RENDER_FRAMES;
}

/*
    When CloseStream() is called, the multi-api layer ensures that
    the stream has already been stopped or aborted.
*/
static PaError CloseStream( PaStream* s )
{
    PaError result = paNoError;
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    g_web_audio_stream = NULL;

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );
    PaUtil_FreeMemory( stream );

    EM_ASM({
      closeAudio();
    });

    return result;
}

#ifdef __cplusplus
}
#endif /* __cplusplus */

static PaError StartStream( PaStream *s )
{
    PaError result = paNoError;
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );

    stream->isActive = 1;

    return result;
}


static PaError StopStream( PaStream *s )
{
    PaError result = paNoError;
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    stream->isActive = 0;

    return result;
}


static PaError AbortStream( PaStream *s )
{
    PaError result = paNoError;
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    /* suppress unused variable warnings */

    stream->isActive = 0;

    return result;
}


static PaError IsStreamStopped( PaStream *s )
{
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    return !stream->isActive;
}


static PaError IsStreamActive( PaStream *s )
{
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    return stream->isActive;
}


static PaTime GetStreamTime( PaStream *s )
{
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    /* suppress unused variable warnings */
    (void) stream;

    PaTime now = emscripten_get_now() / 1000.0;
    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return now;
}

static double GetStreamCpuLoad( PaStream* s )
{
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    return PaUtil_GetCpuLoad( &stream->cpuLoadMeasurer );
}


/*
    As separate stream interfaces are used for blocking and callback
    streams, the following functions can be guaranteed to only be called
    for blocking streams.
*/

static PaError ReadStream( PaStream* s,
                           void *buffer,
                           unsigned long frames )
{
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    /* suppress unused variable warnings */
    (void) buffer;
    (void) frames;
    (void) stream;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return paNoError;
}


static PaError WriteStream( PaStream* s,
                            const void *buffer,
                            unsigned long frames )
{
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    /* suppress unused variable warnings */
    (void) buffer;
    (void) frames;
    (void) stream;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return paNoError;
}


static signed long GetStreamReadAvailable( PaStream* s )
{
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    /* suppress unused variable warnings */
    (void) stream;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}


static signed long GetStreamWriteAvailable( PaStream* s )
{
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    /* suppress unused variable warnings */
    (void) stream;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}
