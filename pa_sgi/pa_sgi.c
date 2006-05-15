/*
 * $Id$
 * PortAudio Portable Real-Time Audio Library. 
 * Latest Version at: http://www.portaudio.com.
 * Silicon Graphics (SGI) IRIX implementation by Pieter Suurmond.
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
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */
/** @file
 @brief SGI IRIX AL implementation (according to V19 API version 2.0).

 @note This file started as a copy of pa_skeleton.c (v 1.1.2.35 2003/09/20), it
 has nothing to do with the old V18 pa_sgi version: this implementation uses the
 newer IRIX AL calls and uses pthreads instead of sproc.

 On IRIX, one may type './configure' followed by 'gmake' from the portaudio root 
 directory to build the static and shared libraries, as well as all the tests.

 On IRIX 6.5, using 'make' instead of 'gmake' may cause Makefile to fail. (This 
 happens on my machine: make does not understand syntax with 2 colons on a line,
 like this:
               $(TESTS): bin/%: [snip]

 Maybe this is due to an old make version(?), my only solution is: use gmake.
 Anyway, all the tests compile well now, with GCC 3.3, as well as with MIPSpro 7.2.1.
 Tested:
        - paqa_devs              ok, but at a certain point digital i/o fails:
                                     TestAdvance: INPUT, device = 2, rate = 32000, numChannels = 1, format = 1
                                     Possibly, this is an illegal sr or number of channels for digital i/o.
        - paqa_errs              13 of the tests run ok, but 5 of them give weird results.
        + patest1                ok.
        + patest_buffer          ok.
        + patest_callbackstop    ok.
        - patest_clip            ok, but hear no difference between dithering turned OFF and ON.
        + patest_hang            ok.
        + patest_latency         ok.
        + patest_leftright       ok.
        + patest_maxsines        ok.
        + patest_many            ok.
        + patest_multi_sine      ok.
        + patest_pink            ok.
        + patest_prime           ok.
        - patest_read_record     ok, but playback stops a little earlier than 5 seconds it seems(?).
        + patest_record          ok.
        + patest_ringmix         ok.
        + patest_saw             ok.
        + patest_sine            ok.
        + patest_sine8           ok.
        - patest_sine_formats    ok, FLOAT32 + INT16 + INT18 are OK, but UINT8 IS NOT OK!
        + patest_sine_time       ok.
        + patest_start_stop      ok, but under/overflow errors of course in the AL queue monitor.
        + patest_stop            ok.
        - patest_sync            ok?
        + patest_toomanysines    ok.
        - patest_underflow       ok? (stopping after SleepTime = 91: err=Stream is stopped)
        - patest_wire            ok.
        + patest_write_sine      ok.
        + pa_devs                ok.
                                 Ok on an Indy, in both stereo and quadrophonic mode.
        + pa_fuzz                ok.
        + pa_minlat              ok.

 Worked on (or checked) proposals:
 
  003:    Improve Latency Specification OK, but not 100% sure: plus or minus 1 host buffer?
  004 OK: Allow Callbacks to Accept Variable Number of Frames per Buffer. 
          Simply using a fixed host buffer size. Very roughly implemented now, the adaption
          to limited-requested latencies and samplerate may be improved. At least this
          implementation chooses its own internal host buffer size (no coredumps any longer).
  005 OK: Blocking Read/Write Interface.
  006:    Non-interleaved buffers seems OK? Covered by the buffer-processor and such?....
  009 OK: Host error reporting should now be.
  010 OK: State Machine and State Querying Functions.
  011 OK: Renaming done.
  014     Implementation Style Guidelines (sorry, my braces are not ANSI style).
  015 OK: Callback Timestamps (During priming, though, these are still null!).
  016 OK: Use Structs for Pa_OpenStream() Parameters.
  019:    Notify Client When All Buffers Have Played (Ok, covered by the buffer processor?)
  020 OK: Allow Callback to prime output stream (StartStream() should do the priming)
          Should be tested more thoroughly for full duplex streams. (patest_prime seems ok).


 @todo Underrun or overflow flags at some more places.

 @todo Callback Timestamps during priming.

 @todo Improve adaption to number of channels, samplerate and such when inventing 
       some frames per host buffer (when client requests 0).

 @todo Make a complete new version to support 'sproc'-applications.
       Or could we manage that with some clever if-defs?
       It must be clear which version we use (especially when using pa as lib!):
       an irix-sproc() version or pthread version.

 @todo In Makefile.in: 'make clean' does not remove lib/libportaudio.so.0.0.19.
    
 Note: Even when mono-output is requested, with ALv7, the audio library opens
       a outputs stereo. One can observe this in SGI's 'Audio Queue Monitor'.
*/

#include <string.h>         /* For strlen() but also for strerror()! */
#include <stdio.h>          /* printf() */
#include <math.h>           /* fabs()   */

#include <dmedia/audio.h>   /* IRIX AL (audio library). Link with -laudio. */
#include <dmedia/dmedia.h>  /* IRIX DL (digital media library), solely for */
                            /* function dmGetUST(). Link with -ldmedia.    */
#include <errno.h>          /* To catch 'oserror' after AL-calls. */
#include <pthread.h>        /* POSIX threads. */
#include <unistd.h>

#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

                            /* Uncomment for diagnostics: */
#define DBUG(x) /*{ printf x; fflush(stdout); }*/


/* prototypes for functions declared in this file */

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

PaError PaSGI_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex index );

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
                           const PaStreamParameters *ipp,
                           const PaStreamParameters *opp,
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


/* 
    Apparently, we must use macros for reporting unanticipated host errors.    
    Only in case we return paUnanticipatedHostError from an Portaudio call, 
    we have to call one of the following three macro's.
    (Constant paAL is defined in pa_common/portaudio.h. See also proposal 009.)

    After an AL error, use this to translate the AL error code to human text:
*/
#define PA_SGI_SET_LAST_AL_ERROR() \
    {\
    int ee = oserror();\
    PaUtil_SetLastHostErrorInfo(paAL, ee, alGetErrorString(ee));\
    }
/*
    But after a generic IRIX error, let strerror() translate the error code from
    the operating system and use this (strerror() gives the same as perror()):
*/
#define PA_SGI_SET_LAST_IRIX_ERROR() \
    {\
    int ee = oserror();\
    PaUtil_SetLastHostErrorInfo(paAL, ee, strerror(ee));\
    }

/* GOT RID OF calling PaUtil_SetLastHostErrorInfo() with 0 as error number.
- Weird samplerate difference became:  paInvalidSampleRate.
- Failing to set AL queue size became: paInternalError
  (Because I cannot decide between paBufferTooBig and paBufferTooSmall
   because it may even the 'default AL queue size that failed... Or 
   should we introduce another error-code like 'paInvalidQueueSize'?... NO)
*/

/* PaSGIHostApiRepresentation - host api datastructure specific to this implementation */

typedef struct
{
    PaUtilHostApiRepresentation   inheritedHostApiRep;
    PaUtilStreamInterface         callbackStreamInterface;
    PaUtilStreamInterface         blockingStreamInterface;
    PaUtilAllocationGroup*        allocations;
                                                    /* implementation specific data goes here. */
    ALvalue*                      sgiDeviceIDs;     /* Array of AL resource device numbers.    */
 /* PaHostApiIndex                hostApiIndex;        Hu? As in the linux and oss files? */
}
PaSGIHostApiRepresentation;

