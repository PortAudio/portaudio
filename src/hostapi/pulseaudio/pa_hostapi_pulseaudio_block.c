
/*
 * $Id: pa_hostapi_pulseaudio.c 1668 2011-05-02 17:07:11Z rossb $
 * PulseAudio host to play natively in Linux based systems without
 * ALSA emulation
 *
 * Copyright (c) 2014 Tuukka Pasanen <tuukka.pasanen@ilmi.fi>
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2002 Ross Bencina, Phil Burk
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

/** @file
 @ingroup common_src

 @brief PulseAudio implementation of support for a host API.

 This host API implements PulseAudio support for portaudio
 it has callbackmode and normal write mode support
*/

#include "pa_hostapi_pulseaudio_block.h"

/*
    As separate stream interfaces are used for blocking and callback
    streams, the following functions can be guaranteed to only be called
    for blocking streams.
*/

PaError PulseAudioReadStreamBlock(
    PaStream * s,
    void *buffer,
    unsigned long frames
)
{
    PaPulseAudioStream *l_ptrStream = (PaPulseAudioStream *) s;
    PaPulseAudioHostApiRepresentation *l_ptrPulseAudioHostApi =
        l_ptrStream->hostapi;
    PaError l_iRet = 0;
    size_t l_lReadable = 0;
    uint8_t *l_ptrData = (uint8_t *) buffer;
    long l_lLength = (frames * l_ptrStream->inputFrameSize);

    pa_threaded_mainloop_lock( l_ptrStream->mainloop );
    while (l_lLength > 0)
    {
        long l_read = PaUtil_ReadRingBuffer( &l_ptrStream->inputRing, l_ptrData,
                                             l_lLength );
        l_ptrData += l_read;
        l_lLength -= l_read;
        if ( l_lLength > 0 )
            pa_threaded_mainloop_wait( l_ptrStream->mainloop );
    }
    pa_threaded_mainloop_unlock( l_ptrStream->mainloop );
    return paNoError;
}


PaError PulseAudioWriteStreamBlock(
    PaStream * s,
    const void *buffer,
    unsigned long frames
)
{
    PaPulseAudioStream *l_ptrStream = (PaPulseAudioStream *) s;
    PaPulseAudioHostApiRepresentation *l_ptrPulseAudioHostApi =
        l_ptrStream->hostapi;
    int l_iRet = 0;
    size_t l_lWritable = 0;
    uint8_t *l_ptrData = (uint8_t *) buffer;
    long l_lLength = (frames * l_ptrStream->outputFrameSize);
    pa_operation *l_ptrOperation = NULL;

    PaUtil_BeginCpuLoadMeasurement(&l_ptrStream->cpuLoadMeasurer);

    while (l_lLength > 0)
    {
        pa_threaded_mainloop_lock(l_ptrStream->mainloop);
        l_lWritable = pa_stream_writable_size(l_ptrStream->outStream);
        pa_threaded_mainloop_unlock(l_ptrStream->mainloop);

        if(l_lWritable > 0)
        {
           if(l_lLength < l_lWritable)
           {
                l_lWritable = l_lLength;
           }
           pa_threaded_mainloop_lock(l_ptrStream->mainloop);
           l_iRet = pa_stream_write(l_ptrStream->outStream,
                l_ptrData,
                l_lWritable,
                NULL,
                0,
                PA_SEEK_RELATIVE
            );

            l_ptrOperation = pa_stream_update_timing_info(l_ptrStream->outStream,
                                                          NULL,
                                                          NULL);
                                                          pa_threaded_mainloop_unlock(l_ptrStream->mainloop);

            while (pa_operation_get_state(l_ptrOperation) == PA_OPERATION_RUNNING)
            {
                usleep(100);
            }
            pa_threaded_mainloop_lock(l_ptrStream->mainloop);

            pa_operation_unref(l_ptrOperation);
            l_ptrOperation = NULL;
            pa_threaded_mainloop_unlock(l_ptrStream->mainloop);

            l_ptrData += l_lWritable;
            l_lLength -= l_lWritable;
        }

        if(l_lLength > 0)
        {
            // Sleep small amount of time not burn CPU we block anyway so this is bearable
            usleep(100);
        }

    }
    PaUtil_EndCpuLoadMeasurement(&l_ptrStream->cpuLoadMeasurer, frames);

    return paNoError;
}


signed long PulseAudioGetStreamReadAvailableBlock(
    PaStream * s
)
{
    PaPulseAudioStream *l_ptrStream = (PaPulseAudioStream *) s;

    if (l_ptrStream->inStream == NULL)
    {
        return 0;
    }

    return (PaUtil_GetRingBufferReadAvailable(&l_ptrStream->inputRing) /
            l_ptrStream->inputFrameSize);
}


signed long PulseAudioGetStreamWriteAvailableBlock(
    PaStream * s
)
{
    PaPulseAudioStream *l_ptrStream = (PaPulseAudioStream *) s;
    PaPulseAudioHostApiRepresentation *l_ptrPulseAudioHostApi =
        l_ptrStream->hostapi;

    if (l_ptrStream->outStream == NULL)
    {
        return 0;
    }

    return (PaUtil_GetRingBufferReadAvailable(&l_ptrStream->outputRing) /
            l_ptrStream->outputFrameSize);
}
