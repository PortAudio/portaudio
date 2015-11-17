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
        PaDeviceInfo deviceInfoArray[1024];
        char *pulseaudioDeviceNames[1024];

        /* PulseAudio stuff goes here */
        pa_threaded_mainloop *mainloop;
        pa_context *context;
        int deviceCount;
        pa_context_state_t state;
        pa_time_event *timeEvent;
    }
    PaPulseAudioHostApiRepresentation;

/* PaPulseAudioStream - a stream data structure specifically for this implementation */

    typedef struct PaPulseAudioStream
    {
        PaUtilStreamRepresentation streamRepresentation;
        PaUtilCpuLoadMeasurer cpuLoadMeasurer;
        PaUtilBufferProcessor bufferProcessor;
        PaPulseAudioHostApiRepresentation *hostapi;

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

        int callbackMode;       /* bool: are we running in callback mode? */
        int rtSched;
        long maxFramesPerBuffer;
        long maxFramesHostPerBuffer;
        int outputFrameSize;
        int inputFrameSize;

        PaDeviceIndex device;

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
    PaPulseAudioStream;

    PaError PaPulseAudio_Initialize(
    PaUtilHostApiRepresentation ** hostApi,
    PaHostApiIndex index
    );

    static void Terminate(
    struct PaUtilHostApiRepresentation *hostApi
    );


    static PaError IsFormatSupported(
    struct PaUtilHostApiRepresentation *hostApi,
    const PaStreamParameters * inputParameters,
    const PaStreamParameters * outputParameters,
    double sampleRate
    );

    static PaError OpenStream(
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


    static PaError IsStreamStopped(
    PaStream * s
    );
    static PaError IsStreamActive(
    PaStream * stream
    );

    static PaTime GetStreamTime(
    PaStream * stream
    );
    static double GetStreamCpuLoad(
    PaStream * stream
    );

    PaPulseAudioHostApiRepresentation *PulseAudioNew(
    void
    );
    void PulseAudioFree(
    PaPulseAudioHostApiRepresentation * ptr
    );

    int PulseAudioCheckConnection(
    PaPulseAudioHostApiRepresentation * ptr
    );

    static void PulseAudioCheckContextStateCb(
    pa_context * c,
    void *userdata
    );
    void PulseAudioSinkListCb(
    pa_context * c,
    const pa_sink_info * l,
    int eol,
    void *userdata
    );
    void PulseAudioSourceListCb(
    pa_context * c,
    const pa_source_info * l,
    int eol,
    void *userdata
    );

    void PulseAudioStreamStateCb(
    pa_stream * s,
    void *userdata
    );
    void PulseAudioStreamStartedCb(
    pa_stream * s,
    void *userdata
    );
    void PulseAudioStreamUnderflowCb(
    pa_stream * s,
    void *userdata
    );

    PaError PulseAudioConvertPortaudioFormatToPulseAudio(
    PaSampleFormat portaudiosf,
    pa_sample_spec * pulseaudiosf
    );

#ifdef __cplusplus
}
#endif                          /* __cplusplus */


#endif
