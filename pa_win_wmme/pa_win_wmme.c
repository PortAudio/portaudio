/*
 * $Id$
 * pa_win_wmme.c
 * Implementation of PortAudio for Windows MultiMedia Extensions (WMME)
 *
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 *
 * Authors: Ross Bencina and Phil Burk
 * Copyright (c) 1999-2000 Ross Bencina and Phil Burk
 *
 * Permission is hereby granted, free of charge, to any person obtainingF
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
/*
  All memory allocations and frees are marked with MEM for quick review.
*/

/* Modification History:
 PLB = Phil Burk
 JM = Julien Maillard
 PLB20010402 - sDevicePtrs now allocates based on sizeof(pointer)
 PLB20010413 - check for excessive numbers of channels
 PLB20010422 - apply Mike Berry's changes for CodeWarrior on PC
               including condition including of memory.h,
      and explicit typecasting on memory allocation
 PLB20010802 - use GlobalAlloc for sDevicesPtr instead of PaHost_AllocFastMemory
 PLB20010816 - pass process instead of thread to SetPriorityClass()
 PLB20010927 - use number of frames instead of real-time for CPULoad calculation.
 JM20020118 - prevent hung thread when buffers underflow.
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <windows.h>
#include <mmsystem.h>
#include <process.h>
/* PLB20010422 - "memory.h" doesn't work on CodeWarrior for PC. Thanks Mike Berry for the mod. */
#ifndef __MWERKS__
#include <malloc.h>
#include <memory.h>
#endif /* __MWERKS__ */
#include "portaudio.h"
#include "pa_host.h"
#include "pa_trace.h"

/************************************************* Constants ********/
#define PA_TRACK_MEMORY          (0)

#define PA_USE_TIMER_CALLBACK    (0)  /* Select between two options for background task. 0=thread, 1=timer */
#define PA_USE_HIGH_LATENCY      (0)  /* For debugging glitches. */
/* Switches for debugging. */
#define PA_SIMULATE_UNDERFLOW    (0)  /* Set to one to force an underflow of the output buffer. */
/* To trace program, enable TRACE_REALTIME_EVENTS in pa_trace.h */
#define PA_TRACE_RUN             (0)
#define PA_TRACE_START_STOP      (1)
#if PA_USE_HIGH_LATENCY
 #define PA_MIN_MSEC_PER_HOST_BUFFER  (100)
 #define PA_MAX_MSEC_PER_HOST_BUFFER  (300) /* Do not exceed unless user buffer exceeds */
 #define PA_MIN_NUM_HOST_BUFFERS      (4)
 #define PA_MAX_NUM_HOST_BUFFERS      (16)  /* OK to exceed if necessary */
 #define PA_WIN_9X_LATENCY            (400)
#else
 #define PA_MIN_MSEC_PER_HOST_BUFFER  (10)
 #define PA_MAX_MSEC_PER_HOST_BUFFER  (100) /* Do not exceed unless user buffer exceeds */
 #define PA_MIN_NUM_HOST_BUFFERS      (3)
 #define PA_MAX_NUM_HOST_BUFFERS      (16)  /* OK to exceed if necessary */
 #define PA_WIN_9X_LATENCY            (200)
#endif
#define MIN_TIMEOUT_MSEC                 (1000)
/*
** Use higher latency for NT because it is even worse at real-time
** operation than Win9x.
*/
#define PA_WIN_NT_LATENCY        (PA_WIN_9X_LATENCY * 2)

#if PA_SIMULATE_UNDERFLOW
static  gUnderCallbackCounter = 0;
#define UNDER_SLEEP_AT       (40)
#define UNDER_SLEEP_FOR      (500)
#endif

#define PRINT(x) { printf x; fflush(stdout); }
#define ERR_RPT(x) PRINT(x)
#define DBUG(x)  /* PRINT(x) */
#define DBUGX(x) /* PRINT(x) */
/************************************************* Definitions ********/
/**************************************************************
 * Structure for internal host specific stream data.
 * This is allocated on a per stream basis.
 */
typedef struct PaHostSoundControl
{
    /* Input -------------- */
    HWAVEIN            pahsc_HWaveIn;
    WAVEHDR           *pahsc_InputBuffers;
    int                pahsc_CurrentInputBuffer;
    int                pahsc_BytesPerHostInputBuffer;
    int                pahsc_BytesPerUserInputBuffer;    /* native buffer size in bytes */
    /* Output -------------- */
    HWAVEOUT           pahsc_HWaveOut;
    WAVEHDR           *pahsc_OutputBuffers;
    int                pahsc_CurrentOutputBuffer;
    int                pahsc_BytesPerHostOutputBuffer;
    int                pahsc_BytesPerUserOutputBuffer;    /* native buffer size in bytes */
    /* Run Time -------------- */
    PaTimestamp        pahsc_FramesPlayed;
    long               pahsc_LastPosition;                /* used to track frames played. */
    /* For measuring CPU utilization. */
    LARGE_INTEGER      pahsc_EntryCount;
    double             pahsc_InverseTicksPerHostBuffer;
    /* Init Time -------------- */
    int                pahsc_NumHostBuffers;
    int                pahsc_FramesPerHostBuffer;
    int                pahsc_UserBuffersPerHostBuffer;
    CRITICAL_SECTION   pahsc_StreamLock;                  /* Mutext to prevent threads from colliding. */
    INT                pahsc_StreamLockInited;
#if PA_USE_TIMER_CALLBACK
    BOOL               pahsc_IfInsideCallback;            /* Test for reentrancy. */
    MMRESULT           pahsc_TimerID;
#else
    HANDLE             pahsc_AbortEvent;
    int                pahsc_AbortEventInited;
    HANDLE             pahsc_BufferEvent;
    int                pahsc_BufferEventInited;
    HANDLE             pahsc_EngineThread;
    DWORD              pahsc_EngineThreadID;
#endif
}
PaHostSoundControl;
/************************************************* Shared Data ********/
/* FIXME - put Mutex around this shared data. */
static int sNumInputDevices = 0;
static int sNumOutputDevices = 0;
static int sNumDevices = 0;
static PaDeviceInfo **sDevicePtrs = NULL;
static int sDefaultInputDeviceID = paNoDevice;
static int sDefaultOutputDeviceID = paNoDevice;
static int sPaHostError = 0;
static const char sMapperSuffixInput[] = " - Input";
static const char sMapperSuffixOutput[] = " - Output";
static int sNumAllocations = 0;

/************************************************* Macros ********/
/* Convert external PA ID to an internal ID that includes WAVE_MAPPER */
#define PaDeviceIdToWinId(id) (((id) < sNumInputDevices) ? (id - 1) : (id - sNumInputDevices - 1))
/************************************************* Prototypes **********/
static Pa_QueryDevices( void );
static void CALLBACK Pa_TimerCallback(UINT uID, UINT uMsg,
                                      DWORD dwUser, DWORD dw1, DWORD dw2);
PaError PaHost_GetTotalBufferFrames( internalPortAudioStream   *past );
static PaError PaHost_UpdateStreamTime( PaHostSoundControl *pahsc );
static PaError PaHost_BackgroundManager( internalPortAudioStream   *past );

static void *PaHost_AllocateTrackedMemory( long numBytes );
static void PaHost_FreeTrackedMemory( void *addr );

/********************************* BEGIN CPU UTILIZATION MEASUREMENT ****/
static void Pa_StartUsageCalculation( internalPortAudioStream   *past )
{
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return;
    /* Query system timer for usage analysis and to prevent overuse of CPU. */
    QueryPerformanceCounter( &pahsc->pahsc_EntryCount );
}
static void Pa_EndUsageCalculation( internalPortAudioStream   *past )
{
    LARGE_INTEGER CurrentCount = { 0, 0 };
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return;
    /*
    ** Measure CPU utilization during this callback. Note that this calculation
    ** assumes that we had the processor the whole time.
    */
#define LOWPASS_COEFFICIENT_0   (0.9)
#define LOWPASS_COEFFICIENT_1   (0.99999 - LOWPASS_COEFFICIENT_0)
    if( QueryPerformanceCounter( &CurrentCount ) )
    {
        LONGLONG InsideCount = CurrentCount.QuadPart - pahsc->pahsc_EntryCount.QuadPart;
        double newUsage = InsideCount * pahsc->pahsc_InverseTicksPerHostBuffer;
        past->past_Usage = (LOWPASS_COEFFICIENT_0 * past->past_Usage) +
                           (LOWPASS_COEFFICIENT_1 * newUsage);
    }
}
/****************************************** END CPU UTILIZATION *******/

