/** @file patest_flex.c
    @ingroup test_src
    @brief Flexible test for recording or playing N channels with optional callback sleep.
    @author Phil Burk  https://www.softsynth.com
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
#include <math.h>

#include "portaudio.h"

#define SAMPLE_RATE           (44100)
#define FRAMES_PER_BUFFER     (128)
#define DEFAULT_NUM_SECONDS   (5)
#define DEFAULT_NUM_CHANNELS  (2)
#define MAX_CHANNELS          (64)
#define BASE_FREQUENCY        (220.0)
#define FREQUENCY_RATIO       (9.0 / 8.0)
#define SECONDS_BEFORE_SLEEP  (2)

#ifndef M_PI
#define M_PI  (3.14159265358979323846)
#endif

typedef struct
{
    int             numInputChannels;
    int             numOutputChannels;
    double          phases[MAX_CHANNELS];
    volatile float  peakAmplitudes[MAX_CHANNELS];
    volatile int    sleepMillis;
    volatile int    isSleeping;
    volatile int    callbackCount;
}
paTestData;

/* This routine will be called by the PortAudio engine when audio is needed.
** It may be called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/
static int patestCallback( const void *inputBuffer, void *outputBuffer,
                           unsigned long framesPerBuffer,
                           const PaStreamCallbackTimeInfo* timeInfo,
                           PaStreamCallbackFlags statusFlags,
                           void *userData )
{
    paTestData *data = (paTestData*)userData;
    unsigned long frameIndex;
    int channelIndex;

    (void) timeInfo;    /* Prevent unused variable warnings. */
    (void) statusFlags;

    data->callbackCount += 1;

    if( data->numInputChannels > 0 )
    {
        const float *in = (const float*)inputBuffer;

        if( in != NULL )
        {
            for( frameIndex = 0; frameIndex < framesPerBuffer; frameIndex++ )
            {
                for( channelIndex = 0; channelIndex < data->numInputChannels; channelIndex++ )
                {
                    float sample = *in++;
                    float absSample = (sample < 0.0f) ? -sample : sample;
                    if( absSample > data->peakAmplitudes[channelIndex] )
                    {
                        data->peakAmplitudes[channelIndex] = absSample;
                    }
                }
            }
        }
    }

    if( data->numOutputChannels > 0 )
    {
        float *out = (float*)outputBuffer;

        if( out != NULL )
        {
            for( frameIndex = 0; frameIndex < framesPerBuffer; frameIndex++ )
            {
                double frequency = BASE_FREQUENCY;
                for( channelIndex = 0; channelIndex < data->numOutputChannels; channelIndex++ )
                {
                    *out++ = (float) sin( data->phases[channelIndex] );

                    data->phases[channelIndex] += (2.0 * M_PI * frequency) / SAMPLE_RATE;
                    if( data->phases[channelIndex] >= (2.0 * M_PI) )
                    {
                        data->phases[channelIndex] -= (2.0 * M_PI);
                    }
                    frequency *= FREQUENCY_RATIO;
                }
            }
        }
    }

    if( data->sleepMillis > 0 )
    {
        int sleepMillis = data->sleepMillis;
        data->sleepMillis = 0;
        data->isSleeping = 1;
        Pa_Sleep( sleepMillis );
        data->isSleeping = 0;
    }

    return paContinue;
}

/*******************************************************************/
static PaError listAudioDevices( void )
{
    int i, numDevices;
    const PaDeviceInfo *deviceInfo;

    numDevices = Pa_GetDeviceCount();
    if( numDevices < 0 )
    {
        printf( "ERROR: Pa_GetDeviceCount returned 0x%x\n", numDevices );
        return numDevices;
    }

    printf( "Number of devices = %d\n", numDevices );
    for( i = 0; i < numDevices; i++ )
    {
        deviceInfo = Pa_GetDeviceInfo( i );
        printf( "#%2d: Name = \"%-64s\", Host API = %-10s, Max inputs = %3d, Max outputs = %3d\n",
                i,
                deviceInfo->name,
                Pa_GetHostApiInfo( deviceInfo->hostApi )->name,
                deviceInfo->maxInputChannels,
                deviceInfo->maxOutputChannels );
    }
    return paNoError;
}

