/** @file patest_select_hostapis.c
	@ingroup test_src
	@brief Test of Pa_SelectHostApis
	@author Ross Bencina <rossb@audiomulch.com>
*/
/*
 * $Id$
 *
 * This program uses the PortAudio Portable Audio Library.
 * For more information see: http://www.portaudio.com/
 * Copyright (c) 1999-2016 Ross Bencina and Phil Burk
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
#include <assert.h>
#include <stdlib.h>

#include "portaudio.h"

/*******************************************************************/
int main(void);
int main(void)
{
    int i, j, k;
    int availableHostApiCount, scratchHostApiCount;
    int maxHostApiCount;
    PaHostApiTypeId *availableHostApis;
    PaHostApiTypeId *scratchHostApis;
    PaError err;

    maxHostApiCount = Pa_GetAvailableHostApisCount();
    availableHostApis = (PaHostApiTypeId*)malloc( sizeof(PaHostApiTypeId) * maxHostApiCount );
    assert( availableHostApis != NULL );
    scratchHostApis = (PaHostApiTypeId*)malloc( sizeof(PaHostApiTypeId) * maxHostApiCount );
    assert( scratchHostApis != NULL );

    availableHostApiCount = 0;
    err = Pa_GetAvailableHostApis( availableHostApis, maxHostApiCount, &availableHostApiCount );
    assert( err == paNoError );
    assert( availableHostApiCount > 0 );

    printf("available host api type ids:\n");
    for (i = 0; i < availableHostApiCount; ++i )
    {
        printf("%d\n", availableHostApis[i]);
    }

    err = Pa_Initialize();
    assert( err == paNoError );
    Pa_Terminate();

    /* excercise Pa_SelectHostApis and Pa_GetSelectedHostApis */

    /* each API in turn */
    for(i = 0; i < availableHostApiCount; ++i)
    {
        PaHostApiTypeId hostApiType = availableHostApis[i];

        printf("selecting api type %d\n", hostApiType);

        err = Pa_SelectHostApis(&hostApiType, 1);
        assert( err == paNoError );

        /* read back and verify selected apis*/
        err = Pa_GetSelectedHostApis( scratchHostApis, maxHostApiCount, &scratchHostApiCount );
        assert( err == paNoError );
        assert( scratchHostApiCount == 1 );
        assert( scratchHostApis[0] == hostApiType );

        err = Pa_Initialize();
        assert( err == paNoError );

        /* verify that all devices match the selected API */

        for(j = 0; j < Pa_GetDeviceCount(); ++j)
        {
            const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(j);
            assert( deviceInfo != NULL );
            assert( Pa_GetHostApiInfo(deviceInfo->hostApi)->type == hostApiType );
        }

        Pa_Terminate();
    }

    /* i counts 1 .. n simultaneously selected APIs */
    for(i = 1; i < availableHostApiCount; ++i)
    {
        printf("selecting %d apis\n", i);

        err = Pa_SelectHostApis(availableHostApis, i);
        assert( err == paNoError );

        /* read back and verify selected apis*/
        err = Pa_GetSelectedHostApis( scratchHostApis, maxHostApiCount, &scratchHostApiCount );
        assert( err == paNoError );
        assert( scratchHostApiCount == i );
        for( j = 0; j < i; ++j )
            assert( scratchHostApis[j] == availableHostApis[j] );

        err = Pa_Initialize();
        assert( err == paNoError );

        /* verify that all devices match one of the selected APIs */

        for(j = 0; j < Pa_GetDeviceCount(); ++j)
        {
            const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(j);
            assert( deviceInfo != NULL );

            /* search for whether the device's host API is one of the selected host APIs */
            err = paHostApiNotFound;
            for(k = 0; k < i; ++k)
            {
                PaHostApiTypeId hostApiType = availableHostApis[k];
                if( Pa_GetHostApiInfo(deviceInfo->hostApi)->type == hostApiType )
                {
                    err = paNoError;
                    break;
                }
            }

            assert( err != paHostApiNotFound );
        }

        Pa_Terminate();
    }

    free(availableHostApis);
    free(scratchHostApis);
}