/*
    Initialises sgiDeviceIDs array.
*/
PaError PaSGI_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError                     result = paNoError;
    int                         e, i, deviceCount, def_in, def_out;
    PaSGIHostApiRepresentation* SGIHostApi;
    PaDeviceInfo*               deviceInfoArray;    
    static const short          numParams = 4;            /* Array with name, samplerate, channels */
    ALpv                        y[numParams];             /* and type.                             */
    static const short          maxDevNameChars = 32;     /* Including the terminating null char.  */
    char                        devName[maxDevNameChars]; /* Too lazy for dynamic alloc.           */

    /* DBUG(("PaSGI_Initialize() started.\n")); */
    SGIHostApi = (PaSGIHostApiRepresentation*)PaUtil_AllocateMemory(sizeof(PaSGIHostApiRepresentation));
    if( !SGIHostApi )
        { result = paInsufficientMemory; goto cleanup; }
    SGIHostApi->allocations = PaUtil_CreateAllocationGroup();
    if( !SGIHostApi->allocations )
        { result = paInsufficientMemory; goto cleanup; }
    *hostApi = &SGIHostApi->inheritedHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paAL;                       /* IRIX AL type id, was paInDevelopment. */
    (*hostApi)->info.name = "SGI IRIX AL";
    (*hostApi)->info.defaultInputDevice  = paNoDevice;  /* Set later. */
    (*hostApi)->info.defaultOutputDevice = paNoDevice;  /* Set later.  */
    (*hostApi)->info.deviceCount = 0;                   /* We 'll increment in the loop below. */
    
    /* Determine the total number of input and output devices (thanks to Gary Scavone). */
    deviceCount = alQueryValues(AL_SYSTEM, AL_DEVICES, 0, 0, 0, 0);
    if (deviceCount < 0)        /* Returns -1 in case of failure. */
        {
        DBUG(("Failed to count devices: alQueryValues()=%d; %s.\n",
               deviceCount, alGetErrorString(oserror())));
        result = paDeviceUnavailable;             /* Is this an appropriate error return code? */
        goto cleanup;
        }
    if (deviceCount > 0)
        {
        (*hostApi)->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
                                  SGIHostApi->allocations, sizeof(PaDeviceInfo*) * deviceCount);
        if (!(*hostApi)->deviceInfos)
            { result = paInsufficientMemory; goto cleanup; }

        /* Allocate all device info structs in a contiguous block. */
        deviceInfoArray = (PaDeviceInfo*)PaUtil_GroupAllocateMemory(
                          SGIHostApi->allocations, sizeof(PaDeviceInfo) * deviceCount);
        if (!deviceInfoArray)
            { result = paInsufficientMemory; goto cleanup; }
                                                             /* Store all AL device IDs in an array. */
        SGIHostApi->sgiDeviceIDs = (ALvalue*)PaUtil_GroupAllocateMemory(SGIHostApi->allocations,
                                                                        deviceCount * sizeof(ALvalue));
        if (!SGIHostApi->sgiDeviceIDs)
            { result = paInsufficientMemory; goto cleanup; }
        /* Same query again, but now store all IDs in array sgiDeviceIDs (still using no qualifiers).*/
        e = alQueryValues(AL_SYSTEM, AL_DEVICES, SGIHostApi->sgiDeviceIDs, deviceCount, 0, 0);
        if (e != deviceCount)
            {
            if (e < 0)                                     /* Sure an AL error really occurred. */
                { PA_SGI_SET_LAST_AL_ERROR() result = paUnanticipatedHostError; }
            else                                                 /* Seems we lost some devices. */
                { DBUG(("Number of devices suddenly changed!\n")); result = paDeviceUnavailable; }
            goto cleanup;
            }
        y[0].param = AL_DEFAULT_INPUT;
        y[1].param = AL_DEFAULT_OUTPUT;
        e = alGetParams(AL_SYSTEM, y, 2);       /* Get params global to the AL system. */
        if (e != 2)
            {
            if (e < 0)
                {
                PA_SGI_SET_LAST_AL_ERROR()         /* Calls oserror() and alGetErrorString(). */
                result = paUnanticipatedHostError; /* Sure an AL error really occurred. */
                }
            else
                {
                DBUG(("Default input and/or output could not be found!\n"));
                result = paDeviceUnavailable;   /* FIX: What if only in or out are available? */
                }
            goto cleanup;
            }
        def_in  = y[0].value.i;         /* Remember both AL devices for a while. */
        def_out = y[1].value.i;
        y[0].param     = AL_NAME;
        y[0].value.ptr = devName;
        y[0].sizeIn    = maxDevNameChars; /* Including terminating null char. */
        y[1].param     = AL_RATE;
        y[2].param     = AL_CHANNELS;
        y[3].param     = AL_TYPE;       /* Subtype of AL_INPUT_DEVICE_TYPE or AL_OUTPUT_DEVICE_TYPE? */
        for (i=0; i < deviceCount; ++i) /* Fill allocated deviceInfo structs. */
            {
            PaDeviceInfo *deviceInfo = &deviceInfoArray[i];
            deviceInfo->structVersion = 2;
            deviceInfo->hostApi = hostApiIndex; /* Retrieve name, samplerate, channels and type. */
            e = alGetParams(SGIHostApi->sgiDeviceIDs[i].i, y, numParams);
            if (e != numParams)
                {
                if (e < 0) /* Calls oserror() and alGetErrorString(). */
                    { PA_SGI_SET_LAST_AL_ERROR() result = paUnanticipatedHostError; }
                else
                    { DBUG(("alGetParams() could not get all params!\n")); result = paInternalError; }
                goto cleanup;
                }
            deviceInfo->name = (char*)PaUtil_GroupAllocateMemory(SGIHostApi->allocations, strlen(devName) + 1);
            if (!deviceInfo->name)
                { result = paInsufficientMemory; goto cleanup; }
            strcpy((char*)deviceInfo->name, devName);

            /* Determine whether the received number of channels belongs to input or output device. */
            if (alIsSubtype(AL_INPUT_DEVICE_TYPE, y[3].value.i))
                {
                deviceInfo->maxInputChannels  = y[2].value.i;
                deviceInfo->maxOutputChannels = 0;
                }
            else if (alIsSubtype(AL_OUTPUT_DEVICE_TYPE, y[3].value.i))
                {
                deviceInfo->maxInputChannels  = 0;
                deviceInfo->maxOutputChannels = y[2].value.i;
                }
            else /* Should never occur. */
                {
                DBUG(("AL device is neither input nor output!\n"));
                result = paInternalError;
                goto cleanup;
                }
            
            /* Determine if this device is the default (in or out). If so, assign. */
            if (def_in == SGIHostApi->sgiDeviceIDs[i].i)
                {
                if ((*hostApi)->info.defaultInputDevice != paNoDevice)
                    {
                    DBUG(("Default input already assigned!\n"));
                    result = paInternalError;
                    goto cleanup;
                    }
                (*hostApi)->info.defaultInputDevice = i;
                /* DBUG(("Default input assigned to pa device %d (%s).\n", i, deviceInfo->name)); */
                }
            else if (def_out == SGIHostApi->sgiDeviceIDs[i].i)
                {
                if ((*hostApi)->info.defaultOutputDevice != paNoDevice)
                    {
                    DBUG(("Default output already assigned!\n"));
                    result = paInternalError;
                    goto cleanup;
                    }
                (*hostApi)->info.defaultOutputDevice = i;
                /* DBUG(("Default output assigned to pa device %d (%s).\n", i, deviceInfo->name)); */
                }
            /*---------------------------------------------- Default latencies set to 'reasonable' values. */
            deviceInfo->defaultLowInputLatency   = 0.050; /* 50 milliseconds seems ok. */
            deviceInfo->defaultLowOutputLatency  = 0.050; /* These are ALSO ABSOLUTE MINIMA in OpenStream(). */
            deviceInfo->defaultHighInputLatency  = 0.500; /* 500 milliseconds a reasonable value? */
            deviceInfo->defaultHighOutputLatency = 0.500; /* Ten times these are ABSOLUTE MAX in OpenStream()). */

            deviceInfo->defaultSampleRate = alFixedToDouble(y[1].value.ll); /* Read current sr. */
            (*hostApi)->deviceInfos[i] = deviceInfo;
            ++(*hostApi)->info.deviceCount;
            }
        }
    /* What if (deviceCount==0)? */
    (*hostApi)->Terminate         = Terminate;
    (*hostApi)->OpenStream        = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface(&SGIHostApi->callbackStreamInterface, CloseStream, StartStream,
                                     StopStream, AbortStream, IsStreamStopped, IsStreamActive,
                                     GetStreamTime, GetStreamCpuLoad,
                                     PaUtil_DummyRead, PaUtil_DummyWrite,
                                     PaUtil_DummyGetReadAvailable, PaUtil_DummyGetWriteAvailable );

    PaUtil_InitializeStreamInterface(&SGIHostApi->blockingStreamInterface, CloseStream, StartStream,
                                     StopStream, AbortStream, IsStreamStopped, IsStreamActive,
                                     GetStreamTime, PaUtil_DummyGetCpuLoad,
                                     ReadStream, WriteStream, GetStreamReadAvailable, GetStreamWriteAvailable );
