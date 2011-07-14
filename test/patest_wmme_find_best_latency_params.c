/*
 * $Id: $
 * Portable Audio I/O Library
 * Windows MME low level buffer parameters search
 *
 * Copyright (c) 2010 Ross Bencina
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

#include <windows.h>    /* required when using pa_win_wmme.h */
#include <mmsystem.h>   /* required when using pa_win_wmme.h */

#include <conio.h>      /* for _getch */


#include "portaudio.h"
#include "pa_win_wmme.h"


#define SAMPLE_RATE             (22050)

#ifndef M_PI
#define M_PI  (3.14159265)
#endif

#define TABLE_SIZE              (2048)

#define CHANNEL_COUNT           (2)


typedef struct
{
    float sine[TABLE_SIZE];
	double phase;
}
paTestData;

static paTestData data;

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
    unsigned long i,j;

    (void) timeInfo; /* Prevent unused variable warnings. */
    (void) statusFlags;
    (void) inputBuffer;
    
    for( i=0; i<framesPerBuffer; i++ )
    {
        float x = data->sine[(int)data->phase];
        data->phase += 20;
        if( data->phase >= TABLE_SIZE ){
			data->phase -= TABLE_SIZE;
		}

		for( j = 0; j < CHANNEL_COUNT; ++j ){
            *out++ = x;
		}
	}
    
    return paContinue;
}


#define YES     1
#define NO      0


static int playUntilKeyPress( int deviceIndex, int framesPerUserBuffer, int framesPerWmmeBuffer, int wmmeBufferCount )
{
    PaStreamParameters outputParameters;
    PaWinMmeStreamInfo wmmeStreamInfo;
    PaStream *stream;
    PaError err;
    int c;

    outputParameters.device = deviceIndex;
    outputParameters.channelCount = CHANNEL_COUNT;
    outputParameters.sampleFormat = paFloat32; /* 32 bit floating point processing */
    outputParameters.suggestedLatency = 0; /*Pa_GetDeviceInfo( outputParameters.device )->defaultLowOutputLatency;*/
    outputParameters.hostApiSpecificStreamInfo = NULL;

    wmmeStreamInfo.size = sizeof(PaWinMmeStreamInfo);
    wmmeStreamInfo.hostApiType = paMME; 
    wmmeStreamInfo.version = 1;
    wmmeStreamInfo.flags = paWinMmeUseLowLevelLatencyParameters | paWinMmeDontThrottleOverloadedProcessingThread;
    wmmeStreamInfo.framesPerBuffer = framesPerWmmeBuffer;
    wmmeStreamInfo.bufferCount = wmmeBufferCount;
    outputParameters.hostApiSpecificStreamInfo = &wmmeStreamInfo;

    err = Pa_OpenStream(
              &stream,
              NULL, /* no input */
              &outputParameters,
              SAMPLE_RATE,
              framesPerUserBuffer,
              paClipOff | paPrimeOutputBuffersUsingStreamCallback,      /* we won't output out of range samples so don't bother clipping them */
              patestCallback,
              &data );
    if( err != paNoError ) goto error;

    err = Pa_StartStream( stream );
    if( err != paNoError ) goto error;


    do{
        printf( "Trying buffer size %d.\nIf it sounds smooth press 'y', if it sounds bad press 'n'\n", framesPerWmmeBuffer );
        c = tolower(_getch());
    }while( c != 'y' && c != 'n' );

    err = Pa_AbortStream( stream );
    if( err != paNoError ) goto error;

    err = Pa_CloseStream( stream );
    if( err != paNoError ) goto error;

    return (c == 'y') ? YES : NO;

error:
    return err;
}


/*******************************************************************/
int main(int argc, char* argv[])
{
    PaError err;
    int i;
    int deviceIndex;
    int wmmeBufferCount, wmmeBufferSize, smallestWorkingBufferSize;
    int min, max, mid;
    int testResult;
    FILE *resultsFp;
    OSVERSIONINFO windowsVersion;

    err = Pa_Initialize();
    if( err != paNoError ) goto error;

    /*
    TODO: print an index of devices and ask the user to select one

    */

	deviceIndex = Pa_GetHostApiInfo( Pa_HostApiTypeIdToHostApiIndex( paMME ) )->defaultOutputDevice;
	if( argc == 2 ){
		sscanf( argv[1], "%d", &deviceIndex );
	}

	printf( "using device id %d (%s)\n", deviceIndex, Pa_GetDeviceInfo(deviceIndex)->name );

    /* initialise sinusoidal wavetable */
    for( i=0; i<TABLE_SIZE; i++ )
    {
        data.sine[i] = (float) sin( ((double)i/(double)TABLE_SIZE) * M_PI * 2. );
    }

	data.phase = 0;


    resultsFp = fopen( "results.txt", "at" );
    fprintf( resultsFp, "*** WMME smallest working buffer sizes\n" );

    memset( &windowsVersion, 0, sizeof(OSVERSIONINFO) );
    windowsVersion.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
    GetVersionEx( &windowsVersion );

    fprintf( resultsFp, "windows version: %d.%d.%d %S\n", windowsVersion.dwMajorVersion, windowsVersion.dwMinorVersion, windowsVersion.dwBuildNumber, windowsVersion.szCSDVersion );
    fprintf( resultsFp, "audio device: %s\n", Pa_GetDeviceInfo( deviceIndex )->name );
    

    /*
        TODO: test at 22050 44100, 48000, 96000, mono and stereo and surround

        TODO: should be testing with 80% CPU load


        another thing to try would be setting the timeBeginPeriod granularity to 1ms and see if it changes the behavior
    */

    printf( "testing with sample rate %f.\n", (float)SAMPLE_RATE );
    fprintf( resultsFp, "sample rate: %f\n", (float)SAMPLE_RATE );
    fprintf( resultsFp, "buffer count, smallest working size (frames)\n" );

    for( wmmeBufferCount = 2; wmmeBufferCount < 13; ++wmmeBufferCount ){
 
     
        printf( "testing with %d buffers...\n", wmmeBufferCount );

        /*
            Binary search after Niklaus Wirth
            from http://en.wikipedia.org/wiki/Binary_search_algorithm#The_algorithm
         */
        min = 1;
        max = 8192;    /* we assume that this size works */
        smallestWorkingBufferSize = 0;

        do{
            mid = min + ((max - min) / 2);

            wmmeBufferSize = mid;
            testResult = playUntilKeyPress( deviceIndex, wmmeBufferSize, wmmeBufferSize, wmmeBufferCount );

            if( testResult == YES ){
                max = mid - 1;
                smallestWorkingBufferSize = wmmeBufferSize;
            }else{
                min = mid + 1;
            }
             
        }while( (min <= max) && (testResult == YES || testResult == NO) );

        printf( "smallest working buffer size for %d buffers is: %d\n", wmmeBufferCount, smallestWorkingBufferSize );

        fprintf( resultsFp, "%d, %d\n", wmmeBufferCount, smallestWorkingBufferSize );
        fflush( resultsFp );
    }

    fprintf( resultsFp, "###\n" );

    fclose( resultsFp );
    
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

