/** @file paqa_latency_monotonic.c
    @ingroup qa_src
    @brief Test reported stream latencies for monotonicity constraints.
    @author Ross Bencina <rossb@audiomulch.com>
    @author Phil Burk <philburk@softsynth.com>
    @see test/patest_suggested_vs_streaminfo_latency.c, test/patest_suggested_vs_streaminfo_latency.py
*/
/*
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
#include "loopback/src/qa_tools.h"

#define NUM_SECONDS   (5)
#define SAMPLE_RATE   (44100)
#define FRAMES_PER_BUFFER  (64)

/* Used to tally the results of the QA tests. */
int g_testsPassed = 0;
int g_testsFailed = 0;

/*******************************************************************/
static int paqaNoopCallback( const void *inputBuffer, void *outputBuffer,
                          unsigned long framesPerBuffer,
                          const PaStreamCallbackTimeInfo* timeInfo,
                          PaStreamCallbackFlags statusFlags,
                          void *userData )
{
    (void)inputBuffer;
    (void)outputBuffer;
    (void)framesPerBuffer;
    (void)timeInfo;
    (void)statusFlags;
    (void)userData;
    return paContinue;
}

#define INDENT "  "
/*******************************************************************/
static int paqaCheckStreamLatencyIsMonotonic(
        PaDeviceIndex deviceIndex, int isInput, int bufferSize )
{
    int i;
    int numLoops;
    PaError err;
    PaStream *stream;
    PaStreamParameters streamParameters;
    const PaStreamInfo* streamInfo;
    double lowLatency;
    double highLatency;
    double reportedLatency;
    double sampleRate = SAMPLE_RATE;
    const PaDeviceInfo *pdi = Pa_GetDeviceInfo( deviceIndex );
    double previousLatency = 0.0;
    int numChannels = 1;
    int atMaximumLatency = 0; /* Set to 1 when we reach the limit. */
    double detectedMaximumLatency = 0.0;

    printf("-------------paqaCheckMultipleSuggested -- %s -- bufferSize = %d ------------\n",
           (isInput ? "INPUT" : "OUTPUT"), bufferSize );
    if( isInput )
    {
        lowLatency = pdi->defaultLowInputLatency;
        highLatency = pdi->defaultHighInputLatency;
        numChannels = (pdi->maxInputChannels < 2) ? 1 : 2;
    }
    else
    {
        lowLatency = pdi->defaultLowOutputLatency;
        highLatency = pdi->defaultHighOutputLatency;
        numChannels = (pdi->maxOutputChannels < 2) ? 1 : 2;
    }
    streamParameters.channelCount = numChannels;
    streamParameters.device = deviceIndex;
    streamParameters.hostApiSpecificStreamInfo = NULL;
    streamParameters.sampleFormat = paFloat32;
    sampleRate = pdi->defaultSampleRate;

    printf(INDENT "lowLatency   = %g\n", lowLatency );
    printf(INDENT "highLatency  = %g\n", highLatency );
    printf(INDENT "numChannels  = %d\n", numChannels );
    printf(INDENT "sampleRate   = %g Hz\n", sampleRate );
    printf(INDENT "samplePeriod = %e seconds\n", 1.0 / sampleRate );

    numLoops = ( highLatency < 0.001 )
            ? 2 /* test suggestedLatency 0 and highLatency. note numLoops must be > 0 to avoid divzero */
            : 11; /* test 0 to 10 inclusive. */

    for( i=0; i<numLoops; i++ )
    {
        streamParameters.suggestedLatency = highLatency * i /(numLoops - 1);
        printf(INDENT "suggested[%2d] = %8.6f", i, streamParameters.suggestedLatency );

        err = Pa_OpenStream(
                &stream,
                (isInput ? &streamParameters : NULL),
                (isInput ? NULL : &streamParameters),
                sampleRate,
                bufferSize,
                paClipOff,      /* we won't output out of range samples so don't bother clipping them */
                paqaNoopCallback,
                NULL );
        if( err != paNoError ) {
            printf("\n");
            goto error;
        }

        streamInfo = Pa_GetStreamInfo( stream );
        /* Get the latency from the streamInfo now because it will be invalid after the
         * stream is closed. */
        reportedLatency = (isInput) ? streamInfo->inputLatency : streamInfo->outputLatency;
        printf(", reported = %8.6f", reportedLatency );
        printf(", (rep - sug) = %11.4e sec", (reportedLatency - streamParameters.suggestedLatency) );
        printf(" = %8.2f fr\n", (reportedLatency - streamParameters.suggestedLatency) * SAMPLE_RATE );
        err = Pa_CloseStream( stream );

        QA_ASSERT_TRUE("Latency should be > 0.0", reportedLatency > 0.0);
        QA_ASSERT_TRUE("Latency should be monotonically non-decreasing with suggested latency.",
                reportedLatency >= previousLatency);

        /* The state machine below checks that the reported stream latency
         * remains at or above the suggested latency until the point at which
         * the reported stream latency clamps at its maximum.
         *
         * latency ^                 .
         *   value |           ___._______
         *         |          /.
         *         |    _- '.                 .  .  .  suggested latency
         *         |   / .                    _______  reported ("final") latency
         *         | -.
         *         '--------------------->
         *        0         loop iteration
         */
        if (atMaximumLatency == 0) {
            /* When not yet at the maximum, interpret a reported stream latency
             * that is less than the suggested latency as clipping at max. */
            if (reportedLatency < streamParameters.suggestedLatency) {
                atMaximumLatency = 1;
                detectedMaximumLatency = reportedLatency;
                printf("     detectedMaximumLatency = %8.6f\n", detectedMaximumLatency );
            }
        }
        /* Now check that reported latency behaves correctly: */
        if (atMaximumLatency == 0) {
            /* Below the maximum, stream should always round latency up. */
            QA_ASSERT_TRUE("Latency should be >= suggestedLatency",
                           reportedLatency >= streamParameters.suggestedLatency);
        } else {
            /* Once the maximum has been reached, reported latency should remain constant. */
            QA_ASSERT_TRUE("Latency should be == detectedMaximumLatency",
                           reportedLatency == detectedMaximumLatency);
        }
        previousLatency = reportedLatency;
    }

    return 0;
error:
    return -1;
}