/*******************************************************************/
static void usage( const char *name )
{
    printf("Usage: %s [-d] [-r] [-p] [-b] [-c#] [-i#] [-o#] [-t#] [-s#] [-f#]\n", name);
    printf("  -d   - List available audio devices.\n");
    printf("  -r   - Record input stream.\n");
    printf("  -p   - Playback sine waves on output stream (default if no mode specified).\n");
    printf("  -b   - Both record and playback (full duplex).\n");
    printf("  -c#  - Number of channels (default %d, max %d).\n", DEFAULT_NUM_CHANNELS, MAX_CHANNELS);
    printf("  -i#  - Record from specified input device ID (or default if no ID given).\n");
    printf("  -o#  - Playback to specified output device ID (or default if no ID given).\n");
    printf("  -t#  - Time to run in seconds (default %d).\n", DEFAULT_NUM_SECONDS);
    printf("  -s#  - Sleep time in milliseconds inside callback after %d seconds (default 0).\n", SECONDS_BEFORE_SLEEP);
    printf("  -f#  - Frames per buffer (default %d).\n", FRAMES_PER_BUFFER);
    printf("  -h   - Help.\n");
}

/*******************************************************************/
static int parseIntOption( int argc, char **argv, int *indexPtr, int *valuePtr )
{
    char *arg = argv[*indexPtr];
    if( arg[2] != '\0' )
    {
        *valuePtr = atoi( &arg[2] );
        return 1;
    }
    else if( (*indexPtr + 1) < argc && argv[*indexPtr + 1][0] != '-' )
    {
        *indexPtr += 1;
        *valuePtr = atoi( argv[*indexPtr] );
        return 1;
    }
    return 0;
}

