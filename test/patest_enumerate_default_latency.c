/** @file patest_enumerate_default_latency.c
    @ingroup test_src
    @brief List default latencies of available devices in table format
    @author Vimal Krishna
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

/*
The purpose of this test is to output data that can be combined into a survey of all host APIs.

The output is printed in textile format and tested on https://textile-lang.com/ .
The columns in the table are as follows:
1. Indicates with an 'X' if any default latencies for available channels are zero, blank otherwise
2. Device Number
3. Number of input/output channels
4. Device Name
5. Host API
6. Default High Input Latency
7. Default Low Input Latency
8. Default High Output Latency
9. Default Low Output latency
*/

#include <stdio.h>
#include <math.h>
#include "portaudio.h"

#ifdef WIN32
#include <windows.h>

#if PA_USE_ASIO
#include "pa_asio.h"
#endif
#endif

int main(void);

#ifdef WIN32
void print_clean_wstring(wchar_t* wstring)
{
    for (int i = 0; i < wcslen(wstring); i++)
    {
        if (isprint(wstring[i]))
        {
            wprintf(L"%c", wstring[i]);
        }
    }
    return;
}
#endif

int main(void)
{
    int     i, numDevices;
    const   PaDeviceInfo *deviceInfo;
    PaError err;

    err = Pa_Initialize();
    if( err != paNoError )
    {
        printf( "ERROR: Pa_Initialize returned 0x%x\n", err );
        goto error;
    }

    numDevices = Pa_GetDeviceCount();
    if( numDevices < 0 )
    {
        printf( "ERROR: Pa_GetDeviceCount returned 0x%x\n", numDevices );
        err = numDevices;
        goto error;
    }

    // Header for the table
    printf("|_. Bad Default Latency? |_. Device Number |_. I/O Channels |_. Device Name |_. Host API |_. Default High Input Latency |_. Default Low Input Latency |_. Default High Output Latency |_. Default Low Output latency |\n");

    for( i=0; i<numDevices; i++ )
    {
        int isLatencyZero = 0;
        deviceInfo = Pa_GetDeviceInfo(i);

        float defaultHighInputLatency = deviceInfo->defaultHighInputLatency;
        float defaultLowInputLatency = deviceInfo->defaultLowInputLatency;
        float defaultHighOutputLatency = deviceInfo->defaultHighOutputLatency;
        float defaultLowOutputLatency = deviceInfo->defaultLowOutputLatency;

        if (deviceInfo->maxInputChannels > 0) {
            if (defaultHighInputLatency == 0 || defaultLowInputLatency == 0) {
                isLatencyZero = 1;
            }
        }

        if (deviceInfo->maxOutputChannels > 0) {
            if (defaultHighOutputLatency == 0 || defaultLowOutputLatency == 0) {
                isLatencyZero = 1;
            }
        }
        char marker = isLatencyZero ? 'X' : ' ';
        printf("| %c | %3d | ", marker, i);
        printf("%d/%d | ", deviceInfo->maxInputChannels, deviceInfo->maxOutputChannels);
#ifdef WIN32
        {   /* Use wide char on windows, so we can show UTF-8 encoded device names */
            wchar_t wideName[MAX_PATH];
            MultiByteToWideChar(CP_UTF8, 0, deviceInfo->name, -1, wideName, MAX_PATH - 1);
            printf(" == ");
            print_clean_wstring(wideName);
            printf(" == | ");
        }
#else
        printf(" == %s == | ", deviceInfo->name);
#endif
        printf("%s | ", Pa_GetHostApiInfo(deviceInfo->hostApi)->name);
        printf("%8.4f | ", defaultHighInputLatency);
        printf("%8.4f | ", defaultLowInputLatency);
        printf("%8.4f | ", defaultHighOutputLatency);
        printf("%8.4f |", defaultLowOutputLatency);
        printf("\n");

    }
    Pa_Terminate();
    return 0;

error:
    Pa_Terminate();
    fprintf( stderr, "Error number: %d\n", err );
    fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
    return err;
}