/*******************************************************************/
static int paqaVerifyMonotonicLatency( void )
{
    PaDeviceIndex id;
    int result = 0;
    const PaDeviceInfo *pdi;
    int numDevices = Pa_GetDeviceCount();
    int bufferSizes[] = {paFramesPerBufferUnspecified, 48, 64, 96, 128, 256, 512, 1024};
    const int numBufferSizes = sizeof(bufferSizes) / sizeof(int);
    int sizeIndex;
    int bufferSize;

    for( id=0; id<numDevices; id++ )            /* Iterate through all devices. */
    {
        pdi = Pa_GetDeviceInfo( id );
        printf("\n=============== Using device #%d: '%s' (%s) ==================\n",
               id, pdi->name, Pa_GetHostApiInfo(pdi->hostApi)->name);

        for (sizeIndex = 0; sizeIndex < numBufferSizes; sizeIndex++) {
            bufferSize = bufferSizes[sizeIndex];
            if( pdi->maxOutputChannels > 0 )
            {
                if( paqaCheckStreamLatencyIsMonotonic( id, 0 /* isInput */, bufferSize ) < 0 )
                {
                    printf("OUTPUT CHECK FAILED !!! #%d: '%s'\n", id, pdi->name);
                    result -= 1;
                }
            }
            if( pdi->maxInputChannels > 0 )
            {
                if( paqaCheckStreamLatencyIsMonotonic( id, 1 /* isInput */, bufferSize ) < 0 )
                {
                    printf("INPUT CHECK FAILED !!! #%d: '%s'\n", id, pdi->name);
                    result -= 1;
                }
            }
        }
    }
    return result;
}

/*******************************************************************/
int main(void);
int main(void)
{
    PaError err;

    printf("\nPortAudio QA: check monotonic behavior of the reported latency.\n");

    err = Pa_Initialize();
    if( err != paNoError ) goto error;

    if( (err = paqaVerifyMonotonicLatency()) < 0 ) goto error;

    Pa_Terminate();
    printf("------------- SUMMARY ---------------------\n");
    printf("SUCCESS - test finished.\n");
    return 0;

error:
    Pa_Terminate();
    printf("------------- SUMMARY ---------------------\n");
    fprintf( stderr, "ERROR - test failed.\n" );
    fprintf( stderr, "Error number: %d\n", err );
    fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
    return -err; /* exit codes are truncated to an unsigned byte */
}
