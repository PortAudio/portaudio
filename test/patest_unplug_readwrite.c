/** @file patest_unplug.c
	@ingroup test_src
	@brief Debug a crash involving unplugging a USB device.
	@author Phil Burk  http://www.softsynth.com
*/
/*
 * $Id$
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
#include <stdlib.h>
#include <memory.h>
#include <math.h>
#include "portaudio.h"

#define NUM_SECONDS   (8)
#define SAMPLE_RATE   (44100)
#ifndef M_PI
#define M_PI  (3.14159265)
#endif
#define TABLE_SIZE        (200)
#define FRAMES_PER_BUFFER  (64)
#define MAX_CHANNELS        (8)

#define INPUT_CHANNELS      (1) /* 1 so it works with headset mic */
#define OUTPUT_CHANNELS     (2)

typedef struct
{
    short sine[TABLE_SIZE];
    long phases[MAX_CHANNELS];
    long numChannels;
}
paTestData;

static void generateSine( short *out,
                          unsigned long framesPerBuffer,
                          paTestData *data )
{
    unsigned int i;
    int channelIndex;

    for( i=0; i<framesPerBuffer; i++ )
    {
        for (channelIndex = 0; channelIndex < data->numChannels; channelIndex++)
        {
            int phase = data->phases[channelIndex];
            *out++ = data->sine[phase];
            phase += channelIndex + 2;
            if( phase >= TABLE_SIZE ) phase -= TABLE_SIZE;
            data->phases[channelIndex] = phase;
        }
    }
}

