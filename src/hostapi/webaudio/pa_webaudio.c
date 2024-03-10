/*
 * Portable Audio I/O Library Web Audio implementation
 * Copyright (c) 2024 fwcd
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

 @brief Web Audio implementation of support for a host API.
*/


#include <assert.h>
#include <emscripten/webaudio.h>
#include <string.h> /* strlen() */
#include <stdint.h>

#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_debugprint.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"


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

static void WasmAudioWorkletThreadInitialized( EMSCRIPTEN_WEBAUDIO_T context,
                                               EM_BOOL success,
                                               void *userData );
static void WasmAudioWorkletProcessorCreated( EMSCRIPTEN_WEBAUDIO_T context,
                                              EM_BOOL success,
                                              void *userData );
static EM_BOOL WebAudioHostProcessingLoop( int numInputs, const AudioSampleFrame *inputBuffer,
                                           int numOutputs, AudioSampleFrame *outputBuffer,
                                           int numParams, const AudioParamFrame *params,
                                           void *userData );

/* IMPLEMENT ME: a macro like the following one should be used for reporting
 host errors */
#define PA_WEBAUDIO_SET_LAST_HOST_ERROR( errorCode, errorText ) \
    PaUtil_SetLastHostErrorInfo( paInDevelopment, errorCode, errorText )

/* PaWebAudioHostApiRepresentation - host api datastructure specific to this implementation */

typedef struct
{
    PaUtilHostApiRepresentation inheritedHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilStreamInterface blockingStreamInterface;

    PaUtilAllocationGroup *allocations;
}
PaWebAudioHostApiRepresentation;


PaError PaWebAudio_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    int i, deviceCount;
    PaWebAudioHostApiRepresentation *webAudioHostApi;
    PaDeviceInfo *deviceInfoArray;

    webAudioHostApi = (PaWebAudioHostApiRepresentation*)PaUtil_AllocateZeroInitializedMemory( sizeof(PaWebAudioHostApiRepresentation) );
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
    (*hostApi)->info.name = "Web Audio";

    (*hostApi)->info.defaultInputDevice = paNoDevice;  /* IMPLEMENT ME */
    (*hostApi)->info.defaultOutputDevice = 0;

    (*hostApi)->info.deviceCount = 0;

    // TODO: Add proper support for multiple devices
    // https://developer.chrome.com/blog/audiocontext-setsinkid
    // https://developer.mozilla.org/en-US/docs/Web/API/AudioContext/setSinkId

    int defaultSampleRate = 44100;

    deviceCount = 1; /* IMPLEMENT ME */

    if( deviceCount > 0 )
    {
        (*hostApi)->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateZeroInitializedMemory(
                webAudioHostApi->allocations, sizeof(PaDeviceInfo*) * deviceCount );
        if( !(*hostApi)->deviceInfos )
        {
            result = paInsufficientMemory;
            goto error;
        }

        /* allocate all device info structs in a contiguous block */
        deviceInfoArray = (PaDeviceInfo*)PaUtil_GroupAllocateZeroInitializedMemory(
                webAudioHostApi->allocations, sizeof(PaDeviceInfo) * deviceCount );
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
            deviceInfo->name = "Default"; /* IMPLEMENT ME: allocate block and copy name eg:
                deviceName = (char*)PaUtil_GroupAllocateZeroInitializedMemory( webAudioHostApi->allocations, strlen(srcName) + 1 );
                if( !deviceName )
                {
                    result = paInsufficientMemory;
                    goto error;
                }
                strcpy( deviceName, srcName );
                deviceInfo->name = deviceName;
            */

            deviceInfo->maxInputChannels = 0;  /* IMPLEMENT ME */
            deviceInfo->maxOutputChannels = 2;  /* IMPLEMENT ME */

            deviceInfo->defaultLowInputLatency = 0.;  /* IMPLEMENT ME */
            deviceInfo->defaultLowOutputLatency = 0.;  /* IMPLEMENT ME */
            deviceInfo->defaultHighInputLatency = 0.;  /* IMPLEMENT ME */
            deviceInfo->defaultHighOutputLatency = 0.;  /* IMPLEMENT ME */

            deviceInfo->defaultSampleRate = defaultSampleRate;

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

    /*
        IMPLEMENT ME:
            - clean up any resources not handled by the allocation group
    */

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


    /* suppress unused variable warnings */
    (void) sampleRate;

    return paFormatIsSupported;
}