cleanup:        
    if (result != paNoError)
        {
        if (SGIHostApi)
            {
            if (SGIHostApi->allocations)
                {
                PaUtil_FreeAllAllocations(SGIHostApi->allocations);
                PaUtil_DestroyAllocationGroup(SGIHostApi->allocations);
                }
            PaUtil_FreeMemory(SGIHostApi);
            }
        }
    return result;
}


static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaSGIHostApiRepresentation *SGIHostApi = (PaSGIHostApiRepresentation*)hostApi;

    /* Clean up any resources not handled by the allocation group. */
    if( SGIHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( SGIHostApi->allocations );
        PaUtil_DestroyAllocationGroup( SGIHostApi->allocations );
    }
    PaUtil_FreeMemory( SGIHostApi );
}

/*
    Check if samplerate is supported for this output device. Called once
    or twice by function IsFormatSupported() and one time by OpenStream().
    When paUnanticipatedHostError is returned, the caller does NOT have 
    to call PA_SGI_SET_LAST_AL_ERROR() or such.
*/
static PaError sr_supported(int al_device, double sr)
{
    int         e;
    PaError     result;
    ALparamInfo pinfo;
    long long   lsr;    /* 64 bit fixed point internal AL samplerate. */
    
    if (alGetParamInfo(al_device, AL_RATE, &pinfo))
        {
        e = oserror();
        DBUG(("alGetParamInfo(AL_RATE) failed: %s.\n", alGetErrorString(e)));
        if (e == AL_BAD_RESOURCE)
            result = paInvalidDevice;
        else
            {
            PA_SGI_SET_LAST_AL_ERROR()        /* Sure an AL error occured. */
            result = paUnanticipatedHostError;
            }
        }
    else
        {
        lsr = alDoubleToFixed(sr);  /* Within the range? */
        if ((pinfo.min.ll <= lsr) && (lsr <= pinfo.max.ll))
            result = paFormatIsSupported;
        else
            result = paInvalidSampleRate;
        }
    /* DBUG(("sr_supported()=%d.\n", result)); */
    return result;
}


