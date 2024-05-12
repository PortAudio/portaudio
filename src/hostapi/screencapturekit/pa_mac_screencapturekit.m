/*
 * Portable Audio I/O Library Screen Capture Kit implementation
 * Copyright (c) 2006-2010 David Viens
 * Copyright (c) 2010-2023 Dmitry Kostjuchenko
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2019 Ross Bencina, Phil Burk
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

#include <CoreMedia/CoreMedia.h>
#include <ScreenCaptureKit/ScreenCaptureKit.h>
#include <pthread.h>
#include <pa_debugprint.h>
#include <pa_hostapi.h>
#include <pa_ringbuffer.h>
#include <pa_stream.h>
#include <pa_util.h>
#include <portaudio.h>

@interface ScreenCaptureKitStreamOutput : NSObject <SCStreamOutput>
@property(nonatomic, assign) PaUtilRingBuffer *ringBuffer;
@end

typedef struct PaScreenCaptureKitHostApiRepresentation
{
    PaUtilHostApiRepresentation inheritedHostApiRep;
    PaUtilStreamInterface blockingStreamInterface;
    PaUtilStreamInterface callbackStreamInterface;
} PaScreenCaptureKitHostApiRepresentation;

typedef struct PaScreenCaptureKitStream
{
    PaUtilStreamRepresentation streamRepresentation;
    SCStream *audioStream;
    ScreenCaptureKitStreamOutput *streamOutput;
    PaUtilRingBuffer ringBuffer;
    BOOL isStopped;
    int sampleRate;
    PaStreamCallback *streamCallback;
    void *userData;
    unsigned long framesPerBuffer;
    pthread_t callbackThreadId;
} PaScreenCaptureKitStream;

@implementation ScreenCaptureKitStreamOutput

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type
{
    if (type == SCStreamOutputTypeAudio)
    {
        // Get the audio buffer list from the sample buffer
        AudioBufferList *bufferList = NULL;
        size_t bufferListSize = 0;
        OSStatus status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sampleBuffer, &bufferListSize, NULL,
                                                                                  0, NULL, NULL, 0, NULL);

        if (status != noErr)
        {
            PA_DEBUG(("Failed to get audio buffer list size\n"));
            goto exit;
        }

        bufferList = (AudioBufferList *)PaUtil_AllocateZeroInitializedMemory(bufferListSize);
        if (bufferList == NULL)
        {
            PA_DEBUG(("Failed to allocate memory for audio buffer list\n"));
            goto exit;
        }

        CMBlockBufferRef blockBuffer = NULL;

        status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sampleBuffer, NULL, bufferList, bufferListSize,
                                                                         NULL, NULL, 0, &blockBuffer);

        if (status != noErr)
        {
            PA_DEBUG(("Failed to get audio buffer list status=%d\n", status));
            goto exit;
        }
        // Get the format description from the sample buffer
        CMFormatDescriptionRef formatDescription = CMSampleBufferGetFormatDescription(sampleBuffer);
        const AudioStreamBasicDescription *asbd = CMAudioFormatDescriptionGetStreamBasicDescription(formatDescription);
        if (asbd == NULL)
        {
            PA_DEBUG(("Failed to get audio stream basic description\n"));
            goto exit;
        }

        // Check the sample format
        if (asbd->mFormatID == kAudioFormatLinearPCM && (asbd->mFormatFlags & kAudioFormatFlagIsFloat))
        {
            // Samples are in float format
            for (int i = 0; i < bufferList->mNumberBuffers; i++)
            {
                PaUtil_WriteRingBuffer(self.ringBuffer, bufferList->mBuffers[i].mData,
                                       bufferList->mBuffers[i].mDataByteSize / asbd->mBytesPerFrame);
            }
        }
        else
        {
            PA_DEBUG(("Unsupported audio format\n"));
        }

    exit:
        if (blockBuffer)
            CFRelease(blockBuffer);

        if (bufferList)
            PaUtil_FreeMemory(bufferList);

        return;
    }
}
@end

static PaError StopStreamInternal(PaStream *s)
{
    PaScreenCaptureKitStream *stream = (PaScreenCaptureKitStream *)s;
    __block PaError result = paNoError;
    dispatch_group_t handlerGroup = dispatch_group_create();
    dispatch_group_enter(handlerGroup);
    // Stop the audio capture session
    [stream->audioStream stopCaptureWithCompletionHandler:^(NSError *error) {
      if (error)
      {
          PA_DEBUG(("Failed to stop audio capture: %s\n", [[error localizedDescription] UTF8String]));
          result = paInternalError;
          return;
      }
      dispatch_group_leave(handlerGroup);
    }];
    stream->isStopped = TRUE;
    dispatch_group_wait(handlerGroup, DISPATCH_TIME_FOREVER);
    return result;
}

// PortAudio host API stream read function
static PaError ReadStream(PaStream *s, void *buffer, unsigned long frames)
{
    PaScreenCaptureKitStream *stream = (PaScreenCaptureKitStream *)s;
    // TODO : Need to implement exit on StopStream, Closestream or similar
    while (PaUtil_GetRingBufferReadAvailable(&stream->ringBuffer) < frames)
    {
        // Sleep for 10ms
        usleep(10000);
    }

    ring_buffer_size_t read = PaUtil_ReadRingBuffer(&stream->ringBuffer, buffer, frames);

    return paNoError;
}

static void *StreamProcessingThread(void *userData)
{
    PaScreenCaptureKitStream *stream = (PaScreenCaptureKitStream *)userData;
    while (!stream->isStopped)
    {
        // Wait until enough data is available in the ring buffer
        if (PaUtil_GetRingBufferReadAvailable(&stream->ringBuffer) >= stream->framesPerBuffer)
        {
            float buffer[stream->framesPerBuffer];
            // Copy the data to the buffer
            ring_buffer_size_t read = PaUtil_ReadRingBuffer(&stream->ringBuffer, buffer, stream->framesPerBuffer);

            const PaStreamCallbackTimeInfo timeInfo = {0, 0, 0}; // TODO : Fill the timestamps
            PaStreamCallbackFlags statusFlags = 0; // TODO : Determine underflow/overflow flags as needed

            // Call the user callback
            PaStreamCallbackResult callbackResult = stream->streamCallback(buffer, NULL,
                stream->framesPerBuffer, &timeInfo, statusFlags, stream->userData);

            if (callbackResult != paContinue)
            {
                StopStreamInternal((PaStream *)stream);
            }
        }
        usleep(1000);
    }
    return NULL;
}

// PortAudio host API is format supported function
static PaError IsFormatSupported(struct PaUtilHostApiRepresentation *hostApi, const PaStreamParameters *inputParameters,
                                 const PaStreamParameters *outputParameters, double sampleRate)
{
    if (inputParameters == NULL)
    {
        return paInvalidDevice;
    }
    // Check if the input parameters are supported
    if (inputParameters != NULL)
    {
        // Check if the number of channels is supported
        if (inputParameters->channelCount > hostApi->deviceInfos[0]->maxInputChannels)
        {
            return paInvalidChannelCount;
        }

        // Check if the sample format is supported
        if (inputParameters->sampleFormat != paFloat32)
        {
            return paSampleFormatNotSupported;
        }
    }

    // Check if the output parameters are supported
    if (outputParameters != NULL)
    {
        return paInvalidDevice;
    }

    if (sampleRate == 0)
    {
        return paInvalidSampleRate;
    }

    return paFormatIsSupported;
}

// PortAudio host API stream open function
static PaError OpenStream(struct PaUtilHostApiRepresentation *hostApi, PaStream **s,
                          const PaStreamParameters *inputParameters, const PaStreamParameters *outputParameters,
                          double sampleRate, unsigned long framesPerBuffer, PaStreamFlags streamFlags,
                          PaStreamCallback *streamCallback, void *userData)
{
    PaError result = IsFormatSupported(hostApi, inputParameters, outputParameters, sampleRate);
    if (result != paFormatIsSupported)
        return result;
    PaScreenCaptureKitHostApiRepresentation *paSck = (PaScreenCaptureKitHostApiRepresentation *)hostApi;
    PaScreenCaptureKitStream *stream = NULL;
    result = paNoError;
    __block NSArray<SCDisplay *> *displays = nil;
    if ((stream = (PaScreenCaptureKitStream *)PaUtil_AllocateZeroInitializedMemory(sizeof(PaScreenCaptureKitStream))) ==
        NULL)
    {
        result = paInsufficientMemory;
        goto error;
    }

    dispatch_group_t handlerGroup = dispatch_group_create();

    dispatch_group_enter(handlerGroup);

    [SCShareableContent
        getShareableContentWithCompletionHandler:^(SCShareableContent *shareableContent, NSError *error) {
          if (error)
          {
              // Handle the error
              PA_DEBUG(("Failed to retrieve shareable content: %s\n", [error.localizedDescription UTF8String]));
          }
          else
          {
              // Store the array of available displays in the global variable
              displays = [shareableContent.displays retain];
          }

          // Leave the dispatch group
          dispatch_group_leave(handlerGroup);
        }];

    dispatch_group_wait(handlerGroup, DISPATCH_TIME_FOREVER);

    // Check the size of displays array
    if (!displays || displays.count < 1)
    {
        PA_DEBUG(("No displays found\n"));
        result = paInternalError;
        goto error;
    }

    SCDisplay *firstDisplay = displays[0];

    NSArray<SCWindow *> *exclude_windows = nil;
    SCContentFilter *contentFilter = [[SCContentFilter alloc] initWithDisplay:firstDisplay
                                                             excludingWindows:exclude_windows];
    if (!contentFilter)
    {
        PA_DEBUG(("Failed to create content filter\n"));
        result = paInternalError;
        goto error;
    }
    // Create a screen capture configuration
    SCStreamConfiguration *streamConfig = [[SCStreamConfiguration alloc] init];
    if (!streamConfig)
    {
        PA_DEBUG(("Failed to create stream configuration\n"));
        result = paInternalError;
        goto error;
    }
    streamConfig.capturesAudio = YES;
    streamConfig.sampleRate = stream->sampleRate = sampleRate;
    streamConfig.channelCount = inputParameters->channelCount;

    // Create an audio capture session
    stream->audioStream = [[SCStream alloc] initWithFilter:contentFilter configuration:streamConfig delegate:nil];
    if (!stream->audioStream)
    {
        PA_DEBUG(("Failed to create audio stream\n"));
        result = paInternalError;
        goto error;
    }

    stream->streamOutput = [[ScreenCaptureKitStreamOutput alloc] init];
    if (!stream->streamOutput)
    {
        PA_DEBUG(("Failed to create stream output\n"));
        result = paInternalError;
        goto error;
    }
    NSError *error = nil;

    BOOL success = [stream->audioStream addStreamOutput:stream->streamOutput
                                                   type:SCStreamOutputTypeAudio
                                     sampleHandlerQueue:nil
                                                  error:&error];
    if (!success || error)
    {
        PA_DEBUG(("Failed to add stream output: %s\n", [[error localizedDescription] UTF8String]));
        result = paInternalError;
        goto error;
    }

    success = [stream->audioStream addStreamOutput:stream->streamOutput
                                              type:SCStreamOutputTypeScreen
                                sampleHandlerQueue:nil
                                             error:&error];
    if (!success || error)
    {
        PA_DEBUG(("Failed to add stream output: %s\n", [[error localizedDescription] UTF8String]));
        result = paInternalError;
        goto error;
    }

    if (streamCallback)
    {
        PaUtil_InitializeStreamRepresentation(&stream->streamRepresentation, &paSck->callbackStreamInterface,
                                              streamCallback, userData);
        stream->streamCallback = streamCallback;
        stream->userData = userData;
        stream->framesPerBuffer = framesPerBuffer;
    }
    else
    {
        PaUtil_InitializeStreamRepresentation(&stream->streamRepresentation, &paSck->blockingStreamInterface, NULL,
                                              NULL);
    }
    stream->isStopped = TRUE;

    // Around 1 second of ringbuffer (allocated to nearest power of 2)
    int audioBufferSize = 1 << (int)ceil(log2(sampleRate));
    void *data = PaUtil_AllocateZeroInitializedMemory(audioBufferSize * sizeof(float));
    if (!data)
    {
        PA_DEBUG(("Failed to allocate memory for ringbuffer"));
        result = paInternalError;
        goto error;
    }
    PaUtil_InitializeRingBuffer(&stream->ringBuffer, sizeof(float), audioBufferSize, data);
    stream->streamOutput.ringBuffer = &stream->ringBuffer;

    [displays release];
    *s = (PaStream *)stream;
    return paNoError;
error:
    if (stream)
    {
        PaUtil_FreeMemory(stream);
    }
    if (displays)
        [displays release];
    return result;
}

static PaError IsStreamStopped(PaStream *s)
{
    return ((PaScreenCaptureKitStream *)s)->isStopped;
}

static PaError IsStreamActive(PaStream *s)
{
    return !IsStreamStopped(s);
}

static PaError StartStream(PaStream *s)
{
    if (IsStreamActive(s))
        return paStreamIsNotStopped;
    __block PaError result = paNoError;
    PaScreenCaptureKitStream *stream = (PaScreenCaptureKitStream *)s;

    dispatch_group_t handlerGroup = dispatch_group_create();
    dispatch_group_enter(handlerGroup);

    // Start the audio capture session
    [stream->audioStream startCaptureWithCompletionHandler:^(NSError *error) {
      if (error)
      {
          PA_DEBUG(("Failed to start audio capture: %s\n", [[error localizedDescription] UTF8String]));
          result = paInternalError;
      }
      dispatch_group_leave(handlerGroup);
    }];
    dispatch_group_wait(handlerGroup, DISPATCH_TIME_FOREVER);
    if (result == paNoError)
    {
        stream->isStopped = FALSE;
    }
    if (stream->streamCallback != NULL)
    {
        int result = pthread_create(&stream->callbackThreadId, NULL, StreamProcessingThread, stream);
        if (result != 0) {
            PA_DEBUG(("Failed to create audio processing thread\n"));
            return paUnanticipatedHostError;
        }
    }

    return result;
}

// PortAudio host API stream stop function
static PaError StopStream(PaStream *s)
{
    if (IsStreamStopped(s))
        return paStreamIsStopped;
    PaScreenCaptureKitStream *stream = (PaScreenCaptureKitStream *)s;
    PaError result = StopStreamInternal(s);
    pthread_join(stream->callbackThreadId, NULL);

    return result;
}

// PortAudio host API stream abort function
static PaError AbortStream(PaStream *s)
{
    return StopStream(s);
}

// PortAudio host API stream close function
static PaError CloseStream(PaStream *s)
{
    StopStream(s);
    PaScreenCaptureKitStream *stream = (PaScreenCaptureKitStream *)s;
    PaUtil_FreeMemory(stream->ringBuffer.buffer);
    PaUtil_FreeMemory(stream);
    return paNoError;
}

static void Terminate(struct PaUtilHostApiRepresentation *hostApi)
{
    // Free the host API representation
    if (hostApi != NULL)
    {
        if (hostApi->deviceInfos[0] != NULL)
            PaUtil_FreeMemory(hostApi->deviceInfos[0]);
        if (hostApi->deviceInfos != NULL)
            PaUtil_FreeMemory(hostApi->deviceInfos);
        PaUtil_FreeMemory(hostApi);
    }

    return;
}

static PaTime GetStreamTime(PaStream *s)
{
    /* suppress unused variable warnings */
    (void)s;

    return PaUtil_GetTime();
}

