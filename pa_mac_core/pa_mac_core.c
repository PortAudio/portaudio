/*
 * $Id$
 * pa_mac_core.c
 * Implementation of PortAudio for Mac OS X Core Audio
 *
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 *
 * Authors: Ross Bencina and Phil Burk
 * Copyright (c) 1999-2000 Ross Bencina and Phil Burk
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
 * CHANGE HISTORY:
 
 3.29.2001 - Phil Burk - First pass... converted from Window MME code with help from Darren.
 3.30.2001 - Darren Gibbs - Added more support for dynamically querying device info.
 12.7.2001 - Gord Peters - Tweaks to compile on PA V17 and OS X 10.1
 */
#include <CoreServices/CoreServices.h>
#include <CoreAudio/CoreAudio.h>
#include <sys/time.h>
#include <unistd.h>
#include "portaudio.h"
#include "pa_host.h"
#include "pa_trace.h"

/************************************************* Constants ********/
#define PA_USE_TIMER_CALLBACK    (0)  /* Select between two options for background task. 0=thread, 1=timer */
#define PA_USE_HIGH_LATENCY      (0)  /* For debugging glitches. */

/* Switches for debugging. */
#define PA_SIMULATE_UNDERFLOW    (0)  /* Set to one to force an underflow of the output buffer. */

/* To trace program, enable TRACE_REALTIME_EVENTS in pa_trace.h */
#define PA_TRACE_RUN             (0)
#define PA_TRACE_START_STOP      (1)

#define PA_MIN_MSEC_PER_HOST_BUFFER  (10)
#define PA_MAX_MSEC_PER_HOST_BUFFER  (100) /* Do not exceed unless user buffer exceeds */
#define PA_MIN_NUM_HOST_BUFFERS      (3)
#define PA_MAX_NUM_HOST_BUFFERS      (16)  /* OK to exceed if necessary */
#define PA_MIN_LATENCY_MSEC          (200)

#define MIN_TIMEOUT_MSEC                 (1000)

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
    int                pahsc_BytesPerHostInputBuffer;
    int                pahsc_BytesPerUserInputBuffer;    /* native buffer size in bytes */
    /* Output -------------- */
    AudioDeviceID      pahsc_OutputAudioDeviceID;
    int                pahsc_BytesPerHostOutputBuffer;
    int                pahsc_BytesPerUserOutputBuffer;    /* native buffer size in bytes */
    /* Run Time -------------- */
    PaTimestamp        pahsc_FramesPlayed;
    long               pahsc_LastPosition;                /* used to track frames played. */
    /* For measuring CPU utilization. */
    // LARGE_INTEGER      pahsc_EntryCount;
    // LARGE_INTEGER      pahsc_LastExitCount;
    /* Init Time -------------- */
    int                pahsc_NumHostBuffers;
    int                pahsc_FramesPerHostBuffer;
    int                pahsc_UserBuffersPerHostBuffer;

    //   CRITICAL_SECTION   pahsc_StreamLock;                  /* Mutext to prevent threads from colliding. */
    int                pahsc_StreamLockInited;

}
PaHostSoundControl;

/************************************************* Shared Data ********/
/* FIXME - put Mutex around this shared data. */
static int sNumDevices = 0;
static PaDeviceInfo **sDevicePtrs = NULL;
static int sDefaultInputDeviceID = paNoDevice;
static int sDefaultOutputDeviceID = paNoDevice;
static int sPaHostError = 0;
static AudioDeviceID *sDeviceIDList;   // Array of AudioDeviceIDs

static const char sMapperSuffixInput[] = " - Input";
static const char sMapperSuffixOutput[] = " - Output";

/************************************************* Macros ********/

/* Convert external PA ID to an internal ID that includes WAVE_MAPPER */
#define PaDeviceIdToWinId(id) (((id) < sNumInputDevices) ? (id - 1) : (id - sNumInputDevices - 1))

/************************************************* Prototypes **********/

static PaError Pa_QueryDevices( void );
PaError PaHost_GetTotalBufferFrames( internalPortAudioStream   *past );
/********************************* BEGIN CPU UTILIZATION MEASUREMENT ****/
static void Pa_StartUsageCalculation( internalPortAudioStream   *past )
{
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return;
    /* Query system timer for usage analysis and to prevent overuse of CPU. */
    // QueryPerformanceCounter( &pahsc->pahsc_EntryCount );
}