/*******************************************************************/
int main( int argc, char **argv );
int main( int argc, char **argv )
{
    PaStreamParameters inputParameters;
    PaStreamParameters outputParameters;
    PaStream *stream = NULL;
    PaError err = paNoError;
    const PaDeviceInfo *inputDeviceInfo = NULL;
    const PaDeviceInfo *outputDeviceInfo = NULL;
    paTestData data;
    int i;
    int second;
    int listDevices = 0;
    int doRecord = 0;
    int doPlay = 0;
    int numChannels = DEFAULT_NUM_CHANNELS;
    int numSeconds = DEFAULT_NUM_SECONDS;
    int sleepMillis = 0;
    int sleepStartSecond;
    int previousCallbackCount = 0;
    unsigned long framesPerBuffer = FRAMES_PER_BUFFER;
    PaDeviceIndex inputDeviceId = paNoDevice;
    PaDeviceIndex outputDeviceId = paNoDevice;
    char *executableName = argv[0];

    /* Parse command line options. */
    i = 1;
    while( i < argc )
    {
        char *arg = argv[i];
        if( arg[0] == '-' )
        {
            int value = 0;
            switch( arg[1] )
            {
                case 'd':
                    listDevices = 1;
                    break;

                case 'r':
                    doRecord = 1;
                    break;

                case 'p':
                    doPlay = 1;
                    break;

                case 'b':
                    doRecord = 1;
                    doPlay = 1;
                    break;

                case 'c':
                    if( !parseIntOption( argc, argv, &i, &numChannels ) )
                    {
                        fprintf( stderr, "Missing channel count for %s\n", arg );
                        usage( executableName );
                        return 1;
                    }
                    break;

                case 'i':
                    doRecord = 1;
                    if( parseIntOption( argc, argv, &i, &value ) )
                    {
                        inputDeviceId = (PaDeviceIndex)value;
                    }
                    break;

                case 'o':
                    doPlay = 1;
                    if( parseIntOption( argc, argv, &i, &value ) )
                    {
                        outputDeviceId = (PaDeviceIndex)value;
                    }
                    break;

                case 't':
                    if( !parseIntOption( argc, argv, &i, &numSeconds ) )
                    {
                        fprintf( stderr, "Missing duration for %s\n", arg );
                        usage( executableName );
                        return 1;
                    }
                    break;

                case 's':
                    if( !parseIntOption( argc, argv, &i, &sleepMillis ) )
                    {
                        fprintf( stderr, "Missing sleep time for %s\n", arg );
                        usage( executableName );
                        return 1;
                    }
                    break;

                case 'f':
                    if( !parseIntOption( argc, argv, &i, &value ) || value < 0 )
                    {
                        fprintf( stderr, "Invalid framesPerBuffer for %s\n", arg );
                        usage( executableName );
                        return 1;
                    }
                    framesPerBuffer = (unsigned long)value;
                    break;

                case 'h':
                case '?':
                    usage( executableName );
                    return 0;

                default:
                    fprintf( stderr, "Illegal option: %s\n", arg );
                    usage( executableName );
                    return 1;
            }
        }
        else
        {
            fprintf( stderr, "Illegal argument: %s\n", arg );
            usage( executableName );
            return 1;
        }
        i += 1;
    }

    if( !doRecord && !doPlay && !listDevices )
    {
        doPlay = 1;
    }

    if( numChannels <= 0 || numChannels > MAX_CHANNELS )
    {
        fprintf( stderr, "Error: numChannels (%d) must be between 1 and %d.\n",
                 numChannels, MAX_CHANNELS );
        return 1;
    }
    if( numSeconds <= 0 )
    {
        fprintf( stderr, "Error: time (%d) must be greater than 0.\n", numSeconds );
        return 1;
    }
    if( sleepMillis < 0 )
    {
        fprintf( stderr, "Error: sleep time (%d) cannot be negative.\n", sleepMillis );
        return 1;
    }

    data.numInputChannels = doRecord ? numChannels : 0;
    data.numOutputChannels = doPlay ? numChannels : 0;
    data.sleepMillis = 0;
    data.isSleeping = 0;
    data.callbackCount = 0;
    for( i = 0; i < MAX_CHANNELS; i++ )
    {
        data.phases[i] = 0.0;
        data.peakAmplitudes[i] = 0.0f;
    }

    err = Pa_Initialize();
    if( err != paNoError ) goto error;

    if( listDevices )
    {
        err = listAudioDevices();
        if( err != paNoError ) goto error;
        if( !doRecord && !doPlay )
        {
            Pa_Terminate();
            return 0;
        }
    }

    if( doRecord )
    {
        if( inputDeviceId == paNoDevice )
        {
            inputDeviceId = Pa_GetDefaultInputDevice();
            if( inputDeviceId == paNoDevice )
            {
                fprintf( stderr, "Error: No default input device.\n" );
                goto error;
            }
        }

        inputDeviceInfo = Pa_GetDeviceInfo( inputDeviceId );
        if( inputDeviceInfo == NULL )
        {
            fprintf( stderr, "Error: Invalid input device ID %d.\n", inputDeviceId );
            err = paInvalidDevice;
            goto error;
        }

        inputParameters.device = inputDeviceId;
        inputParameters.channelCount = numChannels;
        inputParameters.sampleFormat = paFloat32;
        inputParameters.suggestedLatency = inputDeviceInfo->defaultLowInputLatency;
        inputParameters.hostApiSpecificStreamInfo = NULL;
    }

    if( doPlay )
    {
        if( outputDeviceId == paNoDevice )
        {
            outputDeviceId = Pa_GetDefaultOutputDevice();
            if( outputDeviceId == paNoDevice )
            {
                fprintf( stderr, "Error: No default output device.\n" );
                goto error;
            }
        }

        outputDeviceInfo = Pa_GetDeviceInfo( outputDeviceId );
        if( outputDeviceInfo == NULL )
        {
            fprintf( stderr, "Error: Invalid output device ID %d.\n", outputDeviceId );
            err = paInvalidDevice;
            goto error;
        }

        outputParameters.device = outputDeviceId;
        outputParameters.channelCount = numChannels;
        outputParameters.sampleFormat = paFloat32;
        outputParameters.suggestedLatency = outputDeviceInfo->defaultLowOutputLatency;
        outputParameters.hostApiSpecificStreamInfo = NULL;
    }

    if( doRecord && doPlay )
    {
        printf("PortAudio Flex Test: full-duplex %d channel(s) on input device #%d (%s) and output device #%d (%s) for %d second(s).\n",
               numChannels,
               inputDeviceId,
               inputDeviceInfo->name,
               outputDeviceId,
               outputDeviceInfo->name,
               numSeconds );
    }
    else if( doRecord )
    {
        printf("PortAudio Flex Test: recording %d channel(s) on device #%d (%s) for %d second(s).\n",
               numChannels,
               inputDeviceId,
               inputDeviceInfo->name,
               numSeconds );
    }
    else
    {
        printf("PortAudio Flex Test: playing %d channel(s) on device #%d (%s) for %d second(s).\n",
               numChannels,
               outputDeviceId,
               outputDeviceInfo->name,
               numSeconds );
    }

    if( sleepMillis > 0 )
    {
        printf("Callback will sleep %d msec after a few seconds.\n", sleepMillis);
    }

    err = Pa_OpenStream(
              &stream,
              doRecord ? &inputParameters : NULL,
              doPlay ? &outputParameters : NULL,
              SAMPLE_RATE,
              framesPerBuffer,
              paClipOff,
              patestCallback,
              &data );
    if( err != paNoError ) goto error;

    err = Pa_StartStream( stream );
    if( err != paNoError ) goto error;

    sleepStartSecond = (numSeconds > SECONDS_BEFORE_SLEEP) ? SECONDS_BEFORE_SLEEP : (numSeconds / 2);
    if( sleepMillis > 0 )
    {
        int minSeconds = sleepStartSecond + (sleepMillis / 1000) + 2;
        if( numSeconds < minSeconds )
        {
            numSeconds = minSeconds;
        }
    }

    for( second = 0; second < numSeconds; second++ )
    {
        int wasSleeping;
        int currentCallbackCount;
        int callbacksThisSecond;

        if( (sleepMillis > 0) && (second == sleepStartSecond) )
        {
            printf("Setting callback sleep time to %d msec.\n", sleepMillis);
            fflush( stdout );
            data.sleepMillis = sleepMillis;
        }

        wasSleeping = data.isSleeping || (data.sleepMillis > 0 && second == sleepStartSecond);
        previousCallbackCount = data.callbackCount;

        Pa_Sleep( 1000 );

        currentCallbackCount = data.callbackCount;
        callbacksThisSecond = currentCallbackCount - previousCallbackCount;

        if( doRecord )
        {
            printf("Second %2d: callbacks = %4d, peak amplitude =",
                   second + 1, callbacksThisSecond);
            for( i = 0; i < numChannels; i++ )
            {
                float peak = data.peakAmplitudes[i];
                data.peakAmplitudes[i] = 0.0f;
                printf(" %.6f", peak);
            }
            printf("\n");
            fflush( stdout );
        }
        else
        {
            printf("Second %2d: callbacks = %4d\n", second + 1, callbacksThisSecond);
            fflush( stdout );
        }

        if( !wasSleeping && !data.isSleeping && (callbacksThisSecond <= 0) )
        {
            fprintf( stderr, "Error: Callback counter did not advance!\n" );
            err = paInternalError;
            goto error;
        }
    }

    err = Pa_StopStream( stream );
    if( err != paNoError ) goto error;

    err = Pa_CloseStream( stream );
    stream = NULL;
    if( err != paNoError ) goto error;

    Pa_Terminate();
    printf("Test finished.\n");
    return 0;

error:
    if( stream != NULL )
    {
        Pa_CloseStream( stream );
    }
    Pa_Terminate();
    if( err != paNoError )
    {
        fprintf( stderr, "An error occurred while using the portaudio stream\n" );
        fprintf( stderr, "Error number: %d\n", err );
        fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
        return 1;
    }
    return 0;
}
