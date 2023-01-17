/*
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
#include <math.h>

#include "portaudio.h"
#include "paqa_macros.h"

/*--------- Definitions ---------*/
#define SAMPLE_RATE       (44100.0)

#define RUN_TIME_SECONDS  (2)

typedef struct PaQaData
{
    unsigned long  frameCounter;
    unsigned long  framesLeft;
    unsigned long  minFramesPerBuffer;
    unsigned long  maxFramesPerBuffer;
    int            numInputChannels;
    int            numOutputChannels;
    int            bytesPerSample;
    int            mode;
} PaQaData;


/*-------------------------------------------------------------------------*/
static int QaCallback( const void*                      inputBuffer,
                       void*                            outputBuffer,
                       unsigned long                    framesPerBuffer,
                       const PaStreamCallbackTimeInfo*  timeInfo,
                       PaStreamCallbackFlags            statusFlags,
                       void*                            userData )
{
    unsigned long   i;
    unsigned char*  out = (unsigned char *) outputBuffer;
    PaQaData*       data = (PaQaData *) userData;

    (void)inputBuffer; /* Prevent "unused variable" warnings. */

    data->minFramesPerBuffer = (framesPerBuffer < data->minFramesPerBuffer)
            ? framesPerBuffer : data->minFramesPerBuffer;
    data->maxFramesPerBuffer = (framesPerBuffer > data->maxFramesPerBuffer)
            ? framesPerBuffer : data->maxFramesPerBuffer;

    /* Zero out buffer so we don't hear terrible noise. */
    if( data->numOutputChannels > 0 )
    {
        unsigned long numBytes = framesPerBuffer * data->numOutputChannels * data->bytesPerSample;
        for( i=0; i<numBytes; i++ )
        {
            *out++ = 0;
        }
    }
    data->frameCounter += framesPerBuffer;
    /* Are we through yet? */
    if( data->framesLeft > framesPerBuffer )
    {
        data->framesLeft -= framesPerBuffer;
        return 0;
    }
    else
    {
        data->framesLeft = 0;
        return 1;
    }
}

void CheckDefaultCallbackRun(PaStream *stream,
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
    EXPECT((data->frameCounter > (1 * SAMPLE_RATE)));

    if (framesPerBuffer > 0) {
        ASSERT_EQ(framesPerBuffer, data->minFramesPerBuffer);
        ASSERT_EQ(framesPerBuffer, data->maxFramesPerBuffer);
    } else {
        EXPECT((data->minFramesPerBuffer > 0));
        EXPECT((data->maxFramesPerBuffer < SAMPLE_RATE));
    }

    EXPECT((result = Pa_IsStreamActive( stream ) == 0)); // callback terminated it
    EXPECT((result = Pa_IsStreamStopped( stream ) == 0));

    EXPECT((result = Pa_StopStream( stream ) == paNoError));

    EXPECT((result = Pa_IsStreamActive( stream ) == 0));
    EXPECT((result = Pa_IsStreamStopped( stream ) == 1));

    EXPECT((result = Pa_CloseStream( stream ) == paNoError));
error:
    return;
}

void TestDefaultStreamCallback(unsigned long framesPerBuffer,
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
    data.bytesPerSample = sizeof(float);
    data.minFramesPerBuffer = UINT32_MAX;

    CheckDefaultCallbackRun(stream, &data, framesPerBuffer);
error:
    return;
}

/*---------------------------------------------------------------------*/
int main(void);
int main(void)
{
    PaError result;
    const unsigned long kBufferSizes[] = { 0, 256, 960 };
    const int kNumBufferSizes = (int)(sizeof(kBufferSizes)/sizeof(unsigned long));
    const int kChannelCounts[] = { 1, 2 };
    const int kNumChannelCounts = (int)(sizeof(kChannelCounts)/sizeof(int));

    printf("-----------------------------\n");
    printf("paqa_suite - PortAudio QA test\n");
    EXPECT(((result = Pa_Initialize()) == paNoError));

    for (int ib = 0; ib < kNumBufferSizes; ib++) {
        for (int ic = 0; ic < kNumChannelCounts; ic++) {
            TestDefaultStreamCallback(kBufferSizes[ib], 1, 0);
            TestDefaultStreamCallback(kBufferSizes[ib], 0, kChannelCounts[ic]);
            TestDefaultStreamCallback(kBufferSizes[ib], 1, kChannelCounts[ic]);
        }
    }

error:
    Pa_Terminate();
    printf("paqa_suite: %d passed, %d failed.\n", gNumPassed, gNumFailed);
    return (gNumFailed > 0) || (gNumPassed == 0);
}