static void Pa_EndUsageCalculation( internalPortAudioStream   *past )
{
#if 0
    // LARGE_INTEGER CurrentCount = { 0, 0 };
    // LONGLONG      InsideCount;
    // LONGLONG      TotalCount;
    /*
    ** Measure CPU utilization during this callback. Note that this calculation
    ** assumes that we had the processor the whole time.
    */
#define LOWPASS_COEFFICIENT_0   (0.9)
#define LOWPASS_COEFFICIENT_1   (0.99999 - LOWPASS_COEFFICIENT_0)

    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return;

    if( QueryPerformanceCounter( &CurrentCount ) )
    {
        if( past->past_IfLastExitValid )
        {
            InsideCount = CurrentCount.QuadPart - pahsc->pahsc_EntryCount.QuadPart;
            TotalCount =  CurrentCount.QuadPart - pahsc->pahsc_LastExitCount.QuadPart;
            /* Low pass filter the result because sometimes we get called several times in a row.
             * That can cause the TotalCount to be very low which can cause the usage to appear
             * unnaturally high. So we must filter numerator and denominator separately!!!
             */
            past->past_AverageInsideCount = (( LOWPASS_COEFFICIENT_0 * past->past_AverageInsideCount) +
                                             (LOWPASS_COEFFICIENT_1 * InsideCount));
            past->past_AverageTotalCount = (( LOWPASS_COEFFICIENT_0 * past->past_AverageTotalCount) +
                                            (LOWPASS_COEFFICIENT_1 * TotalCount));
            past->past_Usage = past->past_AverageInsideCount / past->past_AverageTotalCount;
        }
        pahsc->pahsc_LastExitCount = CurrentCount;
        past->past_IfLastExitValid = 1;
    }
#endif
}

static PaDeviceID Pa_QueryDefaultDevice( int deviceEnum )
{
    OSStatus err = noErr;
    UInt32  count;
    int          i;
    AudioDeviceID tempDevice = kAudioDeviceUnknown;
    PaDeviceID  defaultDeviceID;

    // get the default output device for the HAL
    // it is required to pass the size of the data to be returned
    count = sizeof(AudioDeviceID);
    err = AudioHardwareGetProperty( deviceEnum,  &count, (void *) &tempDevice);
    if (err != noErr) goto Bail;
    // find index of default device
    defaultDeviceID = paNoDevice;
    for( i=0; i<sNumDevices; i++ )
    {
        if( sDeviceIDList[i] == tempDevice )
        {
            defaultDeviceID = i;
            break;
        }
    }
Bail:
    return defaultDeviceID;
}

/****************************************** END CPU UTILIZATION *******/