static PaError Pa_QueryDevices( void )
{
    int numBytes;
    /* Count the devices and add one extra for the WAVE_MAPPER */
    sNumInputDevices = waveInGetNumDevs() + 1;
    sDefaultInputDeviceID = 0;
    sNumOutputDevices = waveOutGetNumDevs() + 1;
    sDefaultOutputDeviceID = sNumInputDevices;
    sNumDevices = sNumInputDevices + sNumOutputDevices;
    /* Allocate structures to hold device info. */
    /* PLB20010402 - was allocating too much memory. */
    /* numBytes = sNumDevices * sizeof(PaDeviceInfo);  // PLB20010402 */
    numBytes = sNumDevices * sizeof(PaDeviceInfo *); /* PLB20010402 */
    sDevicePtrs = (PaDeviceInfo **) PaHost_AllocateTrackedMemory( numBytes ); /* MEM */
    if( sDevicePtrs == NULL ) return paInsufficientMemory;
    return paNoError;
}
/************************************************************************************/
long Pa_GetHostError()
{
    return sPaHostError;
}
/*************************************************************************/
int Pa_CountDevices()
{
    if( sNumDevices <= 0 ) Pa_Initialize();
    return sNumDevices;
}
/*************************************************************************
** If a PaDeviceInfo structure has not already been created,
** then allocate one and fill it in for the selected device.
**
** We create one extra input and one extra output device for the WAVE_MAPPER.
** [Does anyone know how to query the default device and get its name?]
*/
const PaDeviceInfo* Pa_GetDeviceInfo( PaDeviceID id )
{
#define NUM_STANDARDSAMPLINGRATES   3   /* 11.025, 22.05, 44.1 */
#define NUM_CUSTOMSAMPLINGRATES     5   /* must be the same number of elements as in the array below */
#define MAX_NUMSAMPLINGRATES        (NUM_STANDARDSAMPLINGRATES+NUM_CUSTOMSAMPLINGRATES)
    static DWORD customSamplingRates[] = { 32000, 48000, 64000, 88200, 96000 };
    PaDeviceInfo *deviceInfo;
    double *sampleRates; /* non-const ptr */
    int i;
    char *s;

    if( id < 0 || id >= sNumDevices )
        return NULL;
    if( sDevicePtrs[ id ] != NULL )
    {
        return sDevicePtrs[ id ];
    }
    deviceInfo = (PaDeviceInfo *)PaHost_AllocateTrackedMemory( sizeof(PaDeviceInfo) ); /* MEM */
    if( deviceInfo == NULL ) return NULL;
    deviceInfo->structVersion = 1;
    deviceInfo->maxInputChannels = 0;
    deviceInfo->maxOutputChannels = 0;
    deviceInfo->numSampleRates = 0;
    sampleRates = (double*)PaHost_AllocateTrackedMemory( MAX_NUMSAMPLINGRATES * sizeof(double) ); /* MEM */
    deviceInfo->sampleRates = sampleRates;
    deviceInfo->nativeSampleFormats = paInt16;       /* should query for higher bit depths below */
    if( id < sNumInputDevices )
    {
        /* input device */
        int inputMmID = id - 1; /* WAVE_MAPPER is -1 so we start with WAVE_MAPPER */
        WAVEINCAPS wic;
        if( waveInGetDevCaps( inputMmID, &wic, sizeof( WAVEINCAPS ) ) != MMSYSERR_NOERROR )
            goto error;

        /* Append I/O suffix to WAVE_MAPPER device. */
        if( inputMmID == WAVE_MAPPER )
        {
            s = (char *) PaHost_AllocateTrackedMemory( strlen( wic.szPname ) + 1 + sizeof(sMapperSuffixInput) ); /* MEM */
            strcpy( s, wic.szPname );
            strcat( s, sMapperSuffixInput );
        }
        else
        {
            s = (char *) PaHost_AllocateTrackedMemory( strlen( wic.szPname ) + 1 ); /* MEM */
            strcpy( s, wic.szPname );
        }
        deviceInfo->name = s;
        deviceInfo->maxInputChannels = wic.wChannels;
        /* Sometimes a device can return a rediculously large number of channels.
        ** This happened with an SBLive card on a Windows ME box.
        ** If that happens, then force it to 2 channels.  PLB20010413
        */
        if( (deviceInfo->maxInputChannels < 1) || (deviceInfo->maxInputChannels > 256) )
        {
            ERR_RPT(("Pa_GetDeviceInfo: Num input channels reported as %d! Changed to 2.\n", deviceInfo->maxOutputChannels ));
            deviceInfo->maxInputChannels = 2;
        }
        /* Add a sample rate to the list if we can do stereo 16 bit at that rate
         * based on the format flags. */
        if( wic.dwFormats & WAVE_FORMAT_1M16 ||wic.dwFormats & WAVE_FORMAT_1S16 )
            sampleRates[ deviceInfo->numSampleRates++ ] = 11025.;
        if( wic.dwFormats & WAVE_FORMAT_2M16 ||wic.dwFormats & WAVE_FORMAT_2S16 )
            sampleRates[ deviceInfo->numSampleRates++ ] = 22050.;
        if( wic.dwFormats & WAVE_FORMAT_4M16 ||wic.dwFormats & WAVE_FORMAT_4S16 )
            sampleRates[ deviceInfo->numSampleRates++ ] = 44100.;
        /* Add a sample rate to the list if we can do stereo 16 bit at that rate
         * based on opening the device successfully. */
        for( i=0; i < NUM_CUSTOMSAMPLINGRATES; i++ )
        {
            WAVEFORMATEX wfx;
            wfx.wFormatTag = WAVE_FORMAT_PCM;
            wfx.nSamplesPerSec = customSamplingRates[i];
            wfx.wBitsPerSample = 16;
            wfx.cbSize = 0; /* ignored */
            wfx.nChannels = (WORD)deviceInfo->maxInputChannels;
            wfx.nAvgBytesPerSec = wfx.nChannels * wfx.nSamplesPerSec * sizeof(short);
            wfx.nBlockAlign = (WORD)(wfx.nChannels * sizeof(short));
            if( waveInOpen( NULL, inputMmID, &wfx, 0, 0, WAVE_FORMAT_QUERY ) == MMSYSERR_NOERROR )
            {
                sampleRates[ deviceInfo->numSampleRates++ ] = customSamplingRates[i];
            }
        }

    }
    else if( id - sNumInputDevices < sNumOutputDevices )
    {
        /* output device */
        int outputMmID = id - sNumInputDevices - 1;
        WAVEOUTCAPS woc;
        if( waveOutGetDevCaps( outputMmID, &woc, sizeof( WAVEOUTCAPS ) ) != MMSYSERR_NOERROR )
            goto error;
        /* Append I/O suffix to WAVE_MAPPER device. */
        if( outputMmID == WAVE_MAPPER )
        {
            s = (char *) PaHost_AllocateTrackedMemory( strlen( woc.szPname ) + 1 + sizeof(sMapperSuffixOutput) );  /* MEM */
            strcpy( s, woc.szPname );
            strcat( s, sMapperSuffixOutput );
        }
        else
        {
            s = (char *) PaHost_AllocateTrackedMemory( strlen( woc.szPname ) + 1 );  /* MEM */
            strcpy( s, woc.szPname );
        }
        deviceInfo->name = s;
        deviceInfo->maxOutputChannels = woc.wChannels;
        /* Sometimes a device can return a rediculously large number of channels.
        ** This happened with an SBLive card on a Windows ME box.
        ** If that happens, then force it to 2 channels. PLB20010413
        */
        if( (deviceInfo->maxOutputChannels < 1) || (deviceInfo->maxOutputChannels > 256) )
        {
            ERR_RPT(("Pa_GetDeviceInfo: Num output channels reported as %d! Changed to 2.\n", deviceInfo->maxOutputChannels ));
            deviceInfo->maxOutputChannels = 2;
        }
        /* Add a sample rate to the list if we can do stereo 16 bit at that rate
         * based on the format flags. */
        if( woc.dwFormats & WAVE_FORMAT_1M16 ||woc.dwFormats & WAVE_FORMAT_1S16 )
            sampleRates[ deviceInfo->numSampleRates++ ] = 11025.;
        if( woc.dwFormats & WAVE_FORMAT_2M16 ||woc.dwFormats & WAVE_FORMAT_2S16 )
            sampleRates[ deviceInfo->numSampleRates++ ] = 22050.;
        if( woc.dwFormats & WAVE_FORMAT_4M16 ||woc.dwFormats & WAVE_FORMAT_4S16 )
            sampleRates[ deviceInfo->numSampleRates++ ] = 44100.;
        /* Add a sample rate to the list if we can do stereo 16 bit at that rate
         * based on opening the device successfully. */
        for( i=0; i < NUM_CUSTOMSAMPLINGRATES; i++ )
        {
            WAVEFORMATEX wfx;
            wfx.wFormatTag = WAVE_FORMAT_PCM;
            wfx.nSamplesPerSec = customSamplingRates[i];
            wfx.wBitsPerSample = 16;
            wfx.cbSize = 0; /* ignored */
            wfx.nChannels = (WORD)deviceInfo->maxOutputChannels;
            wfx.nAvgBytesPerSec = wfx.nChannels * wfx.nSamplesPerSec * sizeof(short);
            wfx.nBlockAlign = (WORD)(wfx.nChannels * sizeof(short));
            if( waveOutOpen( NULL, outputMmID, &wfx, 0, 0, WAVE_FORMAT_QUERY ) == MMSYSERR_NOERROR )
            {
                sampleRates[ deviceInfo->numSampleRates++ ] = customSamplingRates[i];
            }
        }
    }
    sDevicePtrs[ id ] = deviceInfo;
    return deviceInfo;
error:
    PaHost_FreeTrackedMemory( sampleRates ); /* MEM */
    PaHost_FreeTrackedMemory( deviceInfo ); /* MEM */

    return NULL;
}
/*************************************************************************
** Returns recommended device ID.
** On the PC, the recommended device can be specified by the user by
** setting an environment variable. For example, to use device #1.
**
**    set PA_RECOMMENDED_OUTPUT_DEVICE=1
**
** The user should first determine the available device ID by using
** the supplied application "pa_devs".
*/
#define PA_ENV_BUF_SIZE  (32)
#define PA_REC_IN_DEV_ENV_NAME  ("PA_RECOMMENDED_INPUT_DEVICE")
#define PA_REC_OUT_DEV_ENV_NAME  ("PA_RECOMMENDED_OUTPUT_DEVICE")
static PaDeviceID PaHost_GetEnvDefaultDeviceID( char *envName )
{
    DWORD   hresult;
    char    envbuf[PA_ENV_BUF_SIZE];
    PaDeviceID recommendedID = paNoDevice;
    /* Let user determine default device by setting environment variable. */
    hresult = GetEnvironmentVariable( envName, envbuf, PA_ENV_BUF_SIZE );
    if( (hresult > 0) && (hresult < PA_ENV_BUF_SIZE) )
    {
        recommendedID = atoi( envbuf );
    }
    return recommendedID;
}
static PaError Pa_MaybeQueryDevices( void )
{
    if( sNumDevices == 0 )
    {
        return Pa_QueryDevices();
    }
    return 0;
}
/**********************************************************************
** Check for environment variable, else query devices and use result.
*/
PaDeviceID Pa_GetDefaultInputDeviceID( void )
{
    PaError result;
    result = PaHost_GetEnvDefaultDeviceID( PA_REC_IN_DEV_ENV_NAME );
    if( result < 0 )
    {
        result = Pa_MaybeQueryDevices();
        if( result < 0 ) return result;
        result = sDefaultInputDeviceID;
    }
    return result;
}
PaDeviceID Pa_GetDefaultOutputDeviceID( void )
{
    PaError result;
    result = PaHost_GetEnvDefaultDeviceID( PA_REC_OUT_DEV_ENV_NAME );
    if( result < 0 )
    {
        result = Pa_MaybeQueryDevices();
        if( result < 0 ) return result;
        result = sDefaultOutputDeviceID;
    }
    return result;
}
/**********************************************************************
** Initialize Host dependant part of API.
*/
PaError PaHost_Init( void )
{
    
#if PA_TRACK_MEMORY
    PRINT(("PaHost_Init: sNumAllocations = %d\n", sNumAllocations ));
#endif

#if PA_SIMULATE_UNDERFLOW
    PRINT(("WARNING - Underflow Simulation Enabled - Expect a Big Glitch!!!\n"));
#endif
    return Pa_MaybeQueryDevices();
}