/*******************************************************************/
int main(int argc, char **args);
int main(int argc, char **args)
{
    PaStreamParameters inputParameters;
    PaStreamParameters outputParameters;
    PaStream *inputStream;
    PaStream *outputStream;
    const PaDeviceInfo *deviceInfo;
    PaError err;
    paTestData data;
    int i;
    int totalSamps;
    int inputDevice = -1;
    int outputDevice = -1;
    int sampsToGo;
    short inputBuffer[INPUT_CHANNELS * FRAMES_PER_BUFFER];
    short outputBuffer[OUTPUT_CHANNELS * FRAMES_PER_BUFFER];

    printf("Test unplugging a USB device.\n");

    if( argc > 1 ) {
       inputDevice = outputDevice = atoi( args[1] );
       printf("Using device number %d.\n\n", inputDevice );
    } else {
       printf("Using default device.\n\n" );
    }

    memset(&data, 0, sizeof(data));

    /* initialise sinusoidal wavetable */
    for( i=0; i<TABLE_SIZE; i++ )
    {
        data.sine[i] = (short) (32767.0 * sin( ((double)i/(double)TABLE_SIZE) * M_PI * 2. ));
    }
    data.numChannels = OUTPUT_CHANNELS;

    sampsToGo = totalSamps =  NUM_SECONDS * SAMPLE_RATE; /* Play for a few seconds. */


    err = Pa_Initialize();
    if( err != paNoError ) goto error;

    if( inputDevice == -1 )
        inputParameters.device = Pa_GetDefaultInputDevice(); /* default input device */
    else
        inputParameters.device = inputDevice ;

    if (inputParameters.device == paNoDevice) {
        fprintf(stderr,"Error: No default input device.\n");
        goto error;
    }

    if( outputDevice == -1 )
        outputParameters.device = Pa_GetDefaultOutputDevice(); /* default output device */
    else
        outputParameters.device = outputDevice ;

    if (outputParameters.device == paNoDevice) {
        fprintf(stderr,"Error: No default output device.\n");
        goto error;
    }

    inputParameters.channelCount = INPUT_CHANNELS;
    inputParameters.sampleFormat = paInt16;
    deviceInfo = Pa_GetDeviceInfo( inputParameters.device );
    if( deviceInfo == NULL )
    {
        fprintf( stderr, "No matching input device.\n" );
        goto error;
    }
    inputParameters.suggestedLatency = deviceInfo->defaultLowInputLatency;
    inputParameters.hostApiSpecificStreamInfo = NULL;
    err = Pa_OpenStream(
                &inputStream,
                &inputParameters,
                NULL,
                SAMPLE_RATE,
                FRAMES_PER_BUFFER,
                0,
                NULL,
                &data );
    if( err != paNoError ) goto error;

    outputParameters.channelCount = OUTPUT_CHANNELS;
    outputParameters.sampleFormat = paInt16;
    deviceInfo = Pa_GetDeviceInfo( outputParameters.device );
    if( deviceInfo == NULL )
    {
        fprintf( stderr, "No matching output device.\n" );
        goto error;
    }
    outputParameters.suggestedLatency = deviceInfo->defaultLowOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = NULL;
    err = Pa_OpenStream(
                &outputStream,
                NULL,
                &outputParameters,
                SAMPLE_RATE,
                FRAMES_PER_BUFFER,
                (paClipOff | paDitherOff),
                NULL,
                &data );
    if( err != paNoError ) goto error;

    err = Pa_StartStream( inputStream );
    if( err != paNoError ) goto error;
    err = Pa_StartStream( outputStream );
    if( err != paNoError ) goto error;

    printf("When you hear sound, unplug the USB device.\n");
    do
    {
        signed long available;
        available = Pa_GetStreamReadAvailable(inputStream);
        while( available > 0 ) {
            if( available > FRAMES_PER_BUFFER )
                available = FRAMES_PER_BUFFER;

            err = Pa_ReadStream( inputStream, inputBuffer, available ); /* reading <= available means don't block */
            if( err != paNoError ) goto done; /* Move on to stopping stream */

            sampsToGo -= available;

            available = Pa_GetStreamReadAvailable(inputStream);
        }

        available = Pa_GetStreamWriteAvailable(outputStream);
        while( available > 0 ) {
            if( available > FRAMES_PER_BUFFER )
                available = FRAMES_PER_BUFFER;

            generateSine( outputBuffer, available, &data );
            err = Pa_WriteStream( outputStream, outputBuffer, available ); /* reading <= available means don't block */
            if( err != paNoError ) goto done; /* Move on to stopping stream */

            available = Pa_GetStreamWriteAvailable(outputStream);
        }

        Pa_Sleep(1);

        printf("Frames remaining = %d\n", sampsToGo);
        printf("Pa_IsStreamActive(inputStream) = %d\n", Pa_IsStreamActive(inputStream));
        printf("Pa_IsStreamActive(outputStream) = %d\n", Pa_IsStreamActive(outputStream));
    } while( Pa_IsStreamActive(inputStream) && Pa_IsStreamActive(outputStream) && sampsToGo > 0 );
done:

#if 1
    printf("Stopping input stream...\n");
    err = Pa_StopStream( inputStream );
    if( err != paNoError ) goto error;
    printf("Input stream stopped OK.\n");

    printf("Stopping output stream...\n");
    err = Pa_StopStream( outputStream );
    if( err != paNoError ) goto error;
    printf("Output stream stopped OK.\n");
#endif

#if 0
    err = Pa_AbortStream( inputStream );
    if( err != paNoError ) goto error;
    printf("Input stream stopped OK.\n");
    err = Pa_AbortStream( outputStream );
    if( err != paNoError ) goto error;
    printf("Output stream stopped OK.\n");
#endif

    err = Pa_CloseStream( inputStream );
    if( err != paNoError ) goto error;
    printf("Input stream closed OK.\n");
    err = Pa_CloseStream( outputStream );
    if( err != paNoError ) goto error;
    printf("Output stream closed OK.\n");
    Pa_Terminate();
    return paNoError;
error:
    Pa_Terminate();
    fprintf( stderr, "An error occured while using the portaudio stream\n" );
    fprintf( stderr, "Error number: %d\n", err );
    fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
    fprintf( stderr, "Host Error message: %s\n", Pa_GetLastHostErrorInfo()->errorText );
    return err;
}