static PaError Pa_QueryDevices( void )
{
    OSStatus err = noErr;
    UInt32   outSize;
    Boolean  outWritable;
    int         numBytes;

    // find out how many audio devices there are, if any
    err = AudioHardwareGetPropertyInfo(kAudioHardwarePropertyDevices, &outSize, &outWritable);
    if (err != noErr)
        ERR_RPT(("Couldn't get info about list of audio devices\n"));

    // calculate the number of device available
    sNumDevices = outSize / sizeof(AudioDeviceID);

    // Bail if there aren't any devices
    if (sNumDevices < 1)
        ERR_RPT(("No Devices Available\n"));

    // make space for the devices we are about to get
    sDeviceIDList = malloc(outSize);

    // get an array of AudioDeviceIDs
    err = AudioHardwareGetProperty(kAudioHardwarePropertyDevices, &outSize, (void *)sDeviceIDList);
    if (err != noErr)
        ERR_RPT(("Couldn't get list of audio device IDs\n"));

    /* Allocate structures to hold device info pointers. */
    numBytes = sNumDevices * sizeof(PaDeviceInfo *);
    sDevicePtrs = PaHost_AllocateFastMemory( numBytes );
    if( sDevicePtrs == NULL ) return paInsufficientMemory;

    sDefaultInputDeviceID = Pa_QueryDefaultDevice( kAudioHardwarePropertyDefaultInputDevice );
    sDefaultOutputDeviceID = Pa_QueryDefaultDevice( kAudioHardwarePropertyDefaultOutputDevice );

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

/*************************************************************************/

/* Allocate a string containing the device name. */
char *deviceNameFromID(AudioDeviceID deviceID )
{
    OSStatus err = noErr;
    UInt32  outSize;
    Boolean  outWritable;
    char     *deviceName = nil;
    // query size of name
    err =  AudioDeviceGetPropertyInfo(deviceID, 0, false, kAudioDevicePropertyDeviceName, &outSize, &outWritable);
    if (err == noErr)
    {
        deviceName = malloc( outSize + 1);
        if( deviceName )
        {
            err = AudioDeviceGetProperty(deviceID, 0, false, kAudioDevicePropertyDeviceName, &outSize, deviceName);
            // FIXME if (err == noErr)
        }
    }

    return deviceName;
}

/*************************************************************************/

void deviceDataFormatFromID(AudioDeviceID deviceID , AudioStreamBasicDescription *desc )
{
    OSStatus err = noErr;
    UInt32  outSize;

    outSize = sizeof(*desc);
    err = AudioDeviceGetProperty(deviceID, 0, false, kAudioDevicePropertyStreamFormat, &outSize, desc);
}

/*************************************************************************/
// An AudioStreamBasicDescription is passed in to query whether or not
// the format is supported. A kAudioDeviceUnsupportedFormatError will
// be returned if the format is not supported and kAudioHardwareNoError
// will be returned if it is supported. AudioStreamBasicDescription
// fields set to 0 will be ignored in the query, but otherwise values
// must match exactly.

Boolean deviceDoesSupportFormat(AudioDeviceID deviceID , AudioStreamBasicDescription *desc )
{
    OSStatus err = noErr;
    UInt32  outSize;

    outSize = sizeof(*desc);
    err = AudioDeviceGetProperty(deviceID, 0, false, kAudioDevicePropertyStreamFormatSupported, &outSize, desc);

    if (err == kAudioHardwareNoError)
        return true;
    else
        return false;
}

/*************************************************************************/
// return an error string
char* coreAudioErrorString (int errCode )
{
    char *str;

    switch (errCode)
    {
    case kAudioHardwareUnspecifiedError:
        str = "kAudioHardwareUnspecifiedError";
        break;
    case kAudioHardwareNotRunningError:
        str = "kAudioHardwareNotRunningError";
        break;
    case kAudioHardwareUnknownPropertyError:
        str = "kAudioHardwareUnknownPropertyError";
        break;
    case kAudioDeviceUnsupportedFormatError:
        str = "kAudioDeviceUnsupportedFormatError";
        break;
    case kAudioHardwareBadPropertySizeError:
        str = "kAudioHardwareBadPropertySizeError";
        break;
    case kAudioHardwareIllegalOperationError:
        str = "kAudioHardwareIllegalOperationError";
        break;
    default:
        str = "UNKNWON ERROR!";
        break;
    }

    return str;
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
#define NUM_CUSTOMSAMPLINGRATES     4   /* must be the same number of elements as in the array below */
#define MAX_NUMSAMPLINGRATES        (NUM_STANDARDSAMPLINGRATES+NUM_CUSTOMSAMPLINGRATES)

    PaDeviceInfo *deviceInfo;
    AudioDeviceID    devID;
    double *sampleRates; /* non-const ptr */
    double possibleSampleRates[] = {8000.0, 11025.0, 22050.0, 44100.0, 48000.0, 88200.0, 96000.0};
    int index;
    AudioStreamBasicDescription formatDesc;
    Boolean result;

    if( id < 0 || id >= sNumDevices )
        return NULL;

    if( sDevicePtrs[ id ] != NULL )
    {
        return sDevicePtrs[ id ];
    }

    deviceInfo = PaHost_AllocateFastMemory( sizeof(PaDeviceInfo) );
    if( deviceInfo == NULL ) return NULL;

    deviceInfo->structVersion = 1;
    deviceInfo->maxInputChannels = 0;
    deviceInfo->maxOutputChannels = 0;
    deviceInfo->numSampleRates = -1;


    // fill in format descriptor
    if( id < sNumDevices )
    {
        devID = sDeviceIDList[ id ];

        // Get the device name
        deviceInfo->name = deviceNameFromID( devID );

        // Figure out supported sample rates
        // Make room in case device supports all rates.
        sampleRates = (double*)PaHost_AllocateFastMemory( MAX_NUMSAMPLINGRATES * sizeof(double) );
        deviceInfo->sampleRates = sampleRates;
        deviceInfo->numSampleRates = 0;

        // Loop through the possible sampling rates and check each to see if the device supports it.
        for (index = 0; index < MAX_NUMSAMPLINGRATES; index ++)
        {
            memset( &formatDesc, 0, sizeof(AudioStreamBasicDescription) );
            formatDesc.mSampleRate = possibleSampleRates[index];
            result = deviceDoesSupportFormat( devID, &formatDesc );

            if (result == true)
            {
                deviceInfo->numSampleRates += 1;
                *sampleRates = possibleSampleRates[index];
                sampleRates++;
            }
        }

        // Get data format info from the device.
        deviceDataFormatFromID(devID, &formatDesc);

        deviceInfo->maxInputChannels = 0; // FIXME - Input and Output are separate devices!
        deviceInfo->maxOutputChannels = formatDesc.mChannelsPerFrame;

        // FIXME - where to put current sample rate?:  formatDesc.mSampleRate

        // Right now the Core Audio headers only define one formatID: LinearPCM
        // Apparently LinearPCM must be Float32 for now.
        // FIXME -
        switch (formatDesc.mFormatID)
        {
        case kAudioFormatLinearPCM:
            deviceInfo->nativeSampleFormats = paFloat32;

            // FIXME - details about the format are in these flags.
            // formatDesc.mFormatFlags

            // here are the possibilities
            // kLinearPCMFormatFlagIsFloat   // set for floating point, clear for integer
            // kLinearPCMFormatFlagIsBigEndian  // set for big endian, clear for little
            // kLinearPCMFormatFlagIsSignedInteger // set for signed integer, clear for unsigned integer,
            //    only valid if kLinearPCMFormatFlagIsFloat is clear
            // kLinearPCMFormatFlagIsPacked   // set if the sample bits are packed as closely together as possible,
            //    clear if they are high or low aligned within the channel
            // kLinearPCMFormatFlagIsAlignedHigh  // set if the sample bits are placed

            break;
        default:
            deviceInfo->nativeSampleFormats = paFloat32;  // FIXME
            break;
        }

    }

    sDevicePtrs[ id ] = deviceInfo;
    return deviceInfo;

    /* FIXME
    error:
        free( sampleRates );
        free( deviceInfo );
    */

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
#if 0
    UInt32   hresult;
    char    envbuf[PA_ENV_BUF_SIZE];
    PaDeviceID recommendedID = paNoDevice;

    /* Let user determine default device by setting environment variable. */
    hresult = GetEnvironmentVariable( envName, envbuf, PA_ENV_BUF_SIZE );
    if( (hresult > 0) && (hresult < PA_ENV_BUF_SIZE) )
    {
        recommendedID = atoi( envbuf );
    }

    return recommendedID;
#endif
    return paNoDevice;
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
#if PA_SIMULATE_UNDERFLOW
    PRINT(("WARNING - Underflow Simulation Enabled - Expect a Big Glitch!!!\n"));
#endif
    return Pa_MaybeQueryDevices();
}

/**********************************************************************
** Fill any available output buffers and use any available
** input buffers by calling user callback.
*/
static PaError Pa_TimeSlice( internalPortAudioStream   *past, const AudioBufferList*  inInputData,
                             AudioBufferList*  outOutputData )
{
    PaError           result = 0;
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

#if PA_SIMULATE_UNDERFLOW
    if(gUnderCallbackCounter++ == UNDER_SLEEP_AT)
    {
        Sleep(UNDER_SLEEP_FOR);
    }
#endif

#if PA_TRACE_RUN
    AddTraceMessage("Pa_TimeSlice: past_NumCallbacks ", past->past_NumCallbacks );
#endif

    Pa_StartUsageCalculation( past );

    /* If we are using output, then we need an empty output buffer. */
    gotOutput = 0;
    if( past->past_NumOutputChannels > 0 )
    {
        outBufPtr =  outOutputData->mBuffers[0].mData,
                     gotOutput = 1;
    }

    /* If we are using input, then we need a full input buffer. */
    gotInput = 0;
    inBufPtr = NULL;
    if(  past->past_NumInputChannels > 0  )
    {
        inBufPtr = inInputData->mBuffers[0].mData,
                   gotInput = 1;
    }

    buffersProcessed += 1;

    /* Each host buffer contains multiple user buffers so do them all now. */
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
            result = Pa_CallConvertFloat32( past, (float *) inBufPtr, (float *) outBufPtr );
            if( result != 0) done = 1;
        }
        if( gotInput ) inBufPtr += pahsc->pahsc_BytesPerUserInputBuffer;
        if( gotOutput) outBufPtr += pahsc->pahsc_BytesPerUserOutputBuffer;
    }

    Pa_EndUsageCalculation( past );