/**********************************************************************
** Check WAVE buffers to see if they are done.
** Fill any available output buffers and use any available
** input buffers by calling user callback.
**
** This routine will loop until:
**    user callback returns !=0 OR
**    all output buffers are filled OR
**    past->past_StopSoon is set OR
**    an error occurs when calling WMME.
**
** Returns >0 when user requests a stop, <0 on error.
**    
*/
static PaError Pa_TimeSlice( internalPortAudioStream   *past )
{
    PaError           result = 0;
    long              bytesEmpty = 0;
    long              bytesFilled = 0;
    long              buffersEmpty = 0;
    MMRESULT          mresult;
    char             *inBufPtr;
    char             *outBufPtr;
    int               gotInput = 0;
    int               gotOutput = 0;
    int               i;
    int               buffersProcessed = 0;
    int               done = 0;
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return paInternalError;

    past->past_NumCallbacks += 1;
#if PA_TRACE_RUN
    AddTraceMessage("Pa_TimeSlice: past_NumCallbacks ", past->past_NumCallbacks );
#endif

    /* JM20020118 - prevent hung thread when buffers underflow. */
    /* while( !done ) /* BAD */
    while( !done && !past->past_StopSoon ) /* GOOD */
    {
#if PA_SIMULATE_UNDERFLOW
        if(gUnderCallbackCounter++ == UNDER_SLEEP_AT)
        {
            Sleep(UNDER_SLEEP_FOR);
        }
#endif

        /* If we are using output, then we need an empty output buffer. */
        gotOutput = 0;
        outBufPtr = NULL;
        if( past->past_NumOutputChannels > 0 )
        {
            if((pahsc->pahsc_OutputBuffers[ pahsc->pahsc_CurrentOutputBuffer ].dwFlags & WHDR_DONE) == 0)
            {
                break;  /* If none empty then bail and try again later. */
            }
            else
            {
                outBufPtr = pahsc->pahsc_OutputBuffers[ pahsc->pahsc_CurrentOutputBuffer ].lpData;
                gotOutput = 1;
            }
        }
        /* Use an input buffer if one is available. */
        gotInput = 0;
        inBufPtr = NULL;
        if( ( past->past_NumInputChannels > 0 ) &&
                (pahsc->pahsc_InputBuffers[ pahsc->pahsc_CurrentInputBuffer ].dwFlags & WHDR_DONE) )
        {
            inBufPtr = pahsc->pahsc_InputBuffers[ pahsc->pahsc_CurrentInputBuffer ].lpData;
            gotInput = 1;
#if PA_TRACE_RUN
            AddTraceMessage("Pa_TimeSlice: got input buffer at ", (int)inBufPtr );
            AddTraceMessage("Pa_TimeSlice: got input buffer # ", pahsc->pahsc_CurrentInputBuffer );
#endif

        }
        /* If we can't do anything then bail out. */
        if( !gotInput && !gotOutput ) break;
        buffersProcessed += 1;
        /* Each Wave buffer contains multiple user buffers so do them all now. */
        /* Base Usage on time it took to process one host buffer. */
        Pa_StartUsageCalculation( past );
        for( i=0; i<pahsc->pahsc_UserBuffersPerHostBuffer; i++ )
        {
            if( done )
            {
                if( gotOutput )
                {
                    /* Clear remainder of wave buffer if we are waiting for stop. */
                    AddTraceMessage("Pa_TimeSlice: zero rest of wave buffer ", i );
                    memset( outBufPtr, 0, pahsc->pahsc_BytesPerUserOutputBuffer );
                }
            }
            else
            {
                /* Convert 16 bit native data to user data and call user routine. */
                result = Pa_CallConvertInt16( past, (short *) inBufPtr, (short *) outBufPtr );
                if( result != 0) done = 1;
            }
            if( gotInput ) inBufPtr += pahsc->pahsc_BytesPerUserInputBuffer;
            if( gotOutput) outBufPtr += pahsc->pahsc_BytesPerUserOutputBuffer;
        }
        Pa_EndUsageCalculation( past );
        /* Send WAVE buffer to Wave Device to be refilled. */
        if( gotInput )
        {
            mresult = waveInAddBuffer( pahsc->pahsc_HWaveIn,
                                       &pahsc->pahsc_InputBuffers[ pahsc->pahsc_CurrentInputBuffer ],
                                       sizeof(WAVEHDR) );
            if( mresult != MMSYSERR_NOERROR )
            {
                sPaHostError = mresult;
                result = paHostError;
                break;
            }
            pahsc->pahsc_CurrentInputBuffer = (pahsc->pahsc_CurrentInputBuffer+1 >= pahsc->pahsc_NumHostBuffers) ?
                                              0 : pahsc->pahsc_CurrentInputBuffer+1;
        }
        /* Write WAVE buffer to Wave Device. */
        if( gotOutput )
        {
#if PA_TRACE_START_STOP
            AddTraceMessage( "Pa_TimeSlice: writing buffer ", pahsc->pahsc_CurrentOutputBuffer );
#endif
            mresult = waveOutWrite( pahsc->pahsc_HWaveOut,
                                    &pahsc->pahsc_OutputBuffers[ pahsc->pahsc_CurrentOutputBuffer ],
                                    sizeof(WAVEHDR) );
            if( mresult != MMSYSERR_NOERROR )
            {
                sPaHostError = mresult;
                result = paHostError;
                break;
            }
            pahsc->pahsc_CurrentOutputBuffer = (pahsc->pahsc_CurrentOutputBuffer+1 >= pahsc->pahsc_NumHostBuffers) ?
                                               0 : pahsc->pahsc_CurrentOutputBuffer+1;
        }

    }

#if PA_TRACE_RUN
    AddTraceMessage("Pa_TimeSlice: buffersProcessed ", buffersProcessed );
#endif
    return (result != 0) ? result : done;
}

