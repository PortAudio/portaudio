/** @file paqa_devs.c
    @ingroup qa_src
    @brief Self Testing Quality Assurance app for PortAudio
    Try to open devices and run through all possible configurations.
    By default, open only the default devices. Command line options support
    opening every device, or all input devices, or all output devices.
    This test does not verify that the configuration works well.
    It just verifies that it does not crash. It requires a human to
    listen to the sine wave outputs.

    @author Phil Burk  http://www.softsynth.com

    Pieter adapted to V19 API. Test now relies heavily on
    Pa_IsFormatSupported(). Uses same 'standard' sample rates
    as in test pa_devs.c.
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
#include <string.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include "portaudio.h"
#include "pa_trace.h"
#include "paqa_macros.h"

/****************************************** Definitions ***********/
#define MODE_INPUT         (0)
#define MODE_OUTPUT        (1)
#define MAX_TEST_CHANNELS  (4)
#define LOWEST_FREQUENCY   (300.0)
#define LOWEST_SAMPLE_RATE (8000.0)
#define SINE_AMPLITUDE     (0.2)

#define RUN_TIME_SECONDS   (2)

typedef struct PaSineOscillator
{
    float          phase;
    float          phaseIncrement;
} PaSineOscillator;

/* Parameters that cover all options for a test.
 */
typedef struct PaQaTestParameters
{
    PaDeviceIndex    deviceID;
    PaSampleFormat   format;
    double           sampleRate;
    int              framesPerBuffer;
    int              framesDuration;
    int              numInputChannels;
    int              numOutputChannels;
    int              mode;
} PaQaTestParameters;

typedef struct PaQaData
{
    PaQaTestParameters *parameters;
    // Dynamic state.
    int              bytesPerSample;
    unsigned long    frameCounter;
    unsigned long    framesLeft;
    unsigned long    minFramesPerBuffer;
    unsigned long    maxFramesPerBuffer;
    PaSineOscillator sineOscillators[MAX_TEST_CHANNELS];
} PaQaData;

/****************************************** Prototypes ***********/
static void TestDevices( int mode, int allDevices );
static void TestFormats( int mode, PaDeviceIndex deviceID, double sampleRate,
                         int numChannels );
static int TestAdvance( int mode, PaDeviceIndex deviceID, double sampleRate,
                        int numChannels, PaSampleFormat format );
static int QaCallback( const void *inputBuffer, void *outputBuffer,
                       unsigned long framesPerBuffer,
                       const PaStreamCallbackTimeInfo* timeInfo,
                       PaStreamCallbackFlags statusFlags,
                       void *userData );

static void PaQaSetupParameters(PaQaTestParameters *parameters,
                                int framesDuration,
                                int framesPerBuffer,
                   int sampleRate,
                   int mode,
                   int numInputChannels,
                   int numOutputChannels,
                   PaSampleFormat format)
{
<<<<<<< HEAD
    float phase = data->phase + PHASE_INCREMENT;
    if( phase > M_PI ) phase -= (float) (2.0 * M_PI);
    data->phase = phase;
=======
    parameters->framesDuration = framesDuration;
    parameters->framesPerBuffer = framesPerBuffer;
    parameters->numInputChannels = numInputChannels;
    parameters->numOutputChannels = numOutputChannels;
    parameters->mode = mode;
    parameters->sampleRate = sampleRate;
    parameters->format = format;
}

static void PaQaSetupData(PaQaData *myData,
                          PaQaTestParameters *parameters)
{
    myData->parameters = parameters;
    myData->frameCounter = 0;
    myData->framesLeft = (unsigned long) parameters->framesDuration;

    myData->minFramesPerBuffer = UINT32_MAX;
    myData->maxFramesPerBuffer = 0;

    for (int channelIndex = 0; channelIndex < MAX_TEST_CHANNELS; channelIndex++)
    {
        myData->sineOscillators[channelIndex].phase = 0.0f;
        myData->sineOscillators[channelIndex].phaseIncrement =
        (2.0 * M_PI * LOWEST_FREQUENCY / parameters->sampleRate);
    }

    switch( parameters->format )
    {
        case paFloat32:
        case paInt32:
        case paInt24:
            myData->bytesPerSample = 4;
            break;
            /*  case paPackedInt24:
             myData->bytesPerSample = 3;
             break; */
        default:
            myData->bytesPerSample = 2;
            break;
    }
}