#if PA_TRACE_RUN
    AddTraceMessage("Pa_TimeSlice: buffersProcessed ", buffersProcessed );
#endif

    return (result != 0) ? result : done;
}

OSStatus appIOProc (AudioDeviceID  inDevice, const AudioTimeStamp*  inNow, const AudioBufferList*  inInputData, const AudioTimeStamp*  inInputTime, AudioBufferList*  outOutputData, const AudioTimeStamp* inOutputTime, void* contextPtr)
{

    PaError      result = 0;
    internalPortAudioStream *past;
    PaHostSoundControl *pahsc;
    past = (internalPortAudioStream *) contextPtr;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;

    // Just Curious: printf("Num input Buffers: %d; Num output Buffers: %d.\n", inInputData->mNumberBuffers, outOutputData->mNumberBuffers);

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
        // FIXME - pretend all done
        past->past_IsActive = 0; /* Will cause thread to return. */
    }
    else
    {
        /* Process full input buffer and fill up empty output buffers. */
        if( (result = Pa_TimeSlice( past, inInputData, outOutputData )) != 0)
        {
            /* User callback has asked us to stop. */
#if PA_TRACE_START_STOP
            AddTraceMessage( "Pa_OutputThreadProc: TimeSlice() returned ", result );
#endif
            past->past_StopSoon = 1; /* Request that audio play out then stop. */
            result = paNoError;
        }
    }

    // FIXME PaHost_UpdateStreamTime( pahsc );

    return result;
}

