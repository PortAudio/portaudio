#ifndef PA_HOST_H
#define PA_HOST_H

/*
 * $Id$
 * Host dependant internal API for PortAudio
 *
 * Author: Phil Burk  <philburk@softsynth.com>
 *
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.softsynth.com/portaudio/
 * DirectSound and Macintosh Implementation
 * Copyright (c) 1999-2000 Phil Burk
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

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

#ifndef SUPPORT_AUDIO_CAPTURE
#define SUPPORT_AUDIO_CAPTURE  (1)
#endif

#ifndef int32
    typedef long int32;
#endif
#ifndef uint32
    typedef unsigned long uint32;
#endif
#ifndef int16
    typedef short int16;
#endif
#ifndef uint16
    typedef unsigned short uint16;
#endif

#define PA_MAGIC    (0x18273645)

/************************************************************************************/
/****************** Structures ******************************************************/
/************************************************************************************/

typedef struct internalPortAudioStream
{
    uint32                    past_Magic;  /* ID for struct to catch bugs. */
    /* User specified information. */
    uint32                    past_FramesPerUserBuffer;
    uint32                    past_NumUserBuffers;
    double                    past_SampleRate;     /* Closest supported sample rate. */
    int                       past_NumInputChannels;
    int                       past_NumOutputChannels;
    PaDeviceID                past_InputDeviceID;
    PaDeviceID                past_OutputDeviceID;
    PaSampleFormat            past_InputSampleFormat;
    PaSampleFormat            past_OutputSampleFormat;
    void                     *past_DeviceData;
    PortAudioCallback        *past_Callback;
    void                     *past_UserData;
    uint32                    past_Flags;
    /* Flags for communicating between foreground and background. */
    volatile int              past_IsActive;      /* Background is still playing. */
    volatile int              past_StopSoon;      /* Background should keep playing when buffers empty. */
    volatile int              past_StopNow;       /* Background should stop playing now. */
    /* These buffers are used when the native format does not match the user format. */
    void                     *past_InputBuffer;
    uint32                    past_InputBufferSize;
    void                     *past_OutputBuffer;
    uint32                    past_OutputBufferSize;
    /* Measurements */
    uint32                    past_NumCallbacks;
    PaTimestamp               past_FrameCount;    /* Frames output to buffer. */
    /* For measuring CPU utilization. */
    double                    past_AverageInsideCount;
    double                    past_AverageTotalCount;
    double                    past_Usage;
    int                       past_IfLastExitValid;
}
internalPortAudioStream;

/************************************************************************************/
/****************** Prototypes ******************************************************/
/************************************************************************************/

PaError PaHost_Init( void );
PaError PaHost_Term( void );

PaError PaHost_OpenStream( internalPortAudioStream   *past );
PaError PaHost_CloseStream( internalPortAudioStream   *past );

PaError PaHost_StartOutput( internalPortAudioStream   *past );
PaError PaHost_StopOutput( internalPortAudioStream   *past, int abort );
PaError PaHost_StartInput( internalPortAudioStream   *past );
PaError PaHost_StopInput( internalPortAudioStream   *past, int abort );
PaError PaHost_StartEngine( internalPortAudioStream   *past );
PaError PaHost_StopEngine( internalPortAudioStream *past, int abort );
PaError PaHost_StreamActive( internalPortAudioStream   *past );

long Pa_CallConvertInt16( internalPortAudioStream   *past,
                          short *nativeInputBuffer,
                          short *nativeOutputBuffer );

long Pa_CallConvertFloat32( internalPortAudioStream   *past,
                            float *nativeInputBuffer,
                            float *nativeOutputBuffer );

void   *PaHost_AllocateFastMemory( long numBytes );
void    PaHost_FreeFastMemory( void *addr, long numBytes );

PaError PaHost_ValidateSampleRate( PaDeviceID id, double requestedFrameRate,
                                   double *closestFrameRatePtr );
int PaHost_FindClosestTableEntry( double allowableError,  const double *rateTable,
                                  int numRates, double frameRate );

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PA_HOST_H */