/*******************************************************************/
static PaError PaHost_BackgroundManager( internalPortAudioStream   *past )
{
    PaError      result = 0;
    int          i;
    int          numQueuedOutputBuffers = 0;
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;

    /* Has someone asked us to abort by calling Pa_AbortStream()? */
    if( past->past_StopNow )
    {
        past->past_IsActive = 0; /* Will cause thread to return. */
    }
    /* Has someone asked us to stop by calling Pa_StopStream()
     * OR has a user callback returned '1' to indicate finished.
     */
    else if( past->past_StopSoon )
    {
        /* Poll buffer and when all have played then exit thread. */
        /* Count how many output buffers are queued. */
        numQueuedOutputBuffers = 0;
        if( past->past_NumOutputChannels > 0 )
        {
            for( i=0; i<pahsc->pahsc_NumHostBuffers; i++ )
            {
                if( !( pahsc->pahsc_OutputBuffers[ i ].dwFlags & WHDR_DONE) )
                {
#if PA_TRACE_START_STOP
                    AddTraceMessage( "WinMMPa_OutputThreadProc: waiting for buffer ", i );
#endif
                    numQueuedOutputBuffers++;
                }
            }
        }
#if PA_TRACE_START_STOP
        AddTraceMessage( "WinMMPa_OutputThreadProc: numQueuedOutputBuffers ", numQueuedOutputBuffers );
#endif
        if( numQueuedOutputBuffers == 0 )
        {
            past->past_IsActive = 0; /* Will cause thread to return. */
        }
    }
    else
    {
        /* Process full input buffer and fill up empty output buffers. */
        if( (result = Pa_TimeSlice( past )) != 0)
        {
            /* User callback has asked us to stop. */
#if PA_TRACE_START_STOP
            AddTraceMessage( "WinMMPa_OutputThreadProc: TimeSlice() returned ", result );
#endif
            past->past_StopSoon = 1; /* Request that audio play out then stop. */
            result = paNoError;
        }
    }

    PaHost_UpdateStreamTime( pahsc );
    return result;
}

#if PA_USE_TIMER_CALLBACK
/*******************************************************************/
static void CALLBACK Pa_TimerCallback(UINT uID, UINT uMsg, DWORD dwUser, DWORD dw1, DWORD dw2)
{
    internalPortAudioStream   *past;
    PaHostSoundControl        *pahsc;
    PaError                    result;
    past = (internalPortAudioStream *) dwUser;
    if( past == NULL ) return;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return;
    if( pahsc->pahsc_IfInsideCallback )
    {
        if( pahsc->pahsc_TimerID != 0 )
        {
            timeKillEvent(pahsc->pahsc_TimerID);  /* Stop callback timer. */
            pahsc->pahsc_TimerID = 0;
        }
        return;
    }
    pahsc->pahsc_IfInsideCallback = 1;
    /* Manage flags and audio processing. */
    result = PaHost_BackgroundManager( past );
    if( result != paNoError )
    {
        past->past_IsActive = 0;
    }
    pahsc->pahsc_IfInsideCallback = 0;
}
#else /* PA_USE_TIMER_CALLBACK */
/*******************************************************************/
static DWORD WINAPI WinMMPa_OutputThreadProc( void *pArg )
{
    internalPortAudioStream *past;
    PaHostSoundControl      *pahsc;
    void        *inputBuffer=NULL;
    HANDLE       events[2];
    int          numEvents = 0;
    DWORD        result = 0;
    DWORD        waitResult;
    DWORD        numTimeouts = 0;
    DWORD        timeOut;
    past = (internalPortAudioStream *) pArg;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;
#if PA_TRACE_START_STOP
    AddTraceMessage( "WinMMPa_OutputThreadProc: timeoutPeriod", timeoutPeriod );
    AddTraceMessage( "WinMMPa_OutputThreadProc: past_NumUserBuffers", past->past_NumUserBuffers );
#endif
    /* Calculate timeOut as half the time it would take to play all buffers. */
    timeOut = (DWORD) (500.0 * PaHost_GetTotalBufferFrames( past ) / past->past_SampleRate);
    /* Get event(s) ready for wait. */
    events[numEvents++] = pahsc->pahsc_BufferEvent;
    if( pahsc->pahsc_AbortEventInited ) events[numEvents++] = pahsc->pahsc_AbortEvent;
    /* Stay in this thread as long as we are "active". */
    while( past->past_IsActive )
    {
        /*******************************************************************/
        /******** WAIT here for an event from WMME or PA *******************/
        /*******************************************************************/
        waitResult = WaitForMultipleObjects( numEvents, events, FALSE, timeOut );
        /* Error? */
        if( waitResult == WAIT_FAILED )
        {
            sPaHostError = GetLastError();
            result = paHostError;
            past->past_IsActive = 0;
        }
        /* Timeout? Don't stop. Just keep polling for DONE.*/
        else if( waitResult == WAIT_TIMEOUT )
        {
#if PA_TRACE_START_STOP
            AddTraceMessage( "WinMMPa_OutputThreadProc: timed out ", numQueuedOutputBuffers );
#endif
            numTimeouts += 1;
        }
        /* Manage flags and audio processing. */
        result = PaHost_BackgroundManager( past );
        if( result != paNoError )
        {
            past->past_IsActive = 0;
        }
    }
    return result;
}
#endif

/*******************************************************************/
PaError PaHost_OpenInputStream( internalPortAudioStream   *past )
{
    MMRESULT         mr;
    PaError          result = paNoError;
    PaHostSoundControl *pahsc;
    int              i;
    int              inputMmId;
    int              bytesPerInputFrame;
    WAVEFORMATEX     wfx;
    const PaDeviceInfo  *pad;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;
    DBUG(("PaHost_OpenStream: deviceID = 0x%x\n", past->past_InputDeviceID));
    pad = Pa_GetDeviceInfo( past->past_InputDeviceID );
    if( pad == NULL ) return paInternalError;
    switch( pad->nativeSampleFormats  )
    {
    case paInt32:
    case paFloat32:
        bytesPerInputFrame = sizeof(float) * past->past_NumInputChannels;
        break;
    default:
        bytesPerInputFrame = sizeof(short) * past->past_NumInputChannels;
        break;
    }
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD) past->past_NumInputChannels;
    wfx.nSamplesPerSec = (DWORD) past->past_SampleRate;
    wfx.nAvgBytesPerSec = (DWORD)(bytesPerInputFrame * past->past_SampleRate);
    wfx.nBlockAlign = (WORD)bytesPerInputFrame;
    wfx.wBitsPerSample = (WORD)((bytesPerInputFrame/past->past_NumInputChannels) * 8);
    wfx.cbSize = 0;
    inputMmId = PaDeviceIdToWinId( past->past_InputDeviceID );