static int PaHost_CalcTimeOut( internalPortAudioStream *past )
{
    /* Calculate timeOut longer than longest time it could take to play all buffers. */
    int timeOut = (UInt32) (1500.0 * PaHost_GetTotalBufferFrames( past ) / past->past_SampleRate);
    if( timeOut < MIN_TIMEOUT_MSEC ) timeOut = MIN_TIMEOUT_MSEC;
    return timeOut;
}

/*******************************************************************/
PaError PaHost_OpenInputStream( internalPortAudioStream   *past )
{
    PaError          result = paNoError;
    PaHostSoundControl *pahsc;
    int              bytesPerInputFrame;
    const PaDeviceInfo  *pad;

    pahsc = (PaHostSoundControl *) past->past_DeviceData;

    DBUG(("PaHost_OpenStream: deviceID = 0x%x\n", past->past_InputDeviceID));
    pad = Pa_GetDeviceInfo( past->past_InputDeviceID );
    if( pad == NULL ) return paInternalError;

    bytesPerInputFrame = Pa_GetSampleSize(pad->nativeSampleFormats) * past->past_NumInputChannels;
    pahsc->pahsc_BytesPerUserInputBuffer = past->past_FramesPerUserBuffer * bytesPerInputFrame;
    pahsc->pahsc_BytesPerHostInputBuffer = pahsc->pahsc_UserBuffersPerHostBuffer * pahsc->pahsc_BytesPerUserInputBuffer;
    // FIXME
    return result;
}

