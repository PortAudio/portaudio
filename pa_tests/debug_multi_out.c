/*
 * $Id$
 * debug_multi.c
 * Play a sine wave on each of multiple channels,
 * using the Portable Audio api.
 * Hacked test for debugging PA.
 *
 * Author: Phil Burk  http://www.softsynth.com
 *
 * This program uses the PortAudio Portable Audio Library.
 * For more information see: http://www.portaudio.com
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
 */
#include <stdio.h>
#include <math.h>
#include "portaudio.h"
#define NUM_CHANNELS        (8)
// #define OUTPUT_DEVICE       (Pa_GetDefaultOutputDeviceID())
#define OUTPUT_DEVICE       (18)
#define NUM_SECONDS         (NUM_CHANNELS*4)
#define SAMPLE_RATE         (44100)
#define FRAMES_PER_CHANNEL  (SAMPLE_RATE/2)
#define FRAMES_PER_BUFFER   (256)
#define MIN_LATENCY_MSEC    (400)
#define NUM_BUFFERS         ((MIN_LATENCY_MSEC * SAMPLE_RATE) / (FRAMES_PER_BUFFER * 1000))
#ifndef M_PI
#define M_PI  (3.14159265)
#endif
#define TABLE_SIZE   (800)
typedef struct
{
    float sine[TABLE_SIZE];
    int   phase;
    int   liveChannel;
    int   count;
    unsigned int sampsToGo;
}
paTestData;
/* This routine will be called by the PortAudio engine when audio is needed.
** It may called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/
static int patestCallback( void *inputBuffer, void *outputBuffer,
                           unsigned long framesPerBuffer,
                           PaTimestamp outTime, void *userData )
{
    paTestData *data = (paTestData*)userData;
    float *out = (float*)outputBuffer;
    int i, j;
    int finished = 0;
    (void) outTime; /* Prevent unused variable warnings. */
    (void) inputBuffer;

    if( data->sampsToGo < framesPerBuffer )
    {
        finished = 1;
    }
    else
    {
        for( i=0; i<(int)framesPerBuffer; i++ )
        {
            for( j=0; j<NUM_CHANNELS; j++ )
            {
                /* Output sine wave only on live channel. */
                *out++ = (j==data->liveChannel) ? data->sine[data->phase] : 0.0f;
                /* Play each channel at a higher frequency. */
                data->phase += 1 + data->liveChannel;
                if( data->phase >= TABLE_SIZE ) data->phase -= TABLE_SIZE;
            }
            /* Switch channels every so often. */
            if( --data->count <= 0 )
            {
                data->count = FRAMES_PER_CHANNEL;
                data->liveChannel += 1;
                if( data->liveChannel >= NUM_CHANNELS ) data->liveChannel = 0;
            }
        }
        data->sampsToGo -= framesPerBuffer;
    }
    return finished;
}
/*******************************************************************/
int main(void);
int main(void)
{
    PortAudioStream *stream;
    PaError err;
    paTestData data;
    int i;
    int totalSamps;
    printf("PortAudio Test: output sine wave. %d buffers\n", NUM_BUFFERS );
    /* initialise sinusoidal wavetable */
    for( i=0; i<TABLE_SIZE; i++ )
    {
        data.sine[i] = (float) sin( ((double)i/(double)TABLE_SIZE) * M_PI * 2. );
    }
    data.phase = 0;
    data.count = FRAMES_PER_CHANNEL;
    data.liveChannel = 0;
    data.sampsToGo = totalSamps =  NUM_SECONDS * SAMPLE_RATE; /* Play for a few seconds. */
    err = Pa_Initialize();
    if( err != paNoError ) goto error;
    err = Pa_OpenStream(
              &stream,
              paNoDevice, /* default input device */
              0,              /* no input */
              paFloat32,  /* 32 bit floating point input */
              NULL,
              OUTPUT_DEVICE,
              NUM_CHANNELS,
              paFloat32,  /* 32 bit floating point output */
              NULL,
              SAMPLE_RATE,
              FRAMES_PER_BUFFER,  /* frames per buffer */
              NUM_BUFFERS,    /* number of buffers, if zero then use default minimum */
              paClipOff,      /* we won't output out of range samples so don't bother clipping them */
              patestCallback,
              &data );
    if( err != paNoError ) goto error;
    err = Pa_StartStream( stream );
    if( err != paNoError ) goto error;
    printf("Is callback being called?\n");
    for( i=0; i<NUM_SECONDS; i++ )
    {
        printf("data.sampsToGo = %d\n", data.sampsToGo );
        Pa_Sleep( 1000 );
    }
    /* Stop sound until ENTER hit. */
    err = Pa_StopStream( stream );
    if( err != paNoError ) goto error;
    Pa_CloseStream( stream );
    Pa_Terminate();
    printf("Test finished.\n");
    return err;
error:
    Pa_Terminate();
    fprintf( stderr, "An error occured while using the portaudio stream\n" );
    fprintf( stderr, "Error number: %d\n", err );
    fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
    return err;
}