#if PA_USE_TIMER_CALLBACK
    mr = waveInOpen( &pahsc->pahsc_HWaveIn, inputMmId, &wfx,
                     0, 0, CALLBACK_NULL );
#else
    mr = waveInOpen( &pahsc->pahsc_HWaveIn, inputMmId, &wfx,
                     (DWORD)pahsc->pahsc_BufferEvent, (DWORD) past, CALLBACK_EVENT );
#endif
    if( mr != MMSYSERR_NOERROR )
    {
        ERR_RPT(("PortAudio: PaHost_OpenInputStream() failed!\n"));
        result = paHostError;
        sPaHostError = mr;
        goto error;
    }
    /* Allocate an array to hold the buffer pointers. */
    pahsc->pahsc_InputBuffers = (WAVEHDR *) PaHost_AllocateTrackedMemory( sizeof(WAVEHDR)*pahsc->pahsc_NumHostBuffers ); /* MEM */
    if( pahsc->pahsc_InputBuffers == NULL )
    {
        result = paInsufficientMemory;
        goto error;
    }
    /* Allocate each buffer. */
    for( i=0; i<pahsc->pahsc_NumHostBuffers; i++ )
    {
        pahsc->pahsc_InputBuffers[i].lpData = (char *)PaHost_AllocateTrackedMemory( pahsc->pahsc_BytesPerHostInputBuffer ); /* MEM */
        if( pahsc->pahsc_InputBuffers[i].lpData == NULL )
        {
            result = paInsufficientMemory;
            goto error;
        }
        pahsc->pahsc_InputBuffers[i].dwBufferLength = pahsc->pahsc_BytesPerHostInputBuffer;
        pahsc->pahsc_InputBuffers[i].dwUser = i;
        if( ( mr = waveInPrepareHeader( pahsc->pahsc_HWaveIn, &pahsc->pahsc_InputBuffers[i], sizeof(WAVEHDR) )) != MMSYSERR_NOERROR )
        {
            result = paHostError;
            sPaHostError = mr;
            goto error;
        }
    }
    return result;
error:
    return result;
}
/*******************************************************************/
PaError PaHost_OpenOutputStream( internalPortAudioStream   *past )
{
    MMRESULT         mr;
    PaError          result = paNoError;
    PaHostSoundControl *pahsc;
    int              i;
    int              outputMmID;
    int              bytesPerOutputFrame;
    WAVEFORMATEX     wfx;
    const PaDeviceInfo *pad;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;
    DBUG(("PaHost_OpenStream: deviceID = 0x%x\n", past->past_OutputDeviceID));
    pad = Pa_GetDeviceInfo( past->past_OutputDeviceID );
    if( pad == NULL ) return paInternalError;
    switch( pad->nativeSampleFormats  )
    {
    case paInt32:
    case paFloat32:
        bytesPerOutputFrame = sizeof(float) * past->past_NumOutputChannels;
        break;
    default:
        bytesPerOutputFrame = sizeof(short) * past->past_NumOutputChannels;
        break;
    }
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD) past->past_NumOutputChannels;
    wfx.nSamplesPerSec = (DWORD) past->past_SampleRate;
    wfx.nAvgBytesPerSec = (DWORD)(bytesPerOutputFrame * past->past_SampleRate);
    wfx.nBlockAlign = (WORD)bytesPerOutputFrame;
    wfx.wBitsPerSample = (WORD)((bytesPerOutputFrame/past->past_NumOutputChannels) * 8);
    wfx.cbSize = 0;
    outputMmID = PaDeviceIdToWinId( past->past_OutputDeviceID );
#if PA_USE_TIMER_CALLBACK
    mr = waveOutOpen( &pahsc->pahsc_HWaveOut, outputMmID, &wfx,
                      0, 0, CALLBACK_NULL );
#else

    pahsc->pahsc_AbortEvent = CreateEvent( NULL, TRUE, FALSE, NULL );
    if( pahsc->pahsc_AbortEvent == NULL )
    {
        result = paHostError;
        sPaHostError = GetLastError();
        goto error;
    }
    pahsc->pahsc_AbortEventInited = 1;
    mr = waveOutOpen( &pahsc->pahsc_HWaveOut, outputMmID, &wfx,
                      (DWORD)pahsc->pahsc_BufferEvent, (DWORD) past, CALLBACK_EVENT );
#endif
    if( mr != MMSYSERR_NOERROR )
    {
        ERR_RPT(("PortAudio: PaHost_OpenOutputStream() failed!\n"));
        result = paHostError;
        sPaHostError = mr;
        goto error;
    }
    /* Allocate an array to hold the buffer pointers. */
    pahsc->pahsc_OutputBuffers = (WAVEHDR *) PaHost_AllocateTrackedMemory( sizeof(WAVEHDR)*pahsc->pahsc_NumHostBuffers ); /* MEM */
    if( pahsc->pahsc_OutputBuffers == NULL )
    {
        result = paInsufficientMemory;
        goto error;
    }
    /* Allocate each buffer. */
    for( i=0; i<pahsc->pahsc_NumHostBuffers; i++ )
    {
        pahsc->pahsc_OutputBuffers[i].lpData = (char *) PaHost_AllocateTrackedMemory( pahsc->pahsc_BytesPerHostOutputBuffer ); /* MEM */
        if( pahsc->pahsc_OutputBuffers[i].lpData == NULL )
        {
            result = paInsufficientMemory;
            goto error;
        }
        pahsc->pahsc_OutputBuffers[i].dwBufferLength = pahsc->pahsc_BytesPerHostOutputBuffer;
        pahsc->pahsc_OutputBuffers[i].dwUser = i;
        if( (mr = waveOutPrepareHeader( pahsc->pahsc_HWaveOut, &pahsc->pahsc_OutputBuffers[i], sizeof(WAVEHDR) )) != MMSYSERR_NOERROR )
        {
            result = paHostError;
            sPaHostError = mr;
            goto error;
        }
    }
    return result;