/*******************************************************************/
PaError PaHost_OpenOutputStream( internalPortAudioStream   *past )
{
    PaError          result = paNoError;
    PaHostSoundControl *pahsc;
    const PaDeviceInfo *pad;
    UInt32           bytesPerHostBuffer;
    UInt32   dataSize;
    UInt32   hardwareLatency = -1;
    Boolean   writeable = false;
    OSStatus         err = noErr;
    int              bytesPerOutputFrame;

    pahsc = (PaHostSoundControl *) past->past_DeviceData;

    DBUG(("PaHost_OpenStream: deviceID = 0x%x\n", past->past_OutputDeviceID));
    pad = Pa_GetDeviceInfo( past->past_OutputDeviceID );
    if( pad == NULL ) return paInternalError;

    pahsc->pahsc_OutputAudioDeviceID = sDeviceIDList[past->past_OutputDeviceID];

    bytesPerOutputFrame = Pa_GetSampleSize(pad->nativeSampleFormats) * past->past_NumOutputChannels;
    pahsc->pahsc_BytesPerUserOutputBuffer = past->past_FramesPerUserBuffer * bytesPerOutputFrame;
    pahsc->pahsc_BytesPerHostOutputBuffer = pahsc->pahsc_UserBuffersPerHostBuffer * pahsc->pahsc_BytesPerUserOutputBuffer;

    // FIXME - force host buffer to user size

    // change the bufferSize of the device
    bytesPerHostBuffer = past->past_OutputBufferSize; // FIXME
    dataSize = sizeof(UInt32);
    err = AudioDeviceSetProperty( pahsc->pahsc_OutputAudioDeviceID, 0, 0, false,
                                  kAudioDevicePropertyBufferSize, dataSize, &bytesPerHostBuffer);
    if( err != noErr )
    {
        ERR_RPT(("Could not force buffer size!"));
        result = paHostError;
        goto error;
    }

    // FIXME -- this isn't working for some reason.  I can't figure out what the error code is.
    dataSize = sizeof(UInt32);
    err = AudioDeviceGetProperty(pahsc->pahsc_OutputAudioDeviceID, 0, false, kAudioDevicePropertyLatency, &dataSize, &writeable);
    if( err != noErr )
    {
        PRINT(("A CoreAudio error occurred: %s.\n", coreAudioErrorString( err ) ));
    }

    err = AudioDeviceGetProperty(pahsc->pahsc_OutputAudioDeviceID, 0, false, kAudioDevicePropertyLatency, &dataSize, (void *)&hardwareLatency);
    if ( err != noErr )
    {
        PRINT(("A CoreAudio error occurred: %s.\n", coreAudioErrorString( err ) ));
    }
    else
    {
        PRINT(("Output Stream Hardware Latency = %d frames.\n", hardwareLatency ));
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
void PaHost_CalcNumHostBuffers( internalPortAudioStream *past )
{
    PaHostSoundControl *pahsc = past->past_DeviceData;
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
    pahsc = (PaHostSoundControl *) malloc(sizeof(PaHostSoundControl));
    if( pahsc == NULL )
    {
        result = paInsufficientMemory;
        goto error;
    }
    memset( pahsc, 0, sizeof(PaHostSoundControl) );
    past->past_DeviceData = (void *) pahsc;

    /* Figure out how user buffers fit into WAVE buffers. */
    // FIXME - just force for now

    pahsc->pahsc_UserBuffersPerHostBuffer = 1;
    pahsc->pahsc_FramesPerHostBuffer = past->past_FramesPerUserBuffer;
    pahsc->pahsc_NumHostBuffers = 2; // FIXME - dunno?!

    {
        int msecLatency = (int) ((PaHost_GetTotalBufferFrames(past) * 1000) / past->past_SampleRate);
        PRINT(("PortAudio on OSX - Latency = %d frames, %d msec\n", PaHost_GetTotalBufferFrames(past), msecLatency ));
    }

    /* ------------------ OUTPUT */
    if( (past->past_OutputDeviceID != paNoDevice) && (past->past_NumOutputChannels > 0) )
    {
        result = PaHost_OpenOutputStream( past );
        if( result < 0 ) goto error;
    }

    /* ------------------ INPUT */
    if( (past->past_InputDeviceID != paNoDevice) && (past->past_NumInputChannels > 0) )
    {
        result = PaHost_OpenInputStream( past );
        if( result < 0 ) goto error;
    }

    return result;

error:
    PaHost_CloseStream( past );
    return result;
}

/*************************************************************************/
PaError PaHost_StartOutput( internalPortAudioStream *past )
{
    PaHostSoundControl *pahsc;
    PaError          result = paNoError;
    OSStatus  err = noErr;


    pahsc = (PaHostSoundControl *) past->past_DeviceData;

    if( past->past_OutputDeviceID != paNoDevice )
    {
        // Associate an IO proc with the device and pass a pointer to the audio data context
        err = AudioDeviceAddIOProc(pahsc->pahsc_OutputAudioDeviceID, (AudioDeviceIOProc)appIOProc, past);
        if (err != noErr) goto error;

        // start playing sound through the device
        err = AudioDeviceStart(pahsc->pahsc_OutputAudioDeviceID, (AudioDeviceIOProc)appIOProc);
        if (err != noErr) goto error;
    }

error:
    return result;
}

/*************************************************************************/
PaError PaHost_StartInput( internalPortAudioStream *past )
{
    PaError          result = paNoError;
    PaHostSoundControl *pahsc;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;

    if( past->past_InputDeviceID != paNoDevice )
    {
        // FIXME
    }
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

    timeOut = PaHost_CalcTimeOut( past );
    past->past_IsActive = 0;

    return paNoError;
}

/*************************************************************************/
PaError PaHost_StopInput( internalPortAudioStream *past, int abort )
{
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return paNoError;
    (void) abort;

    // FIXME
    return paNoError;
}

/*************************************************************************/
PaError PaHost_StopOutput( internalPortAudioStream *past, int abort )
{
    PaHostSoundControl *pahsc;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return paNoError;
    (void) abort;

#if PA_TRACE_START_STOP
    AddTraceMessage( "PaHost_StopOutput: pahsc_HWaveOut ", (int) pahsc->pahsc_HWaveOut );
#endif

    // FIXME   if( pahsc->pahsc_OutputAudioDeviceID != ??? )
    {
        OSStatus  err = noErr;

        err = AudioDeviceStop(pahsc->pahsc_OutputAudioDeviceID, (AudioDeviceIOProc)appIOProc);
        if (err != noErr) goto Bail;

        err = AudioDeviceRemoveIOProc(pahsc->pahsc_OutputAudioDeviceID, (AudioDeviceIOProc)appIOProc);
        if (err != noErr) goto Bail;
    }
Bail:
    return paNoError;
}

/*******************************************************************/
PaError PaHost_CloseStream( internalPortAudioStream   *past )
{
    PaHostSoundControl *pahsc;

    if( past == NULL ) return paBadStreamPtr;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return paNoError;

#if PA_TRACE_START_STOP
    AddTraceMessage( "PaHost_CloseStream: pahsc_HWaveOut ", (int) pahsc->pahsc_HWaveOut );
#endif
    /* Free data and device for output. */

    /* Free data and device for input. */

    free( pahsc );
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
    int       minLatencyMsec = 0;
    double    msecPerBuffer = (1000.0 * framesPerBuffer) / sampleRate;
    int       minBuffers;

    /* Let user determine minimal latency by setting environment variable. */
    {
        minLatencyMsec = PA_MIN_LATENCY_MSEC;
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

                    free( (char*)sDevicePtrs[i]->name );
                    free( (void*)sDevicePtrs[i]->sampleRates );
                    free( sDevicePtrs[i] );
                }
            }

            free( sDevicePtrs );
            sDevicePtrs = NULL;
        }

        sNumDevices = 0;
    }
    return paNoError;
}