/****************************************** Globals ***********/

static float NextSineSample( PaSineOscillator *sineOscillator )
{
    float phase = sineOscillator->phase + sineOscillator->phaseIncrement;
    if( phase > M_PI ) phase -= 2.0 * M_PI;
    sineOscillator->phase = phase;
>>>>>>> 68a7c88 (Improve paqa_devs)
    return sinf(phase) * SINE_AMPLITUDE;
}

/*******************************************************************/
/* This routine will be called by the PortAudio engine when audio is needed.
** It may be called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/
static int QaCallback( const void * /*inputBuffer */,
                       void *outputBuffer,
                       unsigned long framesPerBuffer,
                       const PaStreamCallbackTimeInfo* timeInfo,
                       PaStreamCallbackFlags statusFlags,
                       void *userData )
{
    unsigned long frameIndex;
    unsigned long channelIndex;
    float sample;
    PaQaData *data = (PaQaData *) userData;
    PaQaTestParameters *parameters = data->parameters;

    data->minFramesPerBuffer = (framesPerBuffer < data->minFramesPerBuffer)
            ? framesPerBuffer : data->minFramesPerBuffer;
    data->maxFramesPerBuffer = (framesPerBuffer > data->maxFramesPerBuffer)
            ? framesPerBuffer : data->maxFramesPerBuffer;

    /* Play simple sine wave. */
    if( parameters->mode == MODE_OUTPUT )
    {
        switch( parameters->format )
        {
        case paFloat32:
            {
                float *out =  (float *) outputBuffer;
                for( frameIndex = 0; frameIndex < framesPerBuffer; frameIndex++ )
                {
                    for( channelIndex = 0; channelIndex < parameters->numOutputChannels; channelIndex++ )
                    {
                        sample = NextSineSample( &data->sineOscillators[channelIndex] );
                        *out++ = sample;
                    }
                }
            }
            break;

        case paInt32:
            {
                int *out =  (int *) outputBuffer;
                for( frameIndex = 0; frameIndex < framesPerBuffer; frameIndex++ )
                {
                    for( channelIndex = 0; channelIndex < parameters->numOutputChannels; channelIndex++ )
                    {
                        sample = NextSineSample( &data->sineOscillators[channelIndex] );
                        *out++ = ((int)(sample * 0x00800000)) << 8;
                    }
                }
            }
            break;

        case paInt16:
            {
                short *out =  (short *) outputBuffer;
                for( frameIndex = 0; frameIndex < framesPerBuffer; frameIndex++ )
                {
                    for( channelIndex = 0; channelIndex < parameters->numOutputChannels; channelIndex++ )
                    {
                        sample = NextSineSample( &data->sineOscillators[channelIndex] );
                        *out++ = (short)(sample * 32767);
                    }
                }
            }
            break;

        default:
            {
                unsigned char *out =  (unsigned char *) outputBuffer;
                unsigned long numBytes = framesPerBuffer * parameters->numOutputChannels * data->bytesPerSample;
                memset(out, 0, numBytes);
            }
            break;
        }
    }

    data->frameCounter += framesPerBuffer;

    /* Are we through yet? */
    if( data->framesLeft > framesPerBuffer )
    {
        PaUtil_AddTraceMessage("QaCallback: running. framesLeft", data->framesLeft );
        data->framesLeft -= framesPerBuffer;
        return 0;
    }
    else
    {
        PaUtil_AddTraceMessage("QaCallback: DONE! framesLeft", data->framesLeft );
        data->framesLeft = 0;
        return 1;
    }
}

