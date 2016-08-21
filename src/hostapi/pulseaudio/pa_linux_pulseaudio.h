
/*
 * PulseAudio host to play natively in Linux based systems without
 * ALSA emulation
 *
 * Copyright (c) 2014-2016 Tuukka Pasanen <tuukka.pasanen@ilmi.fi>
 * Copyright (c) 2016 Sqweek <sqweek@gmail.com>
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

#ifndef _PA_HOSTAPI_PULSEAUDIO_H_
#define _PA_HOSTAPI_PULSEAUDIO_H_

#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

#include "pa_unix_util.h"
#include "pa_ringbuffer.h"
#include "pa_debugprint.h"

/* PulseAudio headers */
#include <stdio.h>
#include <string.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>



#ifdef __cplusplus
extern "C"
{
#endif                          /* __cplusplus */

/* prototypes for functions declared in this file */

#define PA_PULSEAUDIO_SET_LAST_HOST_ERROR(errorCode, errorText) \
    PaUtil_SetLastHostErrorInfo(paInDevelopment, errorCode, errorText)

/* Just chosen by hand from mistake and success method. Nothing really groudbreaking
   If there is better number with better explantion then I'll be glad to change this
   @todo change this to something more sophisticated */
#define PULSEAUDIO_TIME_EVENT_USEC 50000

#define PAPULSEAUDIO_MAX_DEVICECOUNT 1024
#define PAPULSEAUDIO_MAX_DEVICENAME 1024

/* Assuming of 2 seconds of 44100 Hz sample rate with FLOAT (4 bytes) and stereo channels (2 channels).
   You should have pretty good size buffer with this. If output/intput doesn't happens in 2 second we
   have more trouble than this buffer.
   @todo change this to something more sophisticated */
#define PULSEAUDIO_BUFFER_SIZE (88100 * 4 * 2)

    typedef struct
    {
        PaUtilHostApiRepresentation inheritedHostApiRep;
        PaUtilStreamInterface callbackStreamInterface;
        PaUtilStreamInterface blockingStreamInterface;

        PaUtilAllocationGroup *allocations;

        PaHostApiIndex hostApiIndex;
        PaDeviceInfo deviceInfoArray[PAPULSEAUDIO_MAX_DEVICECOUNT];
        char *pulseaudioDeviceNames[PAPULSEAUDIO_MAX_DEVICECOUNT];
        char pulseaudioDefaultSource[PAPULSEAUDIO_MAX_DEVICENAME + 1];
        char pulseaudioDefaultSink[PAPULSEAUDIO_MAX_DEVICENAME + 1];

        /* PulseAudio stuff goes here */
        pa_threaded_mainloop *mainloop;
        pa_context *context;
        int deviceCount;
        pa_context_state_t state;
        pa_time_event *timeEvent;
    }
    PaPulseAudio_HostApiRepresentation;

/* PaPulseAudio_Stream - a stream data structure specifically for this implementation */

    typedef struct PaPulseAudio_Stream
    {
        PaUtilStreamRepresentation streamRepresentation;
        PaUtilCpuLoadMeasurer cpuLoadMeasurer;
        PaUtilBufferProcessor bufferProcessor;
        PaPulseAudio_HostApiRepresentation *hostapi;

        PaUnixThread thread;
        unsigned long framesPerHostCallback;
        pa_threaded_mainloop *mainloop;
        pa_simple *simple;
        pa_context *context;
        pa_sample_spec outSampleSpec;
        pa_sample_spec inSampleSpec;
        pa_stream *outStream;
        pa_stream *inStream;
        size_t writableSize;
        pa_usec_t outStreamTime;
        pa_buffer_attr bufferAttr;
        int underflows;
        int latency;
        int outputChannelCount;

        int callbackMode;       /* bool: are we running in callback mode? */
        int rtSched;
        long maxFramesPerBuffer;
        long maxFramesHostPerBuffer;
        int outputFrameSize;
        int inputFrameSize;

        PaDeviceIndex inDevice;
        PaDeviceIndex outDevice;

        void *outBuffer;
        void *inBuffer;

        PaUtilRingBuffer inputRing;
        PaUtilRingBuffer outputRing;

        /* Used in communication between threads */
        volatile sig_atomic_t callback_finished;        /* bool: are we in the "callback finished" state? */
        volatile sig_atomic_t callbackAbort;    /* Drop frames? */
        volatile sig_atomic_t isActive; /* Is stream in active state? (Between StartStream and StopStream || !paContinue) */
        volatile sig_atomic_t isStopped;        /* Is stream in active state? (Between StartStream and StopStream || !paContinue) */

    }
    PaPulseAudio_Stream;

    PaError PaPulseAudio_Initialize(
    PaUtilHostApiRepresentation ** hostApi,
    PaHostApiIndex index
    );

    void Terminate(
    struct PaUtilHostApiRepresentation *hostApi
    );


    PaError IsFormatSupported(
    struct PaUtilHostApiRepresentation *hostApi,
    const PaStreamParameters * inputParameters,
    const PaStreamParameters * outputParameters,
    double sampleRate
    );

    PaError OpenStream(
    struct PaUtilHostApiRepresentation *hostApi,
    PaStream ** s,
    const PaStreamParameters * inputParameters,
    const PaStreamParameters * outputParameters,
    double sampleRate,
    unsigned long framesPerBuffer,
    PaStreamFlags streamFlags,
    PaStreamCallback * streamCallback,
    void *userData
    );


    PaError IsStreamStopped(
    PaStream * s
    );
    PaError IsStreamActive(
    PaStream * stream
    );

    PaTime GetStreamTime(
    PaStream * stream
    );
    double GetStreamCpuLoad(
    PaStream * stream
    );

    PaPulseAudio_HostApiRepresentation *PaPulseAudio_New(
    void
    );
    void PaPulseAudio_Free(
    PaPulseAudio_HostApiRepresentation * ptr
    );

    int PaPulseAudio_CheckConnection(
    PaPulseAudio_HostApiRepresentation * ptr
    );

    void PaPulseAudio_CheckContextStateCb(
    pa_context * c,
    void *userdata
    );
    void PaPulseAudio_ServerInfoCb(
        pa_context *c,
        const pa_server_info *i,
        void *userdata
    );
    void PaPulseAudio_SinkListCb(
    pa_context * c,
    const pa_sink_info * l,
    int eol,
    void *userdata
    );
    void PaPulseAudio_SourceListCb(
    pa_context * c,
    const pa_source_info * l,
    int eol,
    void *userdata
    );

    void PaPulseAudio_StreamStateCb(
    pa_stream * s,
    void *userdata
    );
    void PaPulseAudio_StreamStartedCb(
    pa_stream * s,
    void *userdata
    );
    void PaPulseAudio_StreamUnderflowCb(
    pa_stream * s,
    void *userdata
    );

    PaError PaPulseAudio_ConvertPortaudioFormatToPaPulseAudio_(
    PaSampleFormat portaudiosf,
    pa_sample_spec * pulseaudiosf
    );

#ifdef __cplusplus
}
#endif                          /* __cplusplus */


#endif