/*************************************************************************/
void Pa_Sleep( long msec )
{
    /*       struct timeval timeout;
           timeout.tv_sec = msec / 1000;
           timeout.tv_usec = (msec % 1000) * 1000;
           select( 0, NULL, NULL, NULL, &timeout );
    */
    usleep( msec * 1000 );
}

/*************************************************************************
 * Allocate memory that can be accessed in real-time.
 * This may need to be held in physical memory so that it is not
 * paged to virtual memory.
 * This call MUST be balanced with a call to PaHost_FreeFastMemory().
 */
void *PaHost_AllocateFastMemory( long numBytes )
{
    void *addr = malloc( numBytes ); /* FIXME - do we need physical memory? */
    if( addr != NULL ) memset( addr, 0, numBytes );
    return addr;
}

/*************************************************************************
 * Free memory that could be accessed in real-time.
 * This call MUST be balanced with a call to PaHost_AllocateFastMemory().
 */
void PaHost_FreeFastMemory( void *addr, long numBytes )
{
    if( addr != NULL ) free( addr );
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

/*************************************************************************/
PaTimestamp Pa_StreamTime( PortAudioStream *stream )
{
    PaHostSoundControl *pahsc;
    internalPortAudioStream   *past = (internalPortAudioStream *) stream;
    if( past == NULL ) return paBadStreamPtr;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;

    // FIXME PaHost_UpdateStreamTime( pahsc );
    return pahsc->pahsc_FramesPlayed;
}