static void CheckDefaultCallbackRun(PaStream *stream,
                        PaQaData *data,
                        unsigned long framesPerBuffer) {
    PaError result;

    EXPECT((result = Pa_IsStreamActive( stream ) == 0));
    EXPECT((result = Pa_IsStreamStopped( stream ) == 1));

    EXPECT((result = Pa_StartStream( stream ) == paNoError));

    EXPECT((result = Pa_IsStreamActive( stream ) == 1));
    EXPECT((result = Pa_IsStreamStopped( stream ) == 0));

    /* Sleep long enough for the stream callback to have stopped itself. */
    Pa_Sleep((RUN_TIME_SECONDS + 1)*1000);

    EXPECT((data->framesLeft == 0));
    EXPECT((data->frameCounter > (1 * data->parameters->sampleRate)));

    if (framesPerBuffer > 0) {
        ASSERT_EQ(framesPerBuffer, data->minFramesPerBuffer);
        ASSERT_EQ(framesPerBuffer, data->maxFramesPerBuffer);
    } else {
        EXPECT((data->minFramesPerBuffer > 0));
        EXPECT((data->maxFramesPerBuffer < data->parameters->sampleRate));
    }

    EXPECT((result = Pa_IsStreamActive( stream ) == 0)); // callback terminated it
    EXPECT((result = Pa_IsStreamStopped( stream ) == 0));

    EXPECT((result = Pa_StopStream( stream ) == paNoError));

    EXPECT((result = Pa_IsStreamActive( stream ) == 0));
    EXPECT((result = Pa_IsStreamStopped( stream ) == 1));

    EXPECT((result = Pa_CloseStream( stream ) == paNoError));
    return;

error:
    EXPECT((result = Pa_CloseStream( stream ) == paNoError));
    return;
}

#if 0
static void TestDefaultStreamCallback(unsigned long framesPerBuffer,
                               int inputChannelCount,
                               int outputChannelCount) {
    PaError result;
    PaQaData data = {0};
    PaStream *stream = NULL;
    printf("%s(fpb=%lu, ic=%d, oc=%d)\n", __func__,
           framesPerBuffer, inputChannelCount, outputChannelCount);

    /* Open an audio I/O stream. */
    result = Pa_OpenDefaultStream( &stream,
                                  inputChannelCount,
                                  outputChannelCount,
                                  paFloat32,  /* 32 bit floating point output */
                                  SAMPLE_RATE,
                                  framesPerBuffer,
                                  QaCallback,
                                  &data );

    EXPECT((result == paNoError));
    EXPECT((stream != NULL));

    data.framesLeft = RUN_TIME_SECONDS * SAMPLE_RATE;
    data.numInputChannels = inputChannelCount;
    data.numOutputChannels = outputChannelCount;
    data.minFramesPerBuffer = UINT32_MAX;
    data.maxFramesPerBuffer = 0;

    CheckDefaultCallbackRun(stream, &data, framesPerBuffer);
error:
    return;
}
#endif