/* PaWebAudioStream - a stream data structure specifically for this implementation */

typedef struct PaWebAudioStream
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

    EMSCRIPTEN_WEBAUDIO_T context;

    unsigned long framesPerHostCallback; /* just an example */
}
PaWebAudioStream;

/* Must be a multiple of (and aligned to) 16 bytes. See
    - https://emscripten.org/docs/api_reference/wasm_audio_worklets.html#programming-example
    - https://github.com/emscripten-core/emscripten/blob/2ba2078b/system/include/emscripten/webaudio.h#L70 */
uint8_t WASM_AUDIO_WORKLET_THREAD_STACK[4096];

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
    unsigned long framesPerHostBuffer = 128; // This buffer size is fixed for AudioWorkletProcessor
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
            PaUtil_SelectClosestAvailableFormat( paInt16 /* native formats */, inputSampleFormat );
    }
    else
    {
        inputChannelCount = 0;
        inputSampleFormat = hostInputSampleFormat = paInt16; /* Suppress 'uninitialised var' warnings. */
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

        /* IMPLEMENT ME - establish which  host formats are available */
        hostOutputSampleFormat =
            PaUtil_SelectClosestAvailableFormat( paInt16 /* native formats */, outputSampleFormat );
    }
    else
    {
        outputChannelCount = 0;
        outputSampleFormat = hostOutputSampleFormat = paInt16; /* Suppress 'uninitialized var' warnings. */
    }

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


    stream = (PaWebAudioStream*)PaUtil_AllocateZeroInitializedMemory( sizeof(PaWebAudioStream) );
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


    /* additional stream setup + opening */

    PA_DEBUG(("Creating audio context...\n"));
    stream->context = emscripten_create_audio_context(0);

    PA_DEBUG(("Starting Wasm Audio Worklet thread...\n"));
    emscripten_start_wasm_audio_worklet_thread_async(
            stream->context,
            WASM_AUDIO_WORKLET_THREAD_STACK,
            sizeof(WASM_AUDIO_WORKLET_THREAD_STACK),
            &WasmAudioWorkletThreadInitialized,
            stream);

    stream->framesPerHostCallback = framesPerHostBuffer;

    *s = (PaStream*)stream;

    return result;

error:
    if( stream )
        PaUtil_FreeMemory( stream );

    return result;
}

static void WasmAudioWorkletThreadInitialized( EMSCRIPTEN_WEBAUDIO_T context,
                                               EM_BOOL success,
                                               void *userData )
{
    if (!success) return; // Check browser console for detailed errors

    WebAudioWorkletProcessorCreateOptions opts = {
            .name = "portaudio-stream",
    };

    PA_DEBUG(("Creating Wasm Audio Worklet processor...\n"));
    emscripten_create_wasm_audio_worklet_processor_async(
            context, &opts, &WasmAudioWorkletProcessorCreated, userData);
}

static void WasmAudioWorkletProcessorCreated( EMSCRIPTEN_WEBAUDIO_T context,
                                              EM_BOOL success,
                                              void *userData )
{
    if (!success) return; // Check browser console for detailed errors

    int outputChannelCounts[1] = { 2 };
    EmscriptenAudioWorkletNodeCreateOptions opts = {
            .numberOfInputs = 0,
            .numberOfOutputs = 1,
    };

    PA_DEBUG(("Creating Wasm Audio Worklet node...\n"));
    EMSCRIPTEN_AUDIO_WORKLET_NODE_T node = emscripten_create_wasm_audio_worklet_node(
            context, "portaudio-stream", &opts, &WebAudioHostProcessingLoop, userData);

    PA_DEBUG(("Connecting node to audio context destination...\n"));
    EM_ASM({
        const node = emscriptenGetAudioObject($0);
        const context = emscriptenGetAudioObject($1);
        node.connect(context.destination);
    }, node, context);
}

