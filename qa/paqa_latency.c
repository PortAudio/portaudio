/** @file paqa_latency.c
	@ingroup qa_src
	@brief Test latency estimates.
	@author Ross Bencina <rossb@audiomulch.com>
    @author Phil Burk <philburk@softsynth.com>
*/
/*
 * $Id: patest_sine.c 1368 2008-03-01 00:38:27Z rossb $
 *
 * This program uses the PortAudio Portable Audio Library.
 * For more information see: http://www.portaudio.com/
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
#include <stdio.h>
#include <math.h>
#include "portaudio.h"

#define NUM_SECONDS   (5)
#define SAMPLE_RATE   (44100)
#define FRAMES_PER_BUFFER  (64)

#ifndef M_PI
#define M_PI  (3.14159265)
#endif

#define TABLE_SIZE   (200)
typedef struct
{
    float sine[TABLE_SIZE];
    int left_phase;
    int right_phase;
    char message[20];
    int minFramesPerBuffer;
    int maxFramesPerBuffer;
    int callbackCount;
    PaTime minDeltaDacTime;
    PaTime maxDeltaDacTime;
    PaStreamCallbackTimeInfo previousTimeInfo;
}
paTestData;

/* This routine will be called by the PortAudio engine when audio is needed.
** It may called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/
static int patestCallback( const void *inputBuffer, void *outputBuffer,
                            unsigned long framesPerBuffer,
                            const PaStreamCallbackTimeInfo* timeInfo,
                            PaStreamCallbackFlags statusFlags,
                            void *userData )
{
    paTestData *data = (paTestData*)userData;
    float *out = (float*)outputBuffer;
    unsigned long i;

    (void) timeInfo; /* Prevent unused variable warnings. */
    (void) statusFlags;
    (void) inputBuffer;
    
    if( data->minFramesPerBuffer > framesPerBuffer )
    {
        data->minFramesPerBuffer = framesPerBuffer;
    }
    if( data->maxFramesPerBuffer < framesPerBuffer )
    {
        data->maxFramesPerBuffer = framesPerBuffer;
    }
    
    // Measure min and max output time stamp delta.
    if( data->callbackCount > 0 )
    {
        PaTime delta = timeInfo->outputBufferDacTime - data->previousTimeInfo.outputBufferDacTime;
        if( data->minDeltaDacTime > delta )
        {
            data->minDeltaDacTime = delta;
        }
        if( data->maxDeltaDacTime < delta )
        {
            data->maxDeltaDacTime = delta;
        }
    }
    data->previousTimeInfo = *timeInfo;
    
    for( i=0; i<framesPerBuffer; i++ )
    {
        *out++ = data->sine[data->left_phase];  /* left */
        *out++ = data->sine[data->right_phase];  /* right */
        data->left_phase += 1;
        if( data->left_phase >= TABLE_SIZE ) data->left_phase -= TABLE_SIZE;
        data->right_phase += 3; /* higher pitch so we can distinguish left and right. */
        if( data->right_phase >= TABLE_SIZE ) data->right_phase -= TABLE_SIZE;
    }
    
    data->callbackCount += 1;
    return paContinue;
}

PaError paqaCheckLatency( PaStreamParameters *outputParamsPtr, 
                         paTestData *dataPtr, double sampleRate, unsigned long framesPerBuffer )
{
    PaError err;
    PaStream *stream;
    const PaStreamInfo* streamInfo;

    dataPtr->minFramesPerBuffer = 9999999;
    dataPtr->maxFramesPerBuffer = 0;
    dataPtr->minDeltaDacTime = 9999999.0;
    dataPtr->maxDeltaDacTime = 0.0;
    dataPtr->callbackCount = 0;
    
    printf("Stream parameter: suggestedOutputLatency = %g\n", outputParamsPtr->suggestedLatency );
    if( framesPerBuffer == paFramesPerBufferUnspecified ){
        printf("Stream parameter: user framesPerBuffer = paFramesPerBufferUnspecified\n" );
    }else{
        printf("Stream parameter: user framesPerBuffer = %lu\n", framesPerBuffer );
    }
    err = Pa_OpenStream(
                        &stream,
                        NULL, /* no input */
                        outputParamsPtr,
                        sampleRate,
                        framesPerBuffer,
                        paClipOff,      /* we won't output out of range samples so don't bother clipping them */
                        patestCallback,
                        dataPtr );
    if( err != paNoError ) goto error1;
    
    streamInfo = Pa_GetStreamInfo( stream );
    printf("Stream info: inputLatency  = %g\n", streamInfo->inputLatency );
    printf("Stream info: outputLatency = %g\n", streamInfo->outputLatency );

    err = Pa_StartStream( stream );
    if( err != paNoError ) goto error2;

    printf("Play for %d seconds.\n", NUM_SECONDS );
    Pa_Sleep( NUM_SECONDS * 1000 );
    
    printf("  minFramesPerBuffer = %4d\n", dataPtr->minFramesPerBuffer );
    printf("  maxFramesPerBuffer = %4d\n", dataPtr->maxFramesPerBuffer );
    printf("  minDeltaDacTime = %f\n", dataPtr->minDeltaDacTime );
    printf("  maxDeltaDacTime = %f\n", dataPtr->maxDeltaDacTime );

    err = Pa_StopStream( stream );
    if( err != paNoError ) goto error2;

    err = Pa_CloseStream( stream );
    Pa_Sleep( 1 * 1000 );

    
    printf("-------------------------------------\n");
    return err;
error2:
    Pa_CloseStream( stream );
error1:
    printf("-------------------------------------\n");
    return err;
}