/*******************************************************************/
static int TestAdvance( int mode,
                       PaDeviceIndex deviceID,
                       double sampleRate,
                       int numChannels,
                       PaSampleFormat format )
{
    PaQaTestParameters testParameters = {0};
    PaStreamParameters inputParameters, outputParameters, *ipp, *opp;
    PaStream *stream = NULL;
    PaError result = paNoError;
    PaQaData myData;
    int numInputChannels = 0;
    int numOutputChannels = 0;
    #define FRAMES_PER_BUFFER  (64)

    if( mode == MODE_INPUT )
    {
        numInputChannels = numChannels;
        inputParameters.device       = deviceID;
        inputParameters.channelCount = numChannels;
        inputParameters.sampleFormat = format;
        inputParameters.suggestedLatency =
                Pa_GetDeviceInfo( inputParameters.device )->defaultLowInputLatency;
        inputParameters.hostApiSpecificStreamInfo = NULL;
        ipp = &inputParameters;
    }
    else
    {
        ipp = NULL;
    }

    if( mode == MODE_OUTPUT )
    {
        numOutputChannels = numChannels;
        outputParameters.device       = deviceID;
        outputParameters.channelCount = numChannels;
        outputParameters.sampleFormat = format;
        outputParameters.suggestedLatency =
                Pa_GetDeviceInfo( outputParameters.device )->defaultLowOutputLatency;
        outputParameters.hostApiSpecificStreamInfo = NULL;
        opp = &outputParameters;
    }
    else
    {
        opp = NULL;
    }

    /* Setup data for synthesis thread. */
    PaQaSetupParameters(&testParameters,
                  (int) (sampleRate * RUN_TIME_SECONDS), /* framesLeft */
                  FRAMES_PER_BUFFER,
                  sampleRate,
                  mode,
                  numInputChannels,
                  numOutputChannels,
                  format);
    PaQaSetupData(&myData,
                  &testParameters);

    if(paFormatIsSupported == Pa_IsFormatSupported( ipp, opp, sampleRate ))
    {
        printf("------ TestAdvance: %s, device = %d, rate = %g"
               ", numChannels = %d, format = %lu -------\n",
                ( mode == MODE_INPUT ) ? "INPUT" : "OUTPUT",
                deviceID, sampleRate, numChannels, (unsigned long)format);
        result = Pa_OpenStream( &stream,
                                          ipp,
                                          opp,
                                          sampleRate,
                                          FRAMES_PER_BUFFER,
                                          paClipOff,  /* we won't output out of range samples so don't bother clipping them */
                                          QaCallback,
                                          &myData
                               );
        EXPECT((result == paNoError));
        EXPECT((stream != NULL));

        CheckDefaultCallbackRun(stream, &myData, FRAMES_PER_BUFFER);

        if( 0 )
        {
            PaTime oldStamp, newStamp;
            unsigned long oldFrames;
            int minDelay = ( mode == MODE_INPUT ) ? 1000 : 400;
            /* Was:
            int minNumBuffers = Pa_GetMinNumBuffers( FRAMES_PER_BUFFER, sampleRate );
            int msec = (int) ((minNumBuffers * 3 * 1000.0 * FRAMES_PER_BUFFER) / sampleRate);
            */
            int msec = (int)( 3.0 *
                       (( mode == MODE_INPUT ) ? inputParameters.suggestedLatency : outputParameters.suggestedLatency ));
            if( msec < minDelay ) msec = minDelay;
            printf("msec = %d\n", msec);  /**/
            EXPECT( ((result=Pa_StartStream( stream )) == 0) );
            /* Check to make sure PortAudio is advancing timeStamp. */
            oldStamp = Pa_GetStreamTime(stream);
            Pa_Sleep(msec);
            newStamp = Pa_GetStreamTime(stream);
            printf("oldStamp  = %9.6f, newStamp = %9.6f\n", oldStamp, newStamp ); /**/
            EXPECT( (oldStamp < newStamp) );
            /* Check to make sure callback is decrementing framesLeft. */
            oldFrames = myData.framesLeft;
            Pa_Sleep(msec);
            printf("oldFrames = %lu, myData.framesLeft = %lu\n", oldFrames,  myData.framesLeft ); /**/
            EXPECT( (oldFrames > myData.framesLeft) );
            EXPECT( ((result=Pa_CloseStream( stream )) == 0) );
            stream = NULL;
        }
    }
    return 0;
error:
    if( stream != NULL ) Pa_CloseStream( stream );
    return -1;
}

/*******************************************************************/
static void TestFormats( int mode, PaDeviceIndex deviceID, double sampleRate,
                         int numChannels )
{
    TestAdvance( mode, deviceID, sampleRate, numChannels, paFloat32 );
    TestAdvance( mode, deviceID, sampleRate, numChannels, paInt16 );
    TestAdvance( mode, deviceID, sampleRate, numChannels, paInt32 );
    /* TestAdvance( mode, deviceID, sampleRate, numChannels, paInt24 ); */
}

static void RunQuickTest()
{
    /* TestAdvance( mode, deviceID, sampleRate, numChannels, paFloat32 ); */
    PaDeviceIndex outputDeviceID = Pa_GetDefaultOutputDevice();
    TestAdvance( MODE_OUTPUT, outputDeviceID, 48000, 1, paFloat32 );
    TestAdvance( MODE_OUTPUT, outputDeviceID, 44100, 2, paFloat32 );
    TestAdvance( MODE_OUTPUT, outputDeviceID, 22050, 2, paInt16 );
}