PaError PaMacScreenCapture_Initialize(PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex)
{
    PaError result = paNoError;
    PaScreenCaptureKitHostApiRepresentation *paSck = NULL;

    paSck = (PaScreenCaptureKitHostApiRepresentation *)PaUtil_AllocateZeroInitializedMemory(
        sizeof(PaScreenCaptureKitHostApiRepresentation));
    if (paSck == NULL)
    {
        result = paInsufficientMemory;
        goto error;
    }

    *hostApi = &paSck->inheritedHostApiRep;
    // Set the host API function pointers
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paScreenCaptureKit;
    (*hostApi)->info.name = "Mac ScreenCaptureKit";
    (*hostApi)->info.deviceCount = 1;
    (*hostApi)->info.defaultInputDevice = 0;
    (*hostApi)->info.defaultOutputDevice = paNoDevice;
    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    (*hostApi)->deviceInfos = PaUtil_AllocateZeroInitializedMemory(sizeof(PaDeviceInfo *));
    if ((*hostApi)->deviceInfos == NULL)
    {
        result = paInsufficientMemory;
        goto error;
    }

    (*hostApi)->deviceInfos[0] = PaUtil_AllocateZeroInitializedMemory(sizeof(PaDeviceInfo));
    if ((*hostApi)->deviceInfos[0] == NULL)
    {
        result = paInsufficientMemory;
        goto error;
    }

    (*hostApi)->deviceInfos[0]->structVersion = 2;
    (*hostApi)->deviceInfos[0]->hostApi = hostApiIndex;
    (*hostApi)->deviceInfos[0]->name = "System Audio [Loopback]";
    (*hostApi)->deviceInfos[0]->maxInputChannels = 1;
    (*hostApi)->deviceInfos[0]->defaultSampleRate = 44100;

    PaUtil_InitializeStreamInterface(&paSck->blockingStreamInterface, CloseStream, StartStream, StopStream, AbortStream,
                                     IsStreamStopped, IsStreamActive, GetStreamTime, PaUtil_DummyGetCpuLoad, ReadStream,
                                     PaUtil_DummyWrite, PaUtil_DummyGetReadAvailable, PaUtil_DummyGetWriteAvailable);

    PaUtil_InitializeStreamInterface(&paSck->callbackStreamInterface, CloseStream, StartStream, StopStream, AbortStream,
                                     IsStreamStopped, IsStreamActive, GetStreamTime, PaUtil_DummyGetCpuLoad, PaUtil_DummyRead,
                                     PaUtil_DummyWrite, PaUtil_DummyGetReadAvailable, PaUtil_DummyGetWriteAvailable);
    return result;

error:
    if (paSck != NULL)
        Terminate((PaUtilHostApiRepresentation *)paSck);

    return result;
}