error:
    return result;
}
/*******************************************************************/
PaError PaHost_GetTotalBufferFrames( internalPortAudioStream   *past )
{
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    return pahsc->pahsc_NumHostBuffers * pahsc->pahsc_FramesPerHostBuffer;
}
/*******************************************************************
* Determine number of WAVE Buffers
* and how many User Buffers we can put into each WAVE buffer.
*/
static void PaHost_CalcNumHostBuffers( internalPortAudioStream *past )
{
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    unsigned int  minNumBuffers;
    int           minFramesPerHostBuffer;
    int           maxFramesPerHostBuffer;
    int           minTotalFrames;
    int           userBuffersPerHostBuffer;
    int           framesPerHostBuffer;
    int           numHostBuffers;
    /* Calculate minimum and maximum sizes based on timing and sample rate. */
    minFramesPerHostBuffer = (int) (PA_MIN_MSEC_PER_HOST_BUFFER * past->past_SampleRate / 1000.0);
    minFramesPerHostBuffer = (minFramesPerHostBuffer + 7) & ~7;
    DBUG(("PaHost_CalcNumHostBuffers: minFramesPerHostBuffer = %d\n", minFramesPerHostBuffer ));
    maxFramesPerHostBuffer = (int) (PA_MAX_MSEC_PER_HOST_BUFFER * past->past_SampleRate / 1000.0);
    maxFramesPerHostBuffer = (maxFramesPerHostBuffer + 7) & ~7;
    DBUG(("PaHost_CalcNumHostBuffers: maxFramesPerHostBuffer = %d\n", maxFramesPerHostBuffer ));
    /* Determine number of user buffers based on minimum latency. */
    minNumBuffers = Pa_GetMinNumBuffers( past->past_FramesPerUserBuffer, past->past_SampleRate );
    past->past_NumUserBuffers = ( minNumBuffers > past->past_NumUserBuffers ) ? minNumBuffers : past->past_NumUserBuffers;
    DBUG(("PaHost_CalcNumHostBuffers: min past_NumUserBuffers = %d\n", past->past_NumUserBuffers ));
    minTotalFrames = past->past_NumUserBuffers * past->past_FramesPerUserBuffer;
    /* We cannot make the WAVE buffers too small because they may not get serviced quickly enough. */
    if( (int) past->past_FramesPerUserBuffer < minFramesPerHostBuffer )
    {
        userBuffersPerHostBuffer =
            (minFramesPerHostBuffer + past->past_FramesPerUserBuffer - 1) /
            past->past_FramesPerUserBuffer;
    }
    else
    {
        userBuffersPerHostBuffer = 1;
    }
    framesPerHostBuffer = past->past_FramesPerUserBuffer * userBuffersPerHostBuffer;
    /* Calculate number of WAVE buffers needed. Round up to cover minTotalFrames. */
    numHostBuffers = (minTotalFrames + framesPerHostBuffer - 1) / framesPerHostBuffer;
    /* Make sure we have anough WAVE buffers. */
    if( numHostBuffers < PA_MIN_NUM_HOST_BUFFERS)
    {
        numHostBuffers = PA_MIN_NUM_HOST_BUFFERS;
    }
    else if( (numHostBuffers > PA_MAX_NUM_HOST_BUFFERS) &&
             ((int) past->past_FramesPerUserBuffer < (maxFramesPerHostBuffer/2) ) )
    {
        /* If we have too many WAVE buffers, try to put more user buffers in a wave buffer. */
        while(numHostBuffers > PA_MAX_NUM_HOST_BUFFERS)
        {
            userBuffersPerHostBuffer += 1;
            framesPerHostBuffer = past->past_FramesPerUserBuffer * userBuffersPerHostBuffer;
            numHostBuffers = (minTotalFrames + framesPerHostBuffer - 1) / framesPerHostBuffer;
            /* If we have gone too far, back up one. */
            if( (framesPerHostBuffer > maxFramesPerHostBuffer) ||
                    (numHostBuffers < PA_MAX_NUM_HOST_BUFFERS) )
            {
                userBuffersPerHostBuffer -= 1;
                framesPerHostBuffer = past->past_FramesPerUserBuffer * userBuffersPerHostBuffer;
                numHostBuffers = (minTotalFrames + framesPerHostBuffer - 1) / framesPerHostBuffer;
                break;
            }
        }
    }

    pahsc->pahsc_UserBuffersPerHostBuffer = userBuffersPerHostBuffer;
    pahsc->pahsc_FramesPerHostBuffer = framesPerHostBuffer;
    pahsc->pahsc_NumHostBuffers = numHostBuffers;
    DBUG(("PaHost_CalcNumHostBuffers: pahsc_UserBuffersPerHostBuffer = %d\n", pahsc->pahsc_UserBuffersPerHostBuffer ));
    DBUG(("PaHost_CalcNumHostBuffers: pahsc_NumHostBuffers = %d\n", pahsc->pahsc_NumHostBuffers ));
    DBUG(("PaHost_CalcNumHostBuffers: pahsc_FramesPerHostBuffer = %d\n", pahsc->pahsc_FramesPerHostBuffer ));
    DBUG(("PaHost_CalcNumHostBuffers: past_NumUserBuffers = %d\n", past->past_NumUserBuffers ));
}
/*******************************************************************/
PaError PaHost_OpenStream( internalPortAudioStream   *past )
{
    PaError             result = paNoError;
    PaHostSoundControl *pahsc;
    /* Allocate and initialize host data. */
    pahsc = (PaHostSoundControl *) PaHost_AllocateFastMemory(sizeof(PaHostSoundControl)); /* MEM */
    if( pahsc == NULL )
    {
        result = paInsufficientMemory;
        goto error;
    }
    memset( pahsc, 0, sizeof(PaHostSoundControl) );
    past->past_DeviceData = (void *) pahsc;
    /* Figure out how user buffers fit into WAVE buffers. */
    PaHost_CalcNumHostBuffers( past );
    {
        int msecLatency = (int) ((PaHost_GetTotalBufferFrames(past) * 1000) / past->past_SampleRate);
        DBUG(("PortAudio on WMME - Latency = %d frames, %d msec\n", PaHost_GetTotalBufferFrames(past), msecLatency ));
    }
    InitializeCriticalSection( &pahsc->pahsc_StreamLock );
    pahsc->pahsc_StreamLockInited = 1;

#if (PA_USE_TIMER_CALLBACK == 0)
    pahsc->pahsc_BufferEventInited = 0;
    pahsc->pahsc_BufferEvent = CreateEvent( NULL, FALSE, FALSE, NULL );
    if( pahsc->pahsc_BufferEvent == NULL )
    {
        result = paHostError;
        sPaHostError = GetLastError();
        goto error;
    }
    pahsc->pahsc_BufferEventInited = 1;
#endif /* (PA_USE_TIMER_CALLBACK == 0) */
    /* ------------------ OUTPUT */
    pahsc->pahsc_BytesPerUserOutputBuffer = past->past_FramesPerUserBuffer * past->past_NumOutputChannels * sizeof(short);
    pahsc->pahsc_BytesPerHostOutputBuffer = pahsc->pahsc_UserBuffersPerHostBuffer * pahsc->pahsc_BytesPerUserOutputBuffer;
    if( (past->past_OutputDeviceID != paNoDevice) && (past->past_NumOutputChannels > 0) )
    {
        result = PaHost_OpenOutputStream( past );
        if( result < 0 ) goto error;
    }
    /* ------------------ INPUT */
    pahsc->pahsc_BytesPerUserInputBuffer = past->past_FramesPerUserBuffer * past->past_NumInputChannels * sizeof(short);
    pahsc->pahsc_BytesPerHostInputBuffer = pahsc->pahsc_UserBuffersPerHostBuffer * pahsc->pahsc_BytesPerUserInputBuffer;
    if( (past->past_InputDeviceID != paNoDevice) && (past->past_NumInputChannels > 0) )
    {
        result = PaHost_OpenInputStream( past );
        if( result < 0 ) goto error;
    }
    /* Calculate scalar used in CPULoad calculation. */
    {
        LARGE_INTEGER frequency;
        if( QueryPerformanceFrequency( &frequency ) == 0 )
        {
            pahsc->pahsc_InverseTicksPerHostBuffer = 0.0;
        }
        else
        {
            pahsc->pahsc_InverseTicksPerHostBuffer = past->past_SampleRate /
                    ( (double)frequency.QuadPart * past->past_FramesPerUserBuffer * pahsc->pahsc_UserBuffersPerHostBuffer );
            DBUG(("pahsc_InverseTicksPerHostBuffer = %g\n", pahsc->pahsc_InverseTicksPerHostBuffer ));
        }
    }
    return result;
error:
    PaHost_CloseStream( past );
    return result;
}
/*************************************************************************/
PaError PaHost_StartOutput( internalPortAudioStream *past )
{
    MMRESULT         mr;
    PaHostSoundControl *pahsc;
    PaError          result = paNoError;
    int              i;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( past->past_OutputDeviceID != paNoDevice )
    {
        if( (mr = waveOutPause( pahsc->pahsc_HWaveOut )) != MMSYSERR_NOERROR )
        {
            result = paHostError;
            sPaHostError = mr;
            goto error;
        }
        for( i=0; i<pahsc->pahsc_NumHostBuffers; i++ )
        {
            ZeroMemory( pahsc->pahsc_OutputBuffers[i].lpData, pahsc->pahsc_OutputBuffers[i].dwBufferLength );
            mr = waveOutWrite( pahsc->pahsc_HWaveOut, &pahsc->pahsc_OutputBuffers[i], sizeof(WAVEHDR) );
            if( mr != MMSYSERR_NOERROR )
            {
                result = paHostError;
                sPaHostError = mr;
                goto error;
            }
            past->past_FrameCount += pahsc->pahsc_FramesPerHostBuffer;
        }
        pahsc->pahsc_CurrentOutputBuffer = 0;
        if( (mr = waveOutRestart( pahsc->pahsc_HWaveOut )) != MMSYSERR_NOERROR )
        {
            result = paHostError;
            sPaHostError = mr;
            goto error;
        }
    }
    DBUG(("PaHost_StartOutput: DSW_StartOutput returned = 0x%X.\n", hr));
error:
    return result;
}
/*************************************************************************/
PaError PaHost_StartInput( internalPortAudioStream *past )
{
    PaError          result = paNoError;
    MMRESULT         mr;
    int              i;
    PaHostSoundControl *pahsc;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( past->past_InputDeviceID != paNoDevice )
    {
        for( i=0; i<pahsc->pahsc_NumHostBuffers; i++ )
        {
            mr = waveInAddBuffer( pahsc->pahsc_HWaveIn, &pahsc->pahsc_InputBuffers[i], sizeof(WAVEHDR) );
            if( mr != MMSYSERR_NOERROR )
            {
                result = paHostError;
                sPaHostError = mr;
                goto error;
            }
        }
        pahsc->pahsc_CurrentInputBuffer = 0;
        mr = waveInStart( pahsc->pahsc_HWaveIn );
        DBUG(("Pa_StartStream: waveInStart returned = 0x%X.\n", hr));
        if( mr != MMSYSERR_NOERROR )
        {
            result = paHostError;
            sPaHostError = mr;
            goto error;
        }
    }
error:
    return result;
}
/*************************************************************************/
PaError PaHost_StartEngine( internalPortAudioStream *past )
{
    PaError             result = paNoError;
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
#if PA_USE_TIMER_CALLBACK
    int                 resolution;
    int                 bufsPerTimerCallback;
    int                 msecPerBuffer;
#endif /* PA_USE_TIMER_CALLBACK */

    past->past_StopSoon = 0;
    past->past_StopNow = 0;
    past->past_IsActive = 1;
    pahsc->pahsc_FramesPlayed = 0.0;
    pahsc->pahsc_LastPosition = 0;
#if PA_TRACE_START_STOP
    AddTraceMessage( "PaHost_StartEngine: TimeSlice() returned ", result );
#endif
#if PA_USE_TIMER_CALLBACK
    /* Create timer that will wake us up so we can fill the DSound buffer. */
    bufsPerTimerCallback = pahsc->pahsc_NumHostBuffers/4;
    if( bufsPerTimerCallback < 1 ) bufsPerTimerCallback = 1;
    if( bufsPerTimerCallback < 1 ) bufsPerTimerCallback = 1;
    msecPerBuffer = (1000 * bufsPerTimerCallback *
                     pahsc->pahsc_UserBuffersPerHostBuffer *
                     past->past_FramesPerUserBuffer ) / (int) past->past_SampleRate;
    if( msecPerBuffer < 10 ) msecPerBuffer = 10;
    else if( msecPerBuffer > 100 ) msecPerBuffer = 100;
    resolution = msecPerBuffer/4;
    pahsc->pahsc_TimerID = timeSetEvent( msecPerBuffer, resolution,
                                         (LPTIMECALLBACK) Pa_TimerCallback,
                                         (DWORD) past, TIME_PERIODIC );
    if( pahsc->pahsc_TimerID == 0 )
    {
        result = paHostError;
        sPaHostError = GetLastError();;
        goto error;
    }
#else /* PA_USE_TIMER_CALLBACK */
    ResetEvent( pahsc->pahsc_AbortEvent );
    /* Create thread that waits for audio buffers to be ready for processing. */
    pahsc->pahsc_EngineThread = CreateThread( 0, 0, WinMMPa_OutputThreadProc, past, 0, &pahsc->pahsc_EngineThreadID );
    if( pahsc->pahsc_EngineThread == NULL )
    {
        result = paHostError;
        sPaHostError = GetLastError();;
        goto error;
    }
#if PA_TRACE_START_STOP
    AddTraceMessage( "PaHost_StartEngine: thread ", (int) pahsc->pahsc_EngineThread );
#endif
    /* I used to pass the thread which was failing. I now pass GetCurrentProcess().
    ** This fix could improve latency for some applications. It could also result in CPU
    ** starvation if the callback did too much processing.
    ** I also added result checks, so we might see more failures at initialization.
    ** Thanks to Alberto di Bene for spotting this.
    */
    if( !SetPriorityClass( GetCurrentProcess(), HIGH_PRIORITY_CLASS ) ) /* PLB20010816 */
    {
        result = paHostError;
        sPaHostError = GetLastError();;
        goto error;
    }
    if( !SetThreadPriority( pahsc->pahsc_EngineThread, THREAD_PRIORITY_HIGHEST ) )
    {
        result = paHostError;
        sPaHostError = GetLastError();;
        goto error;
    }
#endif
error:
    return result;
}

