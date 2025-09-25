/** @file patest_long_record.c
    @ingroup test_src
    @brief Test whether we can record for many hours without failing.
    @author Phil Burk  http://www.softsynth.com
*/
/*
 * $Id$
 *
 * Authors:
 *    Ross Bencina <rossb@audiomulch.com>
 *    Phil Burk <philburk@mobileer.com>
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
#include <math.h>
#include <stdint.h>
#include "portaudio.h"

static int64_t sCallbackCount = 0;
// This callback will never be called again after a certain period of time
static int listening ( const void*, void*, unsigned long,
          const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void* ) {
    sCallbackCount++;
    return paContinue;
}

int main ( int, char** ) {
    PaStream* stream = NULL;
    PaStreamParameters inputParameters;
    PaError err = 0;
    int loopCount = 0;
    int64_t previousCallbackCount = sCallbackCount;

    Pa_Initialize ();

    inputParameters.device = Pa_GetDefaultInputDevice(); /* default input device */
    printf("Recording using device #%d\n", inputParameters.device);
    inputParameters.channelCount = 1;
    inputParameters.sampleFormat = paInt16;
    inputParameters.suggestedLatency = Pa_GetDeviceInfo( inputParameters.device )->defaultLowInputLatency;
    inputParameters.hostApiSpecificStreamInfo = NULL;

    err = Pa_OpenStream(
              &stream,
              &inputParameters,
              NULL,                  /* &outputParameters, */
              44100,
              512,
              paClipOff,      /* we won't output out of range samples so don't bother clipping them */
              listening, /* callback */
              NULL ); /* no callback userData */
    if( err != paNoError ) {
        printf("Pa_OpenDefaultStream returned %d!\n", err);
        goto error2;
    }
    err = Pa_StartStream ( stream );
    if( err != paNoError ) {
        printf("Pa_StartStream returned %d!\n", err);
        goto error1;
    }

    while ( Pa_IsStreamActive ( stream ) == 1 ) {
        Pa_Sleep ( 1000 );

        if (previousCallbackCount == sCallbackCount) {
            printf("Callbacks stopped!\n");
            goto error1;
        }
        previousCallbackCount = sCallbackCount;

        loopCount++;
        if ((loopCount % 10) == 0) {
            printf("%d loops\n", loopCount);
        }
    }
    printf("Stream no longer Active!");

error1:
    Pa_CloseStream ( stream );
error2:
    Pa_Terminate ();
    return 0;
}