/*******************************************************************/
int main(void);
int main(void)
{
    PaStreamParameters outputParameters;
    PaError err;
    paTestData data;
    const PaDeviceInfo *deviceInfo;
    int i;
    int framesPerBuffer;
    double sampleRate = 44100;

    
    printf("PortAudio QA: investigate output latency. SR = %d, BufSize = %d\n", SAMPLE_RATE, FRAMES_PER_BUFFER);
    
    /* initialise sinusoidal wavetable */
    for( i=0; i<TABLE_SIZE; i++ )
    {
        data.sine[i] = (float) sin( ((double)i/(double)TABLE_SIZE) * M_PI * 2. );
    }
    data.left_phase = data.right_phase = 0;
    
    err = Pa_Initialize();
    if( err != paNoError ) goto error;

    outputParameters.device = Pa_GetDefaultOutputDevice(); /* default output device */
    if (outputParameters.device == paNoDevice) {
      fprintf(stderr,"Error: No default output device.\n");
      goto error;
    }

    outputParameters.channelCount = 2;       /* stereo output */
    outputParameters.sampleFormat = paFloat32; /* 32 bit floating point output */
    deviceInfo = Pa_GetDeviceInfo( outputParameters.device );
    printf("Using device #%d: '%s' (%s)\n", outputParameters.device, deviceInfo->name, Pa_GetHostApiInfo(deviceInfo->hostApi)->name);
    printf("Device info: defaultLowOutputLatency  = %f seconds\n", deviceInfo->defaultLowOutputLatency);
    printf("Device info: defaultHighOutputLatency = %f seconds\n", deviceInfo->defaultHighOutputLatency);
    outputParameters.hostApiSpecificStreamInfo = NULL;
    
    
    // Try to use a small buffer that is smaller than we think the device can handle.
    // Try to force combining multiple user buffers into a host buffer.
    printf("------------- Try a very small buffer.\n");
    framesPerBuffer = 9;
    outputParameters.suggestedLatency = deviceInfo->defaultLowOutputLatency;
    err = paqaCheckLatency( &outputParameters, &data, sampleRate, framesPerBuffer );
    if( err != paNoError ) goto error;
    
    printf("------------- 64 frame buffer with 1.1 * defaultLow latency.\n");
    framesPerBuffer = 64;
    outputParameters.suggestedLatency = deviceInfo->defaultLowOutputLatency * 1.1;
    err = paqaCheckLatency( &outputParameters, &data, sampleRate, framesPerBuffer );
    if( err != paNoError ) goto error;

    // Try to create a huge buffer that is bigger than the allowed device maximum.
    printf("------------- Try a huge buffer.\n");
    framesPerBuffer = 16*1024;
    outputParameters.suggestedLatency = ((double)framesPerBuffer) / sampleRate; // approximate
    err = paqaCheckLatency( &outputParameters, &data, sampleRate, framesPerBuffer );
    if( err != paNoError ) goto error;
    
    printf("------------- Try suggestedLatency = 0.0\n");
    outputParameters.suggestedLatency = 0.0;
    err = paqaCheckLatency( &outputParameters, &data, sampleRate, paFramesPerBufferUnspecified );
    if( err != paNoError ) goto error;
    
    printf("------------- Try suggestedLatency = defaultLowOutputLatency\n");
    outputParameters.suggestedLatency = deviceInfo->defaultLowOutputLatency;
    err = paqaCheckLatency( &outputParameters, &data, sampleRate, paFramesPerBufferUnspecified );
    if( err != paNoError ) goto error;
    
    printf("------------- Try suggestedLatency = defaultHighOutputLatency\n");
    outputParameters.suggestedLatency = deviceInfo->defaultHighOutputLatency;
    err = paqaCheckLatency( &outputParameters, &data, sampleRate, paFramesPerBufferUnspecified );
    if( err != paNoError ) goto error;
    
    printf("------------- Try suggestedLatency = defaultHighOutputLatency * 4\n");
    outputParameters.suggestedLatency = deviceInfo->defaultHighOutputLatency * 4;
    err = paqaCheckLatency( &outputParameters, &data, sampleRate, paFramesPerBufferUnspecified );
    if( err != paNoError ) goto error;
    
    
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