/*************************************************************************/
PaError PaHost_StopEngine( internalPortAudioStream *past, int abort )
{
    int timeOut;
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return paNoError;

    /* Tell background thread to stop generating more data and to let current data play out. */
    past->past_StopSoon = 1;
    /* If aborting, tell background thread to stop NOW! */
    if( abort ) past->past_StopNow = 1;

    /* Calculate timeOut longer than longest time it could take to play all buffers. */
    timeOut = (DWORD) (1500.0 * PaHost_GetTotalBufferFrames( past ) / past->past_SampleRate);
    if( timeOut < MIN_TIMEOUT_MSEC ) timeOut = MIN_TIMEOUT_MSEC;

#if PA_USE_TIMER_CALLBACK
    if( (past->past_OutputDeviceID != paNoDevice) &&
            past->past_IsActive &&
            (pahsc->pahsc_TimerID != 0) )
    {
        /* Wait for IsActive to drop. */
        while( (past->past_IsActive) && (timeOut > 0) )
        {
            Sleep(10);
            timeOut -= 10;
        }
        timeKillEvent(pahsc->pahsc_TimerID);  /* Stop callback timer. */
        pahsc->pahsc_TimerID = 0;
    }
#else /* PA_USE_TIMER_CALLBACK */
#if PA_TRACE_START_STOP
    AddTraceMessage( "PaHost_StopEngine: thread ", (int) pahsc->pahsc_EngineThread );
#endif
    if( (past->past_OutputDeviceID != paNoDevice) &&
            (past->past_IsActive) &&
            (pahsc->pahsc_EngineThread != NULL) )
    {
        DWORD got;
        /* Tell background thread to stop generating more data and to let current data play out. */
        DBUG(("PaHost_StopEngine: waiting for background thread.\n"));
        got = WaitForSingleObject( pahsc->pahsc_EngineThread, timeOut );
        if( got == WAIT_TIMEOUT )
        {
            ERR_RPT(("PaHost_StopEngine: timed out while waiting for background thread to finish.\n"));
            return paTimedOut;
        }
        CloseHandle( pahsc->pahsc_EngineThread );
        pahsc->pahsc_EngineThread = NULL;
    }
#endif /* PA_USE_TIMER_CALLBACK */

    past->past_IsActive = 0;
    return paNoError;
}
/*************************************************************************/
PaError PaHost_StopInput( internalPortAudioStream *past, int abort )
{
    MMRESULT mr;
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return paNoError;
    (void) abort;
    if( pahsc->pahsc_HWaveIn != NULL )
    {
        mr = waveInReset( pahsc->pahsc_HWaveIn );
        if( mr != MMSYSERR_NOERROR )
        {
            sPaHostError = mr;
            return paHostError;
        }
    }
    return paNoError;
}
/*************************************************************************/
PaError PaHost_StopOutput( internalPortAudioStream *past, int abort )
{
    MMRESULT mr;
    PaHostSoundControl *pahsc;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return paNoError;
    (void) abort;
#if PA_TRACE_START_STOP
    AddTraceMessage( "PaHost_StopOutput: pahsc_HWaveOut ", (int) pahsc->pahsc_HWaveOut );
#endif
    if( pahsc->pahsc_HWaveOut != NULL )
    {
        mr = waveOutReset( pahsc->pahsc_HWaveOut );
        if( mr != MMSYSERR_NOERROR )
        {
            sPaHostError = mr;
            return paHostError;
        }
    }
    return paNoError;
}
/*******************************************************************/
PaError PaHost_CloseStream( internalPortAudioStream   *past )
{
    int                 i;
    PaHostSoundControl *pahsc;
    if( past == NULL ) return paBadStreamPtr;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return paNoError;
#if PA_TRACE_START_STOP
    AddTraceMessage( "PaHost_CloseStream: pahsc_HWaveOut ", (int) pahsc->pahsc_HWaveOut );
#endif
    /* Free data and device for output. */
    if( pahsc->pahsc_HWaveOut )
    {
        if( pahsc->pahsc_OutputBuffers )
        {
            for( i=0; i<pahsc->pahsc_NumHostBuffers; i++ )
            {
                waveOutUnprepareHeader( pahsc->pahsc_HWaveOut, &pahsc->pahsc_OutputBuffers[i], sizeof(WAVEHDR) );
                PaHost_FreeTrackedMemory( pahsc->pahsc_OutputBuffers[i].lpData ); /* MEM */
            }
            PaHost_FreeTrackedMemory( pahsc->pahsc_OutputBuffers ); /* MEM */
        }
        waveOutClose( pahsc->pahsc_HWaveOut );
    }
    /* Free data and device for input. */
    if( pahsc->pahsc_HWaveIn )
    {
        if( pahsc->pahsc_InputBuffers )
        {
            for( i=0; i<pahsc->pahsc_NumHostBuffers; i++ )
            {
                waveInUnprepareHeader( pahsc->pahsc_HWaveIn, &pahsc->pahsc_InputBuffers[i], sizeof(WAVEHDR) );
                PaHost_FreeTrackedMemory( pahsc->pahsc_InputBuffers[i].lpData ); /* MEM */
            }
            PaHost_FreeTrackedMemory( pahsc->pahsc_InputBuffers ); /* MEM */
        }
        waveInClose( pahsc->pahsc_HWaveIn );
    }
#if (PA_USE_TIMER_CALLBACK == 0)
    if( pahsc->pahsc_AbortEventInited ) CloseHandle( pahsc->pahsc_AbortEvent );
    if( pahsc->pahsc_BufferEventInited ) CloseHandle( pahsc->pahsc_BufferEvent );
#endif
    if( pahsc->pahsc_StreamLockInited )
        DeleteCriticalSection( &pahsc->pahsc_StreamLock );
    PaHost_FreeFastMemory( pahsc, sizeof(PaHostSoundControl) ); /* MEM */
    past->past_DeviceData = NULL;
    return paNoError;
}
/*************************************************************************
** Determine minimum number of buffers required for this host based
** on minimum latency. Latency can be optionally set by user by setting
** an environment variable. For example, to set latency to 200 msec, put:
**
**    set PA_MIN_LATENCY_MSEC=200
**
** in the AUTOEXEC.BAT file and reboot.
** If the environment variable is not set, then the latency will be determined
** based on the OS. Windows NT has higher latency than Win95.
*/
#define PA_LATENCY_ENV_NAME  ("PA_MIN_LATENCY_MSEC")
int Pa_GetMinNumBuffers( int framesPerBuffer, double sampleRate )
{
    char      envbuf[PA_ENV_BUF_SIZE];
    DWORD     hostVersion;
    DWORD     hresult;
    int       minLatencyMsec = 0;
    double    msecPerBuffer = (1000.0 * framesPerBuffer) / sampleRate;
    int       minBuffers;
    /* Let user determine minimal latency by setting environment variable. */
    hresult = GetEnvironmentVariable( PA_LATENCY_ENV_NAME, envbuf, PA_ENV_BUF_SIZE );
    if( (hresult > 0) && (hresult < PA_ENV_BUF_SIZE) )
    {
        minLatencyMsec = atoi( envbuf );
    }
    else
    {
        /* Set minimal latency based on whether NT or Win95.
         * NT has higher latency.
         */
        hostVersion = GetVersion();
        /* High bit clear if NT */
        minLatencyMsec = ( (hostVersion & 0x80000000) == 0 ) ? PA_WIN_NT_LATENCY : PA_WIN_9X_LATENCY  ;
#if PA_USE_HIGH_LATENCY
        PRINT(("PA - Minimum Latency set to %d msec!\n", minLatencyMsec ));
#endif

    }
    DBUG(("PA - Minimum Latency set to %d msec!\n", minLatencyMsec ));
    minBuffers = (int) (1.0 + ((double)minLatencyMsec / msecPerBuffer));
    if( minBuffers < 2 ) minBuffers = 2;
    return minBuffers;
}
/*************************************************************************
** Cleanup device info.
*/
PaError PaHost_Term( void )
{
    int i;
    if( sNumDevices > 0 )
    {
        if( sDevicePtrs != NULL )
        {
            for( i=0; i<sNumDevices; i++ )
            {
                if( sDevicePtrs[i] != NULL )
                {
                    PaHost_FreeTrackedMemory( (char*)sDevicePtrs[i]->name ); /* MEM */
                    PaHost_FreeTrackedMemory( (void*)sDevicePtrs[i]->sampleRates ); /* MEM */
                    PaHost_FreeTrackedMemory( sDevicePtrs[i] ); /* MEM */
                }
            }
            PaHost_FreeTrackedMemory( sDevicePtrs ); /* MEM */
            sDevicePtrs = NULL;
        }
        sNumDevices = 0;
    }

#if PA_TRACK_MEMORY
    PRINT(("PaHost_Term: sNumAllocations = %d\n", sNumAllocations ));
#endif

    return paNoError;
}
/*************************************************************************/
void Pa_Sleep( long msec )
{
    Sleep( msec );
}
/*************************************************************************
 * Allocate memory that can be accessed in real-time.
 * This may need to be held in physical memory so that it is not
 * paged to virtual memory.
 * This call MUST be balanced with a call to PaHost_FreeFastMemory().
 * Memory will be set to zero.
 */