static EM_BOOL WebAudioHostProcessingLoop( int numInputs, const AudioSampleFrame *inputs,
                                           int numOutputs, AudioSampleFrame *outputs,
                                           int numParams, const AudioParamFrame *params,
                                           void *userData )
{
    PaWebAudioStream *stream = (PaWebAudioStream*)userData;
    PaStreamCallbackTimeInfo timeInfo = {0,0,0}; /* IMPLEMENT ME */
    int callbackResult;
    unsigned long framesProcessed;

    PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );

    /*
        IMPLEMENT ME:
            - generate timing information
            - handle buffer slips
    */

    /*
        If you need to byte swap or shift inputBuffer to convert it into a
        portaudio format, do it here.
    */



    PaUtil_BeginBufferProcessing( &stream->bufferProcessor, &timeInfo, 0 /* IMPLEMENT ME: pass underflow/overflow flags when necessary */ );

    /*
        depending on whether the host buffers are interleaved, non-interleaved
        or a mixture, you will want to call PaUtil_SetInterleaved*Channels(),
        PaUtil_SetNonInterleaved*Channel() or PaUtil_Set*Channel() here.
    */

    if (numInputs > 0) {
        assert(numInputs == 1);
        PaUtil_SetInputFrameCount( &stream->bufferProcessor, 0 /* default to host buffer size */ );
        PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor,
                0, /* first channel of inputBuffer is channel 0 */
                inputs[0].data,
                inputs[0].numberOfChannels );
    }

    if (numOutputs > 0) {
        assert(numOutputs == 1);
        PaUtil_SetOutputFrameCount( &stream->bufferProcessor, 0 /* default to host buffer size */ );
        PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor,
                0, /* first channel of outputBuffer is channel 0 */
                outputs[0].data,
                outputs[0].numberOfChannels );
    }

    /* you must pass a valid value of callback result to PaUtil_EndBufferProcessing()
        in general you would pass paContinue for normal operation, and
        paComplete to drain the buffer processor's internal output buffer.
        You can check whether the buffer processor's output buffer is empty
        using PaUtil_IsBufferProcessorOuputEmpty( bufferProcessor )
    */
    callbackResult = paContinue;
    framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor, &callbackResult );


    /*
        If you need to byte swap or shift outputBuffer to convert it to
        host format, do it here.
    */

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

    return callbackResult == paContinue;
}


/*
    When CloseStream() is called, the multi-api layer ensures that
    the stream has already been stopped or aborted.
*/
static PaError CloseStream( PaStream* s )
{
    PaError result = paNoError;
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    /* additional stream closing + cleanup */

    EM_ASM({
        const context = emscriptenGetAudioObject($0);
        context.close();
    }, stream->context);

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );
    PaUtil_FreeMemory( stream );

    return result;
}


static PaError StartStream( PaStream *s )
{
    PaError result = paNoError;
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );

    /* TODO: Check if this is right, see portaudio.h for required behavior */

    PA_DEBUG(("Resuming audio context...\n"));
    emscripten_resume_audio_context_sync(stream->context);

    /*
        Resuming the context is only allowed after the user has interacted with
        the page e.g. by clicking somewhere. To make sure that the context is
        actually running, we use a sleep-wait loop that periodically reattempts
        to resume the context.

        TODO: Find a more elegant solution
    */

    while (!IsStreamActive(stream)) {
        PA_DEBUG(("Audio context is not running yet, waiting for user interaction...\n"));
        emscripten_sleep(500);
        emscripten_resume_audio_context_sync(stream->context);
    }

    return result;
}


static PaError StopStream( PaStream *s )
{
    PaError result = paNoError;
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    /* TODO: Check if this is right, see portaudio.h for required behavior */

    PA_DEBUG(("Suspending audio context upon stop...\n"));
    EM_ASM({
        const context = emscriptenGetAudioObject($0);
        Asyncify.handleAsync(async () => {
            await context.suspend();
        });
    }, stream->context);

    return result;
}


static PaError AbortStream( PaStream *s )
{
    PaError result = paNoError;
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    /* TODO: Check if this is right, see portaudio.h for required behavior */

    PA_DEBUG(("Suspending audio context upon abort...\n"));
    EM_ASM({
        const context = emscriptenGetAudioObject($0);
        Asyncify.handleAsync(async () => {
            await context.suspend();
        });
    }, stream->context);

    return result;
}


static PaError IsStreamStopped( PaStream *s )
{
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    /* TODO: Check if this is right, see portaudio.h for required behavior */

    return emscripten_audio_context_state(stream->context) != AUDIO_CONTEXT_STATE_RUNNING;
}


static PaError IsStreamActive( PaStream *s )
{
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    /* TODO: Check if this is right, see portaudio.h for required behavior */

    return emscripten_audio_context_state(stream->context) == AUDIO_CONTEXT_STATE_RUNNING;
}


static PaTime GetStreamTime( PaStream *s )
{
    PaWebAudioStream *stream = (PaWebAudioStream*)s;

    /* suppress unused variable warnings */
    (void) stream;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
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