/*
    See common/portaudio.h (suggestedLatency field is ignored).
*/
static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate )
{
    PaSGIHostApiRepresentation* SGIHostApi = (PaSGIHostApiRepresentation*)hostApi;
    int inputChannelCount, outputChannelCount, result;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    
    if( inputParameters )
    {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;
        /* Unless alternate device specification is supported, reject the use of
           paUseHostApiSpecificDeviceSpecification. */
        if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;
        /* Check that input device can support inputChannelCount. */
        if( inputChannelCount > hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels )
            return paInvalidChannelCount;
        /* Validate inputStreamInfo. */
        if( inputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
        /* Check if samplerate is supported for this input device. */
        result = sr_supported(SGIHostApi->sgiDeviceIDs[inputParameters->device].i, sampleRate);
        if (result != paFormatIsSupported) /* PA_SGI_SET_LAST_AL_ERROR() may already be called. */
            return result;
    }
    else
    {
        inputChannelCount = 0;
    }
    if( outputParameters ) /* As with input above. */
    {
        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;
        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;
        if( outputChannelCount > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels )
            return paInvalidChannelCount;
        if( outputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
        /* Check if samplerate is supported for this output device. */
        result = sr_supported(SGIHostApi->sgiDeviceIDs[outputParameters->device].i, sampleRate);
        if (result != paFormatIsSupported)
            return result;
    }
    else
    {
        outputChannelCount = 0;
    }
    /*  IMPLEMENT ME:
        Because the buffer adapter handles conversion between all standard
        sample formats, the following checks are only required if paCustomFormat
        is implemented, or under some other unusual conditions.

            - check that input device can support inputSampleFormat, or that
              we have the capability to convert from outputSampleFormat to
              a native format

            - check that output device can support outputSampleFormat, or that
              we have the capability to convert from outputSampleFormat to
              a native format
    */
    /* suppress unused variable warnings */
    (void) inputSampleFormat;
    (void) outputSampleFormat;
    return paFormatIsSupported;
}

/** Auxilary struct, embedded twice in the struct below, for inputs and outputs. */
typedef struct PaSGIhostPortBuffer
{
            /** NULL means IRIX AL port closed. */
    ALport  port;
            /** NULL means memory not allocated. */
    void*   buffer;
}
    PaSGIhostPortBuffer;

/** Stream data structure specifically for this IRIX AL implementation. */
typedef struct PaSGIStream
{
    PaUtilStreamRepresentation  streamRepresentation;
    PaUtilCpuLoadMeasurer       cpuLoadMeasurer;
    PaUtilBufferProcessor       bufferProcessor;
    unsigned long               framesPerHostCallback;
                                /** Allocated host buffers and AL ports. */
    PaSGIhostPortBuffer         hostPortBuffIn,
                                hostPortBuffOut;
                                /** Copy of stream flags given to OpenStream(). */
    PaStreamFlags               streamFlags;
                                /** Stream state may be 0 or 1 or 2, but never 3. */
    unsigned char               state;
                                /** Requests to stop or abort may come from the parent,
                                    or from the child itself (user callback result). */
    unsigned char               stopAbort;
    pthread_t                   thread;
}
    PaSGIStream;

/** Stream can be in only one of the following three states: stopped (1), active (2), or
    callback finshed (0). To prevent 'state 3' from occurring, Setting and testing of the
    state bits is done atomically.
*/
#define PA_SGI_STREAM_FLAG_FINISHED_ (0) /* After callback finished or cancelled queued buffers. */
#define PA_SGI_STREAM_FLAG_STOPPED_  (1) /* Set by OpenStream(), StopStream() and AbortStream(). */
#define PA_SGI_STREAM_FLAG_ACTIVE_   (2) /* Set by StartStream. Reset by OpenStream(),           */
                                         /* StopStream() and AbortStream().                      */

/** Stop requests, via the 'stopAbort' field can be either 1, meaning 'stop' or 2, meaning 'abort'.
    When both occur at the same time, 'abort' takes precedence, even after a first 'stop'.
*/
#define PA_SGI_REQ_CONT_    (0)         /* Reset by OpenStream(), StopStream and AbortStream. */
#define PA_SGI_REQ_STOP_    (1)         /* Set by StopStream(). */
#define PA_SGI_REQ_ABORT_   (2)         /* Set by AbortStream(). */


/** Called by OpenStream() once or twice. First, the number of channels, sampleformat, and
    queue size are configured. The configuration is then bound to the specified AL device. 
    Then an AL port is opened. Finally, the samplerate of the device is altered (or at least
    set again).
    
    After successful return, actual latency is written in *latency, and actual samplerate 
    in *samplerate.

    @param pa_params may be NULL and pa_params->channelCount may also be null, in both 
           cases the function immediately returns.
    @return paNoError if configuration was skipped or if it succeeded.
*/
static PaError set_sgi_device(ALvalue*                  sgiDeviceIDs,   /* Array built by PaSGI_Initialize(). */
                              const PaStreamParameters* pa_params,      /* read device and channels. */                             
                              double*                   latency,        /* Read and write in seconds. */
                              
                              PaSampleFormat            pasfmt,         /* Don't read from pa_params!. */
                              char*                     direction,      /* "r" or "w". */
                              char*                     name,
                              long                      framesPerHostBuffer,
                              double*                   samplerate,     /* Also writes back here. */
                              PaSGIhostPortBuffer*      hostPortBuff)   /* Receive pointers here. */
{
    int       bytesPerFrame, sgiDevice, alErr, d, dd, iq_size, default_iq_size;
    ALpv      pvs[2];
    ALconfig  alc = NULL;
    PaError   result = paNoError;

    if (!pa_params)
        goto cleanup;                  /* Not errors, just not full duplex, skip all. */
    if (!pa_params->channelCount)
        goto cleanup;
    alc = alNewConfig();    /* Create default config. This defaults to stereo, 16-bit integer data. */
    if (!alc)               /* Call alFreeConfig() later, when done with it. */
        { result = paInsufficientMemory;  goto cleanup; }
    /*----------------------- CONFIGURE NUMBER OF CHANNELS: ---------------------------*/
    if (alSetChannels(alc, pa_params->channelCount))          /* Returns 0 on success. */
        {
        if (oserror() == AL_BAD_CHANNELS)
            result = paInvalidChannelCount;
        else
            {
            PA_SGI_SET_LAST_AL_ERROR()
            result = paUnanticipatedHostError;
            }
        goto cleanup;
        }
    bytesPerFrame = pa_params->channelCount;          /* Is multiplied by width below. */
    /*----------------------- CONFIGURE SAMPLE FORMAT: --------------------------------*/
    if (pasfmt == paFloat32)
        {
        if (alSetSampFmt(alc, AL_SAMPFMT_FLOAT))
            {
            if (oserror() == AL_BAD_SAMPFMT)
                result = paSampleFormatNotSupported;
            else
                {
                PA_SGI_SET_LAST_AL_ERROR()
                result = paUnanticipatedHostError; 
                }
            goto cleanup;
            }
        bytesPerFrame *= 4;             /* No need to set width for floats. */
        }
    else
        {
        if (alSetSampFmt(alc, AL_SAMPFMT_TWOSCOMP))
            {
            if (oserror() == AL_BAD_SAMPFMT)
                result = paSampleFormatNotSupported;
            else
                {
                PA_SGI_SET_LAST_AL_ERROR()
                result = paUnanticipatedHostError;
                }
            goto cleanup;
            }
        if (pasfmt == paInt8)
            {
            if (alSetWidth(alc, AL_SAMPLE_8))
                {
                if (oserror() == AL_BAD_WIDTH)
                    result = paSampleFormatNotSupported;
                else
                    {
                    PA_SGI_SET_LAST_AL_ERROR()
                    result = paUnanticipatedHostError;
                    }
                goto cleanup;
                }
            /* bytesPerFrame *= 1; */
            }
        else if (pasfmt == paInt16)
            {
            if (alSetWidth(alc, AL_SAMPLE_16))
                {
                if (oserror() == AL_BAD_WIDTH)
                    result = paSampleFormatNotSupported;
                else
                    {
                    PA_SGI_SET_LAST_AL_ERROR()
                    result = paUnanticipatedHostError;
                    }
                goto cleanup;
                }
            bytesPerFrame *= 2;
            }
        else if (pasfmt == paInt24)
            {
            if (alSetWidth(alc, AL_SAMPLE_24))
                {
                if (oserror() == AL_BAD_WIDTH)
                    result = paSampleFormatNotSupported;
                else
                    {
                    PA_SGI_SET_LAST_AL_ERROR()
                    result = paUnanticipatedHostError;
                    }
                goto cleanup;
                }
            bytesPerFrame *= 3;   /* OR 4 ???????! */
            }
        else return paSampleFormatNotSupported;
        }
    /*----------------------- SET INTERNAL AL QUEUE SIZE: -------------------------------*/
    /*  The AL API doesn't provide a means for querying minimum and maximum buffer sizes.
        So, if the requested size fails, try again with a value that is closer to the AL's 
        default queue size. In this implementation, 'Portaudio latency' corresponds to
        the AL queue size minus one buffersize:
                                                 AL queue size - framesPerHostBuffer
                                    PA latency = -----------------------------------
                                                            sample rate                  */
    default_iq_size = alGetQueueSize(alc);
    if (default_iq_size < 0)     /* So let's first get that 'default size'. */
        {                        /* Default internal queue size could not be determined. */
        PA_SGI_SET_LAST_AL_ERROR()
        result = paUnanticipatedHostError;
        goto cleanup;
        }
    /* AL buffer becomes somewhat bigger than the suggested latency, notice this is   */
    /* based on requsted samplerate, not in the actual rate, which is measured later. */
    /* Do NOT read pa_params->suggestedLatency, but use the limited *latency param!   */
    
    iq_size = (int)(0.5 + ((*latency) * (*samplerate))) + (int)framesPerHostBuffer;
                                                /* The AL buffer becomes somewhat     */
                                                /* bigger than the suggested latency. */
    if (iq_size < (framesPerHostBuffer << 1))   /* Make sure the minimum is twice     */
        {                                       /* framesPerHostBuffer.               */
        DBUG(("Setting minimum queue size.\n"));
        iq_size = (framesPerHostBuffer << 1);
        }
    d = iq_size - default_iq_size;                 /* Determine whether we'll decrease */
    while (alSetQueueSize(alc, iq_size))           /* or increase after failing.       */
        {                                          /* Size in sample frames.           */
        if (oserror() != AL_BAD_QSIZE)                       /* Stop at AL_BAD_CONFIG. */
            {
            PA_SGI_SET_LAST_AL_ERROR()
            result = paUnanticipatedHostError;
            goto cleanup;
            }
        dd = iq_size - default_iq_size;     /* Stop when even the default size failed  */
        if (((d >= 0) && (dd <= 0)) ||      /* (dd=0) or when difference flipped sign. */
            ((d <= 0) && (dd >= 0)) ||
            (iq_size <= framesPerHostBuffer))  /* Also guarentee that framesPerHostBuffer */
            {                                  /* can be subtracted (res>0) after return. */
            DBUG(("Could not set AL queue size to %d sample frames!\n", iq_size));
            result = paInternalError; /* FIX: PROBABLY AN INAPROPRIATE ERROR CODE HERE.   */
            goto cleanup;             /* As inapropriate as paUnanticipatedHostError was? */
            }
        DBUG(("Failed to set internal queue size to %d frames, ", iq_size));
        if (d > 0)
            iq_size -= framesPerHostBuffer;    /* Try lesser multiple. */
        else
            iq_size += framesPerHostBuffer;    /* Try larger multiple. */
        DBUG(("trying %d frames now...\n", iq_size));
        }
    /* Note: Actual latency is written back to *latency after meausuring actual (not
             the requested) samplerate. See below. 
    */
    /*----------------------- ALLOCATE HOST BUFFER: --------------------------------------*/
    hostPortBuff->buffer = PaUtil_AllocateMemory((long)bytesPerFrame * framesPerHostBuffer);
    if (!hostPortBuff->buffer) /* Caller is responsible for cleanup+close after failures! */
        { result = paInsufficientMemory; goto cleanup; }
    /*----------------------- BIND CONFIGURATION TO DEVICE: ------------------------------*/
    sgiDevice = sgiDeviceIDs[pa_params->device].i;
    if (alSetDevice(alc, sgiDevice)) /* Try to switch the hardware. */
        {
        if (oserror() == AL_BAD_DEVICE)
            result = paInvalidDevice;
        else
            {
            PA_SGI_SET_LAST_AL_ERROR()
            result = paUnanticipatedHostError;
            }
        goto cleanup;
        }
    /*----------------------- OPEN PORT: ----------------------------------------------*/
    hostPortBuff->port = alOpenPort(name, direction, alc);  /* Caller is responsible   */
    if (!hostPortBuff->port)                                /* for closing after fail. */
        {
        PA_SGI_SET_LAST_AL_ERROR()
        result = paUnanticipatedHostError;
        goto cleanup;
        }                                                     /* Maybe set SR earlier? */
    /*----------------------- SET SAMPLERATE: -----------------------------------------*/
    pvs[0].param    = AL_MASTER_CLOCK;       /* Attempt to set a crystal-based sample- */
    pvs[0].value.i  = AL_CRYSTAL_MCLK_TYPE;  /* rate on input or output device.        */
    pvs[1].param    = AL_RATE;
    pvs[1].value.ll = alDoubleToFixed(*samplerate);
    if (2 != alSetParams(sgiDevice, pvs, 2))
        {
        DBUG(("alSetParams() failed to set samplerate to %.4f Hz!\n", *samplerate));
        result = paInvalidSampleRate;
        goto cleanup;
        }
    /*----------------------- GET ACTUAL SAMPLERATE: ---------------------------*/
    alErr = alGetParams(sgiDevice, &pvs[1], 1); /* SEE WHAT WE REALY SET IT TO. */
    if (alErr != 1)                             /* And return that to caller.   */
        {
        DBUG(("alGetParams() failed to read samplerate!\n"));
        result = paInvalidSampleRate;
        goto cleanup;
        }
    *samplerate = alFixedToDouble(pvs[1].value.ll);  /* Between 1 Hz and 1 MHz. */
    if ((*samplerate < 1.0) || (*samplerate > 1000000.0))
        {
        DBUG(("alFixedToDouble() resulted a weird samplerate: %.6f Hz!\n", *samplerate));
        result = paInvalidSampleRate;
        goto cleanup;
        }
    /*----------------------- CALC ACTUAL LATENCY (based on actual SR): -----------------------*/
    *latency = (iq_size - framesPerHostBuffer) / (*samplerate);         /* FIX:  SURE > 0!???? */
cleanup:
    if (alc)
        alFreeConfig(alc); /* We no longer need configuration. */
    return result;
}

/**
    Called by OpenStream() if it fails and by CloseStream. Only used here, in this file.
    Fields MUST be set to NULL or to a valid value, prior to call.
*/
static void streamCleanupAndClose(PaSGIStream* stream)
{
    if (stream->hostPortBuffIn.port)    alClosePort(stream->hostPortBuffIn.port);         /* Close AL ports.  */
    if (stream->hostPortBuffIn.buffer)  PaUtil_FreeMemory(stream->hostPortBuffIn.buffer); /* Release buffers. */
    if (stream->hostPortBuffOut.port)   alClosePort(stream->hostPortBuffOut.port);
    if (stream->hostPortBuffOut.buffer) PaUtil_FreeMemory(stream->hostPortBuffOut.buffer);
}


/* See pa_hostapi.h for a list of validity guarantees made about OpenStream parameters. */
static PaError OpenStream(struct PaUtilHostApiRepresentation* hostApi,
                          PaStream**                          s,
                          const PaStreamParameters*           ipp,
                          const PaStreamParameters*           opp,
                          double                              sampleRate, /* Common to both i and o. */
                          unsigned long                       framesPerBuffer,
                          PaStreamFlags                       streamFlags,
                          PaStreamCallback*                   streamCallback,
                          void*                               userData)
{
    PaError                     result = paNoError;
    PaSGIHostApiRepresentation* SGIHostApi = (PaSGIHostApiRepresentation*)hostApi;
    PaSGIStream*                stream = 0;
    unsigned long               framesPerHostBuffer;   /* Not necessarily the same as framesPerBuffer. */
    int                         inputChannelCount,     outputChannelCount;
    PaSampleFormat              inputSampleFormat,     outputSampleFormat,
                                hostInputSampleFormat, hostOutputSampleFormat;
    double                      sr_in,                 sr_out,
                                latency_in,            latency_out;
    static const PaSampleFormat irixFormats = (paInt8 | paInt16 | paInt24 | paFloat32);
    /* Constant used by PaUtil_SelectClosestAvailableFormat(). Because IRIX AL does not
       provide a way to query for possible formats for a given device, interface or port,
       just add together the formats we know that are supported in general by IRIX AL 
       (at the end of the year 2003): AL_SAMPFMT_TWOSCOMP with AL_SAMPLE_8(=paInt8),
       AL_SAMPLE_16(=paInt16) or AL_SAMPLE_24(=paInt24); AL_SAMPFMT_FLOAT(=paFloat32); 
       AL_SAMPFMT_DOUBLE(=paFloat64); IRIX misses unsigned 8 and 32 bit signed ints.
    */
    DBUG(("OpenStream() started.\n"));
    if (ipp)
        {
        inputChannelCount = ipp->channelCount;
        inputSampleFormat = ipp->sampleFormat;
        /* Unless alternate device specification is supported, reject the use of paUseHostApiSpecificDeviceSpecification. */
        if (ipp->device == paUseHostApiSpecificDeviceSpecification) /* DEVICE CHOOSEN BY CLIENT. */
            return paInvalidDevice;
        /* Check that input device can support inputChannelCount. */
        if (inputChannelCount > hostApi->deviceInfos[ipp->device]->maxInputChannels)
            return paInvalidChannelCount;
        /* Validate inputStreamInfo. */
        if (ipp->hostApiSpecificStreamInfo)
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
        hostInputSampleFormat = PaUtil_SelectClosestAvailableFormat(irixFormats, inputSampleFormat);
        /* Check if samplerate is supported for this input device. */
        result = sr_supported(SGIHostApi->sgiDeviceIDs[ipp->device].i, sampleRate);
        if (result != paFormatIsSupported) /* PA_SGI_SET_LAST_AL_ERROR() may already be called. */
            return result;
        /* Validate input latency. Use defaults if necessary. */
        if (ipp->suggestedLatency < hostApi->deviceInfos[ipp->device]->defaultLowInputLatency)
            latency_in = hostApi->deviceInfos[ipp->device]->defaultLowInputLatency;         /* Low = minimum. */
        else if (ipp->suggestedLatency > 10.0 * hostApi->deviceInfos[ipp->device]->defaultHighInputLatency)
            latency_in = 10.0 * hostApi->deviceInfos[ipp->device]->defaultHighInputLatency; /* 10*High = max. */
        else
            latency_in = ipp->suggestedLatency;
        }
    else
        {
        inputChannelCount = 0;
        inputSampleFormat = hostInputSampleFormat = paInt16; /* Surpress 'uninitialised var' warnings. */
        latency_in = 0.0; /* Necessary? */
        }
    if (opp)
        {
        outputChannelCount = opp->channelCount;        
        outputSampleFormat = opp->sampleFormat;
        if (opp->device == paUseHostApiSpecificDeviceSpecification) /* Like input (above). */
            return paInvalidDevice;
        if (outputChannelCount > hostApi->deviceInfos[opp->device]->maxOutputChannels)
            return paInvalidChannelCount;
        if (opp->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
        hostOutputSampleFormat = PaUtil_SelectClosestAvailableFormat(irixFormats, outputSampleFormat);
        /* Check if samplerate is supported for this output device. */
        result = sr_supported(SGIHostApi->sgiDeviceIDs[opp->device].i, sampleRate);
        if (result != paFormatIsSupported)
            return result;
        /* Validate output latency. Use defaults if necessary. */
        if (opp->suggestedLatency < hostApi->deviceInfos[opp->device]->defaultLowOutputLatency)
            latency_out = hostApi->deviceInfos[opp->device]->defaultLowOutputLatency;         /* Low = minimum. */
        else if (opp->suggestedLatency > 10.0 * hostApi->deviceInfos[opp->device]->defaultHighOutputLatency)
            latency_out = 10.0 * hostApi->deviceInfos[opp->device]->defaultHighOutputLatency; /* 10*High = max. */
        else
            latency_out = opp->suggestedLatency;
        }
    else
        {
        outputChannelCount = 0;
        outputSampleFormat = hostOutputSampleFormat = paInt16; /* Surpress 'uninitialised var' warning. */
        latency_out = 0.0;
        }
    /*  Sure that ipp and opp will never be both NULL. */

    if( (streamFlags & paPlatformSpecificFlags) != 0 )  /* Validate platform specific flags.  */
        return paInvalidFlag;                           /* Unexpected platform specific flag. */

    stream = (PaSGIStream*)PaUtil_AllocateMemory( sizeof(PaSGIStream) );
    if (!stream)
        { result = paInsufficientMemory; goto cleanup; }

    stream->hostPortBuffIn.port    = (ALport)NULL;       /* Ports closed.   */
    stream->hostPortBuffIn.buffer  =         NULL;       /* No buffers yet. */
    stream->hostPortBuffOut.port   = (ALport)NULL;
    stream->hostPortBuffOut.buffer =         NULL;

    if (streamCallback)
        PaUtil_InitializeStreamRepresentation(&stream->streamRepresentation,
               &SGIHostApi->callbackStreamInterface, streamCallback, userData);
    else
        PaUtil_InitializeStreamRepresentation(&stream->streamRepresentation,
               &SGIHostApi->blockingStreamInterface, streamCallback, userData);
                                                     /* (NULL.) */
    if (framesPerBuffer == paFramesPerBufferUnspecified)            /* Proposal 004. */
        { /* Keep framesPerBuffer zero but come up with some fixed host buffer size. */
        double  lowest_lat = 0.0; /* 0.0 to surpress uninit warning, we're sure it will end up higher. */
        if (ipp)
            lowest_lat = latency_in;            /* Sure > 0.0! */
        if (opp && (latency_out < lowest_lat))
            lowest_lat = latency_out;
        /* So that queue size becomes approximately 5 times framesPerHostBuffer: */
        framesPerHostBuffer = (unsigned long)((lowest_lat * sampleRate) / 4.0);
        /* But always limit: */
        if (framesPerHostBuffer < 64L)
            framesPerHostBuffer = 64L;
        else if (framesPerHostBuffer > 32768L)
            framesPerHostBuffer = 32768L;
        DBUG(("Decided to use a fixed host buffer size of %ld frames.\n", framesPerHostBuffer));
        }
    else
        framesPerHostBuffer = framesPerBuffer; /* Then just take the requested amount. No buffer-adaption yet? */

    sr_in = sr_out = sampleRate;
    /*-------------------------------------------------------------------------------------------*/
    result = set_sgi_device(SGIHostApi->sgiDeviceIDs, /* Needed by alSetDevice and other functs. */
                            ipp,                      /* Reads channelCount, device but NOT latency. */
                            &latency_in,              /* Read limited requested latency but also WRITE actual. */
                            hostInputSampleFormat,    /* For alSetSampFmt and alSetWidth. */                            
                            "r",                      /* "r" for reading from input port. */
                            "portaudio in",           /* Name string. */
                            framesPerHostBuffer,      /* As calculated or as requested by the client. */
                            &sr_in,                   /* Receive actual s.rate after setting it. */
                            &stream->hostPortBuffIn); /* Receives ALport and input host buffer.  */
    if (result != paNoError) goto cleanup;
    /*-------------------------------------------------------------------------------------------*/
    result = set_sgi_device(SGIHostApi->sgiDeviceIDs,
                            opp,
                            &latency_out,
                            hostOutputSampleFormat,
                            "w",                      /* "w" for writing. */
                            "portaudio out",
                            framesPerHostBuffer,
                            &sr_out,
                            &stream->hostPortBuffOut);
    if (result != paNoError) goto cleanup;
    /*------------------------------------------------------------------------------------------*/
    if (fabs(sr_in - sr_out) > 0.001)                         /* Make sure both are the 'same'. */
        {
        DBUG(("Weird samplerate difference between input and output!\n"));
        result = paInvalidSampleRate;            /* Could not come up with a better error code. */
        goto cleanup;
        }                                                                 /* sr_in '==' sr_out. */
    sampleRate = sr_in;                  /* Following fields set to estimated or actual values: */
    stream->streamRepresentation.streamInfo.sampleRate    = sampleRate;
    stream->streamRepresentation.streamInfo.inputLatency  = latency_in;  /* 0.0 if output only. */
    stream->streamRepresentation.streamInfo.outputLatency = latency_out; /* 0.0 if input only.  */

    PaUtil_InitializeCpuLoadMeasurer(&stream->cpuLoadMeasurer, sampleRate);
    result = PaUtil_InitializeBufferProcessor(&stream->bufferProcessor,
                    inputChannelCount,   inputSampleFormat,  hostInputSampleFormat,
                    outputChannelCount,  outputSampleFormat, hostOutputSampleFormat,
                    sampleRate,          streamFlags,
                    framesPerBuffer,           /* As requested by OpenStream(), may be zero! */
                    framesPerHostBuffer,       /* Use fixed number of frames per host buffer */
                    paUtilFixedHostBufferSize, /* to keep things simple. See pa_common/pa_   */
                    streamCallback, userData); /* process.h for more hostbuffersize options. */
    if (result != paNoError)
        goto cleanup;

    stream->framesPerHostCallback = framesPerHostBuffer;
    stream->streamFlags           = streamFlags;                  /* Remember priming request. */
    stream->state                 = PA_SGI_STREAM_FLAG_STOPPED_;  /* After opening, the stream */
    stream->stopAbort             = PA_SGI_REQ_CONT_;             /* is in the stopped state.  */
    *s = (PaStream*)stream;         /* Pass object to caller. */
cleanup:
    if (result != paNoError)        /* Always set result when jumping to cleanup after failure. */
        {
        if (stream)
            {
            streamCleanupAndClose(stream); /* Frees i/o buffers and closes AL ports. */
            PaUtil_FreeMemory(stream);
            }
        }
    return result;
}

/** POSIX thread that performs the actual i/o and calls the client's callback,
    spawned by StartStream().
*/
static void* PaSGIpthread(void *userData)
{
    PaSGIStream*              stream = (PaSGIStream*)userData;
    int                       callbackResult = paContinue;
    double                    nanosec_per_frame;
    PaStreamCallbackTimeInfo  timeInfo = { 0, 0, 0 };

    stream->state = PA_SGI_STREAM_FLAG_ACTIVE_;   /* Parent thread also sets active flag, but we 
                                                     make no assumption about who does this first. */
    nanosec_per_frame = 1000000000.0 / stream->streamRepresentation.streamInfo.sampleRate;
    /*----------------------------------------------- OUTPUT PRIMING: -----------------------------*/
    if (stream->hostPortBuffOut.port)              /* Somewhat less than AL queue size so the next */
        {                                          /* output buffer will (probably) not block.     */
        unsigned long frames_to_prime = (long)(0.5 + 
                                        (stream->streamRepresentation.streamInfo.outputLatency
                                         * stream->streamRepresentation.streamInfo.sampleRate));
        if (stream->streamFlags & paPrimeOutputBuffersUsingStreamCallback)
          {
          PaStreamCallbackFlags cbflags = paPrimingOutput;
          if (stream->hostPortBuffIn.port) /* Only set this flag in case of full duplex. */
            cbflags |= paInputUnderflow;
          DBUG(("Prime with client's callback: < %ld frames.\n", frames_to_prime));
          while (frames_to_prime >= stream->framesPerHostCallback)  /* We will not do less (yet).  */
            {                                                     /* TODO: Timestamps and CPU load */
            PaUtil_BeginBufferProcessing(&stream->bufferProcessor,  /* measurement during priming. */
                                         &timeInfo,
                                         cbflags);             /* Pass underflow + priming flags.  */
            if (stream->hostPortBuffIn.port)                   /* Does that provide client's call- */
                PaUtil_SetNoInput(&stream->bufferProcessor);   /* back with silent inputbuffers?   */
            
            PaUtil_SetOutputFrameCount(&stream->bufferProcessor, 0);   /* 0=take host buffer size. */
            PaUtil_SetInterleavedOutputChannels(&stream->bufferProcessor, 0,
                                                 stream->hostPortBuffOut.buffer, 0);
            callbackResult = paContinue;                            /* Call the client's callback. */
            frames_to_prime -= PaUtil_EndBufferProcessing(&stream->bufferProcessor, &callbackResult);
            if (callbackResult == paAbort)
                {                                           /* What should we do in other cases    */
                stream->stopAbort = PA_SGI_REQ_ABORT_;      /* where (callbackResult!=paContinue). */
                break; /* Don't even output the samples just returned (also skip following while). */
                }
            else                                       /* Write interleaved samples to SGI device. */
                alWriteFrames(stream->hostPortBuffOut.port, stream->hostPortBuffOut.buffer, 
                              stream->framesPerHostCallback);
            }
          }
        else /* Prime with silence.  */
            {
            DBUG(("Prime with silence: %ld frames.\n", frames_to_prime));
            alZeroFrames(stream->hostPortBuffOut.port, frames_to_prime);
            }
        }
    /*------------------------------------------------------ I/O: ---------------------------------*/
    while (!stream->stopAbort)         /* Exit loop immediately when 'stop' or 'abort' are raised. */
        {
        unsigned long   framesProcessed;
        stamp_t         fn, t, fnd, td;   /* Unsigned 64 bit. */
        
        PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );
        /* IMPLEMENT ME: - handle buffer slips. */
        if (stream->hostPortBuffIn.port)
            {
            /*  Get device sample frame number associated with next audio sample frame
                we're going to read from this port. */
            alGetFrameNumber(stream->hostPortBuffIn.port, &fn);
            /*  Get some recent pair of (frame number, time) from the audio device to 
                which our port is connected. time is 'UST' which is given in nanoseconds 
                and shared with the other audio devices and with other media. */
            alGetFrameTime(stream->hostPortBuffIn.port, &fnd, &td);
            /*  Calculate UST associated with fn, the next sample frame we're going to read or
                write. Because this is signed arithmetic, code works for both inputs and outputs. */
            t = td + (stamp_t) ((double)(fn - fnd) * nanosec_per_frame);
            /*  If port is not in underflow or overflow state, we can alReadFrames() or 
                alWriteFrames() here and know that t is the time associated with the first 
                sample frame of the buffer we read or write. */
            timeInfo.inputBufferAdcTime = ((PaTime)t) / 1000000000.0;
            /* Read interleaved samples from AL port (I think it will block only the first time). */
            alReadFrames(stream->hostPortBuffIn.port, stream->hostPortBuffIn.buffer, 
                         stream->framesPerHostCallback);
            }
        if (stream->hostPortBuffOut.port)
            {
            alGetFrameNumber(stream->hostPortBuffOut.port, &fn);
            alGetFrameTime(stream->hostPortBuffOut.port, &fnd, &td);
            t = td + (stamp_t) ((double)(fn - fnd) * nanosec_per_frame);
            timeInfo.outputBufferDacTime = ((PaTime)t) / 1000000000.0;
            }
        dmGetUST((unsigned long long*)(&t));                /* Receive time in nanoseconds in t. */
        timeInfo.currentTime = ((PaTime)t) / 1000000000.0;
        
        /* If you need to byte swap or shift inputBuffer to convert it into a pa format, do it here. */
        PaUtil_BeginBufferProcessing(&stream->bufferProcessor,
                                     &timeInfo,
                                     0 /* IMPLEMENT ME: pass underflow/overflow flags when necessary */);
                                     
        if (stream->hostPortBuffIn.port)                    /* Equivalent to (inputChannelCount > 0) */
            {                /* We are sure about the amount to transfer (PaUtil_Set before alRead). */
            PaUtil_SetInputFrameCount(&stream->bufferProcessor, 0 /* 0 means take host buffer size */);
            PaUtil_SetInterleavedInputChannels(&stream->bufferProcessor,
                    0, /* first channel of inputBuffer is channel 0 */
                    stream->hostPortBuffIn.buffer,
                    0 ); /* 0 - use inputChannelCount passed to init buffer processor */
            }
        if (stream->hostPortBuffOut.port)
            {
            PaUtil_SetOutputFrameCount(&stream->bufferProcessor, 0 /* 0 means take host buffer size */);
            PaUtil_SetInterleavedOutputChannels(&stream->bufferProcessor,
                    0, /* first channel of outputBuffer is channel 0 */
                    stream->hostPortBuffOut.buffer,
                    0 ); /* 0 - use outputChannelCount passed to init buffer processor */
            }
        /*
            You must pass a valid value of callback result to PaUtil_EndBufferProcessing()
            in general you would pass paContinue for normal operation, and
            paComplete to drain the buffer processor's internal output buffer.
            You can check whether the buffer processor's output buffer is empty
            using PaUtil_IsBufferProcessorOuputEmpty( bufferProcessor )
        */
        callbackResult = paContinue;      /* Whoops, lost this somewhere, back again in v 1.2.2.16! */
        framesProcessed = PaUtil_EndBufferProcessing(&stream->bufferProcessor, &callbackResult);
        /* If you need to byte swap or shift outputBuffer to convert it to host format, do it here. */
        PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );
        
        if (callbackResult != paContinue)
            {                                              /* Once finished, call the finished callback. */
            DBUG(("SGI callbackResult = %d.\n", callbackResult));
            if (stream->streamRepresentation.streamFinishedCallback)
                stream->streamRepresentation.streamFinishedCallback(stream->streamRepresentation.userData);
            if (callbackResult == paAbort)
                {
                stream->stopAbort = PA_SGI_REQ_ABORT_;
                break;  /* Don't play the last buffer returned. */
                }
            else        /* paComplete or some other non-zero value. */
                stream->stopAbort = PA_SGI_REQ_STOP_;
            }
        if (stream->hostPortBuffOut.port)                /* Write interleaved samples to SGI device */
            alWriteFrames(stream->hostPortBuffOut.port,  /* (like unix_oss, AFTER checking callback result). */
                          stream->hostPortBuffOut.buffer, stream->framesPerHostCallback);
        }
    if (stream->hostPortBuffOut.port) /* Drain output buffer(s), as long as we don't see an 'abort' request. */
        {
        while ((!(stream->stopAbort & PA_SGI_REQ_ABORT_)) &&    /* Assume _STOP_ is set (or meant). */
               (alGetFilled(stream->hostPortBuffOut.port) > 1)) /* In case of ABORT we quickly leave (again). */
            ; /* Don't provide any new (not even silent) samples, but let an underrun [almost] occur. */
        }
    if (callbackResult != paContinue)
        stream->state = PA_SGI_STREAM_FLAG_FINISHED_;
    return NULL;
}


/*
    When CloseStream() is called, the multi-api layer ensures
    that the stream has already been stopped or aborted.
*/
static PaError CloseStream(PaStream* s)
{
    PaError       result = paNoError;
    PaSGIStream*  stream = (PaSGIStream*)s;

    DBUG(("SGI CloseStream() started.\n"));
    streamCleanupAndClose(stream); /* Releases i/o buffers and closes AL ports. */
    PaUtil_TerminateBufferProcessor(&stream->bufferProcessor);
    PaUtil_TerminateStreamRepresentation(&stream->streamRepresentation);
    PaUtil_FreeMemory(stream);
    return result;
}


static PaError StartStream(PaStream *s)
{
    PaError       result = paNoError;
    PaSGIStream*  stream = (PaSGIStream*)s;

    DBUG(("StartStream() started.\n"));
    PaUtil_ResetBufferProcessor(&stream->bufferProcessor); /* See pa_common/pa_process.h. */
    if (stream->bufferProcessor.streamCallback)
        {                                       /* only when callback is used */
        if (pthread_create(&stream->thread,
                           NULL,                /* pthread_attr_t * attr */
                           PaSGIpthread,        /* Function to spawn.    */
                           (void*)stream))      /* Pass stream as arg.   */
            {
            PA_SGI_SET_LAST_IRIX_ERROR()        /* Let's hope oserror() tells something useful. */
            result = paUnanticipatedHostError;
            }
        else
            stream->state = PA_SGI_STREAM_FLAG_ACTIVE_;
        }                   /* Set active before returning from this function. */
    else
        stream->state = PA_SGI_STREAM_FLAG_ACTIVE_; /* Apparently, setting active for blocking i/o is */
    return result;                                  /* necessary (for patest_write_sine for example). */
}


static PaError StopStream( PaStream *s )
{
    PaError         result = paNoError;
    PaSGIStream*    stream = (PaSGIStream*)s;
    
    if (stream->bufferProcessor.streamCallback) /* Only for callback streams. */
        {
        stream->stopAbort = PA_SGI_REQ_STOP_;   /* Signal and wait for the thread to drain outputs. */
        if (pthread_join(stream->thread, NULL)) /* When succesful, stream->state */
            {                                   /* is still ACTIVE, or FINISHED. */
            PA_SGI_SET_LAST_IRIX_ERROR()
            result = paUnanticipatedHostError;
            }
        else  /* Transition from ACTIVE or FINISHED to STOPPED. */
            stream->state = PA_SGI_STREAM_FLAG_STOPPED_;
        stream->stopAbort = PA_SGI_REQ_CONT_; /* For possible next start. */
        }
/*  else
        stream->state = PA_SGI_STREAM_FLAG_STOPPED_;  Is this necessary for blocking i/o? */
    return result;
}


static PaError AbortStream( PaStream *s )
{
    PaError result = paNoError;
    PaSGIStream *stream = (PaSGIStream*)s;

    if (stream->bufferProcessor.streamCallback) /* Only for callback streams. */
        {
        stream->stopAbort = PA_SGI_REQ_ABORT_;
        if (pthread_join(stream->thread, NULL))
            {
            PA_SGI_SET_LAST_IRIX_ERROR()
            result = paUnanticipatedHostError;
            }
        else  /* Transition from ACTIVE or FINISHED to STOPPED. */
            stream->state = PA_SGI_STREAM_FLAG_STOPPED_;
        stream->stopAbort = PA_SGI_REQ_CONT_; /* For possible next start. */
        }
/*  else
        stream->state = PA_SGI_STREAM_FLAG_STOPPED_;  Is this necessary for blocking i/o? */
    return result;
}


static PaError IsStreamStopped( PaStream *s )   /* Not just the opposite of IsStreamActive(): */
{                                               /* in the 'callback finished' state, it       */
                                                /* returns zero instead of nonzero.           */
    if (((PaSGIStream*)s)->state & PA_SGI_STREAM_FLAG_STOPPED_)
        return 1;
    return 0;
}


static PaError IsStreamActive( PaStream *s )
{
    if (((PaSGIStream*)s)->state & PA_SGI_STREAM_FLAG_ACTIVE_)
        return 1;
    return 0;
}


static PaTime GetStreamTime( PaStream *s )
{
    stamp_t t;
    
    (void) s; /* Suppress unused argument warning. */
    dmGetUST((unsigned long long*)(&t)); /* Receive time in nanoseconds in t. */
    return (PaTime)t / 1000000000.0;
}


static double GetStreamCpuLoad( PaStream* s )
{
    PaSGIStream *stream = (PaSGIStream*)s;

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
    PaSGIStream*    stream = (PaSGIStream*)s;
    int             n;

printf("stream->framesPerHostCallback=%ld.\n", stream->framesPerHostCallback);
fflush(stdout);

    while (frames)
        {
        if (frames > stream->framesPerHostCallback) n = stream->framesPerHostCallback;
        else                                        n = frames;
        /* Read interleaved samples from SGI device. */
        alReadFrames(stream->hostPortBuffIn.port,           /* Port already opened by OpenStream(). */
                     stream->hostPortBuffIn.buffer, n);     /* Already allocated by OpenStream().   */
                                                            /* alReadFrames() always returns 0.     */
        PaUtil_SetInputFrameCount(&stream->bufferProcessor, 0); /* 0 means take host buffer size */
        PaUtil_SetInterleavedInputChannels(&stream->bufferProcessor,
                                           0,   /* first channel of inputBuffer is channel 0 */
                                           stream->hostPortBuffIn.buffer,
                                           0 ); /* 0 means use inputChannelCount passed at init. */
        /* Copy samples from host input channels set up by the PaUtil_SetInterleavedInputChannels 
           to a user supplied buffer. */
printf("frames=%ld, buffer=%ld\n", frames, (long)buffer);
fflush(stdout);
        PaUtil_CopyInput(&stream->bufferProcessor, &buffer, n);
        frames -= n;
        }
printf("DONE: frames=%ld, buffer=%ld\n", frames, (long)buffer);
    return paNoError;
}


static PaError WriteStream( PaStream* s,
                            const void *buffer,
                            unsigned long frames )
{
    PaSGIStream*    stream = (PaSGIStream*)s;
    unsigned long   n;
    while (frames)
        {
        PaUtil_SetOutputFrameCount(&stream->bufferProcessor, 0); /* 0 means take host buffer size */
        PaUtil_SetInterleavedOutputChannels(&stream->bufferProcessor,
                                            0,   /* first channel of inputBuffer is channel 0 */
                                            stream->hostPortBuffOut.buffer,
                                            0 ); /* 0 means use inputChannelCount passed at init. */
        /* Copy samples from user supplied buffer to host input channels set up by
           PaUtil_SetInterleavedOutputChannels. Copies the minimum of the number of user frames 
           (specified by the frameCount parameter) and the number of host frames (specified in 
           a previous call to SetOutputFrameCount()). */
        n = PaUtil_CopyOutput(&stream->bufferProcessor, &buffer, frames);
        /* Write interleaved samples to SGI device. */
        alWriteFrames(stream->hostPortBuffOut.port, stream->hostPortBuffOut.buffer, n);
        frames -= n;                                           /* alWriteFrames always returns 0. */
        }
    return paNoError;
}


static signed long GetStreamReadAvailable( PaStream* s )
{
    return (signed long)alGetFilled(((PaSGIStream*)s)->hostPortBuffIn.port);
}


static signed long GetStreamWriteAvailable( PaStream* s )
{
    return (signed long)alGetFillable(((PaSGIStream*)s)->hostPortBuffOut.port);
}


/* CVS reminder:
   To download the 'v19-devel' branch from portaudio's CVS server for the first time, type:
    cvs -d:pserver:anonymous@www.portaudio.com:/home/cvs checkout -r v19-devel portaudio
   Then 'cd' to the 'portaudio' directory that should have been created.
   To commit changes:
    cvs -d:pserver:pieter@www.portaudio.com:/home/cvs login
    cvs -d:pserver:pieter@www.portaudio.com:/home/cvs commit -m 'blabla.' -r v19-devel pa_sgi/pa_sgi.c
    cvs -d:pserver:pieter@www.portaudio.com:/home/cvs logout
   To see if someone else worked on something:
    cvs -d:pserver:anonymous@www.portaudio.com:/home/cvs update -r v19-devel
   To get an older revision of a certain file (without sticky business):
    cvs -d:pserver:anonymous@www.portaudio.com:/home/cvs update -p -r 1.1.1.1.2.4 pa_tests/patest1.c >pa_tests/patest1.c-OLD
   To see logs:
    cvs -d:pserver:anonymous@www.portaudio.com:/home/cvs log pa_common/pa_skeleton.c
*/