void *PaHost_AllocateFastMemory( long numBytes )
{
    return PaHost_AllocateTrackedMemory( numBytes ); /* FIXME - do we need physical memory? Use VirtualLock() */ /* MEM */
}
/*************************************************************************
 * Free memory that could be accessed in real-time.
 * This call MUST be balanced with a call to PaHost_AllocateFastMemory().
 */
void PaHost_FreeFastMemory( void *addr, long numBytes )
{
    PaHost_FreeTrackedMemory( addr ); /* MEM */
}

/*************************************************************************
 * Track memory allocations to avoid leaks.
 */
static void *PaHost_AllocateTrackedMemory( long numBytes )
{
    void *addr = GlobalAlloc( GPTR, numBytes ); /* MEM */
#if PA_TRACK_MEMORY
    if( addr != NULL ) sNumAllocations += 1;
#endif
    return addr;
}

static void PaHost_FreeTrackedMemory( void *addr )
{
    if( addr != NULL )
    {
        GlobalFree( addr ); /* MEM */
#if PA_TRACK_MEMORY
        sNumAllocations -= 1;
#endif
    }
}

/***********************************************************************/
PaError PaHost_StreamActive( internalPortAudioStream   *past )
{
    PaHostSoundControl *pahsc;
    if( past == NULL ) return paBadStreamPtr;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return paInternalError;
    return (PaError) past->past_IsActive;
}
/*************************************************************************
 * This must be called periodically because mmtime.u.sample
 * is a DWORD and can wrap and lose sync after a few hours.
 */
static PaError PaHost_UpdateStreamTime( PaHostSoundControl *pahsc )
{
    MMRESULT  mr;
    MMTIME    mmtime;
    mmtime.wType = TIME_SAMPLES;
    if( pahsc->pahsc_HWaveOut != NULL )
    {
        mr = waveOutGetPosition( pahsc->pahsc_HWaveOut, &mmtime, sizeof(mmtime) );
    }
    else
    {
        mr = waveInGetPosition( pahsc->pahsc_HWaveIn, &mmtime, sizeof(mmtime) );
    }
    if( mr != MMSYSERR_NOERROR )
    {
        sPaHostError = mr;
        return paHostError;
    }
    /* This data has two variables and is shared by foreground and background. */
    /* So we need to make it thread safe. */
    EnterCriticalSection( &pahsc->pahsc_StreamLock );
    pahsc->pahsc_FramesPlayed += ((long)mmtime.u.sample) - pahsc->pahsc_LastPosition;
    pahsc->pahsc_LastPosition = (long)mmtime.u.sample;
    LeaveCriticalSection( &pahsc->pahsc_StreamLock );
    return paNoError;
}
/*************************************************************************/
PaTimestamp Pa_StreamTime( PortAudioStream *stream )
{
    PaHostSoundControl *pahsc;
    internalPortAudioStream   *past = (internalPortAudioStream *) stream;
    if( past == NULL ) return paBadStreamPtr;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;
    PaHost_UpdateStreamTime( pahsc );
    return pahsc->pahsc_FramesPlayed;
}
