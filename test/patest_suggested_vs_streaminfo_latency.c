/** @file patest_suggested_vs_streaminfo_latency.c
	@ingroup test_src
	@brief Print suggested vs. PaStreamInfo reported actual latency
	@author Ross Bencina <rossb@audiomulch.com>
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

#define NUM_CHANNELS  (2)

#define SUGGESTED_LATENCY_START_SECONDS     (0.0)
#define SUGGESTED_LATENCY_END_SECONDS       (.5)
#define SUGGESTED_LATENCY_INCREMENT_SECONDS (0.0005) // half a millisecond increments


// dummy callback. does nothing
static int patestCallback( const void *inputBuffer, void *outputBuffer,
                            unsigned long framesPerBuffer,
                            const PaStreamCallbackTimeInfo* timeInfo,
                            PaStreamCallbackFlags statusFlags,
                            void *userData )
{
}


/*
    TODO:
        

        A test that opens streams with a sequence of suggested latency values 
        from 0 to 2 seconds in .5ms intervals and gathers the resulting actual 
        latency values. Output a csv file and graph suggested vs. actual. Run 
        with framesPerBuffer unspecified, powers of 2 and multiples of 50 and 
        prime number buffer sizes.

        o- add command line parameters to specify frames per buffer, sample rate, devices

        o- test at standard sample rates



*/

/*******************************************************************/
int main(void);
int main(void)
{
    PaStreamParameters outputParameters, inputParameters;
    PaStream *stream;
    PaError err;
    int i;
    PaTime suggestedLatency;
    PaStreamInfo *streamInfo;
    PaDeviceInfo *deviceInfo;

    err = Pa_Initialize();
    if( err != paNoError ) goto error;

    printf("# sample rate=%f, frames per buffer=%d\n", (float)SAMPLE_RATE, FRAMES_PER_BUFFER );

    outputParameters.device = Pa_GetDefaultOutputDevice();
    if (outputParameters.device == paNoDevice) {
      fprintf(stderr,"Error: No default output device.\n");
      goto error;
    }
    
    outputParameters.channelCount = NUM_CHANNELS;
    outputParameters.sampleFormat = paFloat32; /* 32 bit floating point output */
    outputParameters.hostApiSpecificStreamInfo = NULL;

    deviceInfo = Pa_GetDeviceInfo(outputParameters.device);
    printf( "# using output device id %d (%s, %s)\n", outputParameters.device, deviceInfo->name, Pa_GetHostApiInfo(deviceInfo->hostApi)->name );


    inputParameters.device = Pa_GetDefaultInputDevice();
    if (inputParameters.device == paNoDevice) {
      fprintf(stderr,"Error: No default input device.\n");
      goto error;
    }
    
    inputParameters.channelCount = NUM_CHANNELS;
    inputParameters.sampleFormat = paFloat32; /* 32 bit floating point output */
    inputParameters.hostApiSpecificStreamInfo = NULL;

    deviceInfo = Pa_GetDeviceInfo(inputParameters.device);
    printf( "# using input device id %d (%s, %s)\n", inputParameters.device, deviceInfo->name, Pa_GetHostApiInfo(deviceInfo->hostApi)->name );


    printf( "# suggested latency, half duplex PaStreamInfo::outputLatency, half duplex PaStreamInfo::inputLatency, half duplex PaStreamInfo::outputLatency, half duplex PaStreamInfo::inputLatency\n" );
    suggestedLatency = SUGGESTED_LATENCY_START_SECONDS;
    while( suggestedLatency <= SUGGESTED_LATENCY_END_SECONDS ){

        outputParameters.suggestedLatency = suggestedLatency;
        inputParameters.suggestedLatency = suggestedLatency;

        printf( "%f, ", suggestedLatency );

        /* ------------------------------ output ------------------------------ */

        err = Pa_OpenStream(
                  &stream,
                  NULL, /* no input */
                  &outputParameters,
                  SAMPLE_RATE,
                  FRAMES_PER_BUFFER,
                  paClipOff,      /* we won't output out of range samples so don't bother clipping them */
                  patestCallback,
                  0 );
        if( err != paNoError ) goto error;

        streamInfo = Pa_GetStreamInfo( stream );

        printf( "%f,", streamInfo->outputLatency  );

        err = Pa_CloseStream( stream );
        if( err != paNoError ) goto error;

        /* ------------------------------ input ------------------------------ */

        err = Pa_OpenStream(
                  &stream,
                  &inputParameters, 
                  NULL, /* no output */
                  SAMPLE_RATE,
                  FRAMES_PER_BUFFER,
                  paClipOff,      /* we won't output out of range samples so don't bother clipping them */
                  patestCallback,
                  0 );
        if( err != paNoError ) goto error;

        streamInfo = Pa_GetStreamInfo( stream );

        printf( "%f,", streamInfo->inputLatency  );

        err = Pa_CloseStream( stream );
        if( err != paNoError ) goto error;

        /* ------------------------------ full duplex ------------------------------ */

        err = Pa_OpenStream(
                  &stream,
                  &inputParameters, 
                  &outputParameters,
                  SAMPLE_RATE,
                  FRAMES_PER_BUFFER,
                  paClipOff,      /* we won't output out of range samples so don't bother clipping them */
                  patestCallback,
                  0 );
        if( err != paNoError ) goto error;

        streamInfo = Pa_GetStreamInfo( stream );

        printf( "%f,%f", streamInfo->outputLatency, streamInfo->inputLatency );

        err = Pa_CloseStream( stream );
        if( err != paNoError ) goto error;

        /* ------------------------------------------------------------ */

        printf( "\n" );
        suggestedLatency += SUGGESTED_LATENCY_INCREMENT_SECONDS;
    }

    Pa_Terminate();
    printf("# Test finished.\n");
    
    return err;
error:
    Pa_Terminate();
    fprintf( stderr, "An error occured while using the portaudio stream\n" );
    fprintf( stderr, "Error number: %d\n", err );
    fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
    return err;
}