/*******************************************************************
* Try each output device, through its full range of capabilities. */
static void TestDevices( int mode, int allDevices )
{
    int id, jc, i;
    int maxChannels;
    int isDefault;
    const PaDeviceInfo *pdi;
    static double standardSampleRates[] = {  8000.0,  9600.0, 11025.0, 12000.0,
                                            16000.0,          22050.0, 24000.0,
                                            32000.0,          44100.0, 48000.0,
                                                              88200.0, 96000.0,
                                               -1.0 }; /* Negative terminated list. */
    int numDevices = Pa_GetDeviceCount();
    for( id=0; id<numDevices; id++ )            /* Iterate through all devices. */
    {
        pdi = Pa_GetDeviceInfo( id );

        if( mode == MODE_INPUT ) {
            maxChannels = pdi->maxInputChannels;
            isDefault = ( id == Pa_GetDefaultInputDevice());
        } else {
            maxChannels = pdi->maxOutputChannels;
            isDefault = ( id == Pa_GetDefaultOutputDevice());
        }
        if( maxChannels > MAX_TEST_CHANNELS )
            maxChannels = MAX_TEST_CHANNELS;

        if (!allDevices && !isDefault) continue; // skip this device

        for( jc=1; jc<=maxChannels; jc++ )
        {
            printf("\n===========================================================\n");
            printf("            Device = %s\n", pdi->name );
            printf("===========================================================\n");
            /* Try each standard sample rate. */
            for( i=0; standardSampleRates[i] > 0; i++ )
            {
                TestFormats( mode, (PaDeviceIndex)id, standardSampleRates[i], jc );
            }
        }
    }
}

/*******************************************************************/
static void usage( const char *name )
{
    printf("%s [-a]\n", name);
    printf("  -a - Test ALL devices, otherwise just the default devices.\n");
    printf("  -i - Test INPUT only.\n");
    printf("  -o - Test OUTPUT only.\n");
    printf("  -q - Quick test only\n");
    printf("  -? - Help\n");
}

/*******************************************************************/
int main( int argc, char **argv );
int main( int argc, char **argv )
{
    int     i;
    PaError result;
    int     allDevices = 0;
    int     testOutput = 1;
    int     testInput = 1;
    int     quickTest = 1; // FIXME, default 0
    char   *executableName = argv[0];

    /* Parse command line parameters. */
    i = 1;
    while( i<argc )
    {
        char *arg = argv[i];
        if( arg[0] == '-' )
        {
            switch(arg[1])
            {
                case 'a':
                    allDevices = 1;
                    break;
                case 'i':
                    testOutput = 0;
                    break;
                case 'o':
                    testInput = 0;
                    break;
                case 'q':
                    quickTest = 1;
                    break;

                default:
                    printf("Illegal option: %s\n", arg);
                case '?':
                    usage( executableName );
                    exit(1);
                    break;
            }
        }
        else
        {
            printf("Illegal argument: %s\n", arg);
            usage( executableName );
            return 1;

        }
        i += 1;
    }

    EXPECT(sizeof(short) == 2); /* The callback assumes we have 16-bit shorts. */
    EXPECT(sizeof(int) == 4); /* The callback assumes we have 32-bit ints. */
    EXPECT( ((result=Pa_Initialize()) == 0) );

    if (quickTest) {
        printf("\n---- Quick Test ---------------\n");
        RunQuickTest();
    } else {
        if( testOutput )
        {
            printf("\n---- Test OUTPUT ---------------\n");
            TestDevices( MODE_OUTPUT, allDevices );
        }
        if( testInput )
        {
            printf("\n---- Test INPUT ---------------\n");
            TestDevices( MODE_INPUT, allDevices );
        }
    }

error:
    Pa_Terminate();
    printf("QA Report: %d passed, %d failed.\n", gNumPassed, gNumFailed );
    return (gNumFailed > 0) || (gNumPassed == 0);
}
