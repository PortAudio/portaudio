
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


#include <string.h>     /* strlen() */

#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

#include "pa_unix_util.h"
#include "pa_ringbuffer.h"

#include "pa_hostapi_pulseaudio_cb.h"


/* PulseAudio headers */
#include <stdio.h>
#include <string.h>
#include <pulse/pulseaudio.h>

void PulseAudioStreamReadCb(
    pa_stream * s,
    size_t length,
    void *userdata
)
{
    PaPulseAudioStream *l_ptrStream = (PaPulseAudioStream *) userdata;
    PaStreamCallbackTimeInfo timeInfo = { 0, 0, 0 };    /* TODO: IMPLEMENT ME */
    int l_iResult = paContinue;
    long numFrames = 0;
    int i = 0;
    size_t l_lDataSize = 0;
    size_t l_lLocation = 0;
    size_t l_lBufferSize = 0;
    const void *l_ptrSampleData = NULL;

    assert(s);
    assert(length > 0);

    if (l_ptrStream == NULL)
    {
        PA_DEBUG(("Portaudio %s: Stream is null!\n", __FUNCTION__));
        return;
    }

    memset(l_ptrStream->inBuffer, 0x00, PULSEAUDIO_BUFFER_SIZE);


    PaUtil_BeginCpuLoadMeasurement(&l_ptrStream->cpuLoadMeasurer);

    while (pa_stream_readable_size(s) > 0)
    {
        l_ptrSampleData = NULL;

        if (pa_stream_peek(s, &l_ptrSampleData, &l_lDataSize))
        {
            PA_DEBUG(("Portaudio %s: Can't read audio!\n", __FUNCTION__));
            return;

        }

        if (l_lDataSize > 0 && l_ptrSampleData)
        {
            memcpy(l_ptrStream->inBuffer + l_lBufferSize, l_ptrSampleData,
                   l_lDataSize);
            l_lBufferSize += l_lDataSize;
        }
        else
        {
            PA_DEBUG(("Portaudio %s: Can't read!\n", __FUNCTION__));
        }


        pa_stream_drop(s);

    }

    if (l_ptrStream->bufferProcessor.streamCallback != NULL)
    {
        PaUtil_BeginBufferProcessing(&l_ptrStream->bufferProcessor, &timeInfo,
                                     0);
        PaUtil_SetInterleavedInputChannels(&l_ptrStream->bufferProcessor, 0,
                                           l_ptrStream->inBuffer,
                                           l_ptrStream->inSampleSpec.channels);
        PaUtil_SetInputFrameCount(&l_ptrStream->bufferProcessor,
                                  l_lBufferSize / l_ptrStream->inputFrameSize);
        numFrames =
            PaUtil_EndBufferProcessing(&l_ptrStream->bufferProcessor,
                                       &l_iResult);

        PaUtil_EndCpuLoadMeasurement(&l_ptrStream->cpuLoadMeasurer, numFrames);
    }
    else
    {
        PaUtil_WriteRingBuffer(&l_ptrStream->inputRing, l_ptrStream->inBuffer,
                               l_lBufferSize);
        // XXX should check whether all bytes were actually written
    }

    pa_threaded_mainloop_signal(l_ptrStream->mainloop, 0);

    if (l_iResult != paContinue)
    {
        l_ptrStream->isActive = 0;
        return;
    }
}

void PulseAudioStreamWriteCb(
    pa_stream * s,
    size_t length,
    void *userdata
)
{
    PaPulseAudioStream *l_ptrStream = (PaPulseAudioStream *) userdata;
    PaStreamCallbackTimeInfo timeInfo = { 0, 0, 0 };    /* TODO: IMPLEMENT ME */
    int l_iResult = paContinue;
    long numFrames = 0;
    int i = 0;

    assert(s);
    assert(length > 0);

    if (l_ptrStream == NULL)
    {
        PA_DEBUG(("Portaudio %s: Stream is null!\n", __FUNCTION__));
        return;
    }

    memset(l_ptrStream->outBuffer, 0x00, PULSEAUDIO_BUFFER_SIZE);

    if (l_ptrStream->bufferProcessor.streamCallback != NULL)
    {
        PaUtil_BeginBufferProcessing(&l_ptrStream->bufferProcessor, &timeInfo,
                                     0);
        PaUtil_BeginCpuLoadMeasurement(&l_ptrStream->cpuLoadMeasurer);

        PaUtil_SetInterleavedOutputChannels(&l_ptrStream->bufferProcessor, 0,
                                            l_ptrStream->outBuffer,
                                            l_ptrStream->outSampleSpec.
                                            channels);
        PaUtil_SetOutputFrameCount(&l_ptrStream->bufferProcessor,
                                   length / l_ptrStream->outputFrameSize);

        numFrames =
            PaUtil_EndBufferProcessing(&l_ptrStream->bufferProcessor,
                                       &l_iResult);
    }
    else
    {
       /* This Shouldn't happen but we are here so note that and fill audio with silence */
       PA_DEBUG(("Portaudio %s: We are not in callback-mode but we are in callback!\n", __FUNCTION__));
       memset(l_ptrStream->outBuffer, length, 0x00);
    }

    if (l_iResult != paContinue)
    {
        l_ptrStream->isActive = 0;
        return;
    }

    if (pa_stream_write
        (s, l_ptrStream->outBuffer, length, NULL, 0, PA_SEEK_RELATIVE))
    {
        PA_DEBUG(("Portaudio %s: Can't write audio!\n", __FUNCTION__));
    }


    PaUtil_EndCpuLoadMeasurement(&l_ptrStream->cpuLoadMeasurer, numFrames);
    pa_threaded_mainloop_signal(l_ptrStream->mainloop, 0);
}


/* This is left for future use! */
static void PulseAudioStreamSuccessCb(
    pa_stream * s,
    int success,
    void *userdata
)
{
    PA_DEBUG(("Portaudio %s: %d\n", __FUNCTION__, success));
    assert(s);
}

/* This is left for future use! */
void PulseAudioStreamStartedCb(
    pa_stream * stream,
    void *userdata
)
{
    assert(stream);
}


/*
    When CloseStream() is called, the multi-api layer ensures that
    the stream has already been stopped or aborted.
*/
PaError PulseAudioCloseStreamCb(
    PaStream * s
)
{
    PaError result = paNoError;
    PaPulseAudioStream *stream = (PaPulseAudioStream *) s;
    PaPulseAudioHostApiRepresentation *l_ptrPulseAudioHostApi = stream->hostapi;
    pa_operation *l_ptrOperation = NULL;

    if (stream->outStream != NULL && pa_stream_get_state(stream->outStream) == PA_STREAM_READY)
    {
        pa_threaded_mainloop_lock(stream->mainloop);

        /* Then we cancel all writing (if there is any)
         *  and wait for disconnetion (TERMINATION) which
         * can take a while for ethernet connections
         */
        pa_stream_cancel_write(stream->outStream);
        pa_stream_disconnect(stream->outStream);

        pa_threaded_mainloop_unlock(stream->mainloop);
        while (pa_stream_get_state(stream->outStream) != PA_STREAM_TERMINATED)
        {
            usleep(100);
        }

        pa_stream_unref(stream->outStream);
        stream->outStream = NULL;

        PaUtil_FreeMemory(stream->outBuffer);
        stream->outBuffer = NULL;
    }

    if (stream->inStream != NULL && pa_stream_get_state(stream->inStream) == PA_STREAM_READY)
    {
        pa_threaded_mainloop_lock(stream->mainloop);

        /* Then we disconnect stream and wait for
         * Termination
         */
        pa_stream_disconnect(stream->inStream);

        pa_threaded_mainloop_unlock(stream->mainloop);
        while (pa_stream_get_state(stream->inStream) != PA_STREAM_TERMINATED)
        {
            usleep(100);
        }

        pa_stream_unref(stream->inStream);
        stream->inStream = NULL;

        PaUtil_FreeMemory(stream->inBuffer);
        stream->inBuffer = NULL;
    }

    PaUtil_TerminateBufferProcessor(&stream->bufferProcessor);
    PaUtil_TerminateStreamRepresentation(&stream->streamRepresentation);
    PaUtil_FreeMemory(stream);

    stream->isStopped = 1;
    stream->isActive = 0;

    return result;
}


PaError PulseAudioStartStreamCb(
    PaStream * s
)
{
    PaError result = paNoError;
    PaPulseAudioStream *stream = (PaPulseAudioStream *) s;
    int streamStarted = 0;      /* So we can know whether we need to take the stream down */
    PaPulseAudioHostApiRepresentation *l_ptrPulseAudioHostApi = stream->hostapi;
    const char *l_strName = NULL;
    pa_operation *l_ptrOperation = NULL;

    pa_threaded_mainloop_lock(l_ptrPulseAudioHostApi->mainloop);

    /* Ready the processor */
    PaUtil_ResetBufferProcessor(&stream->bufferProcessor);

    stream->latency = 20000;
    stream->underflows = 0;
    stream->bufferAttr.fragsize = (uint32_t) - 1;
    stream->bufferAttr.prebuf = (uint32_t) - 1;

    if (stream->outStream != NULL)
    {
        /* Just keep on trucking if we are just corked*/
        if(pa_stream_get_state(stream->outStream) == PA_STREAM_READY &&
           pa_stream_is_corked(stream->outStream))
        {
           l_ptrOperation = pa_stream_cork(stream->outStream,
                                           0,
                                           NULL,
                                           NULL);
            pa_threaded_mainloop_unlock(l_ptrPulseAudioHostApi->mainloop);

            while (pa_operation_get_state(l_ptrOperation) == PA_OPERATION_RUNNING)
            {
                pa_threaded_mainloop_wait(l_ptrPulseAudioHostApi->mainloop);
            }

            pa_threaded_mainloop_lock(l_ptrPulseAudioHostApi->mainloop);
            pa_operation_unref(l_ptrOperation);
            l_ptrOperation = NULL;
        }
        else
        {
            stream->bufferAttr.maxlength =
                pa_usec_to_bytes(stream->latency, &stream->outSampleSpec);
            stream->bufferAttr.minreq = pa_usec_to_bytes(0, &stream->outSampleSpec);
            stream->bufferAttr.tlength =
                pa_usec_to_bytes(stream->latency, &stream->outSampleSpec);

            PA_UNLESS(stream->outBuffer =
                      PaUtil_AllocateMemory(PULSEAUDIO_BUFFER_SIZE),
                      paInsufficientMemory);

            if (stream->device != paNoDevice)
            {
                PA_DEBUG(("Portaudio %s: %d (%s)\n", __FUNCTION__, stream->device,
                          l_ptrPulseAudioHostApi->pulseaudioDeviceNames[stream->
                                                                        device]));
            }

            pa_stream_connect_playback(stream->outStream,
                                       l_ptrPulseAudioHostApi->
                                       pulseaudioDeviceNames[stream->device],
                                       &stream->bufferAttr,
                                       PA_STREAM_INTERPOLATE_TIMING |
                                       PA_STREAM_ADJUST_LATENCY |
                                       PA_STREAM_AUTO_TIMING_UPDATE |
                                       PA_STREAM_NO_REMIX_CHANNELS |
                                       PA_STREAM_NO_REMAP_CHANNELS, NULL, NULL);

            pa_stream_set_underflow_callback(stream->outStream,
                                             PulseAudioStreamUnderflowCb, stream);

            l_strName = NULL;
        }
    }

    if (stream->inStream != NULL)
    {
        stream->bufferAttr.maxlength =
            pa_usec_to_bytes(stream->latency, &stream->inSampleSpec);
        stream->bufferAttr.minreq = pa_usec_to_bytes(0, &stream->inSampleSpec);
        stream->bufferAttr.tlength =
            pa_usec_to_bytes(stream->latency, &stream->inSampleSpec);

        PA_UNLESS(stream->inBuffer =
                  PaUtil_AllocateMemory(PULSEAUDIO_BUFFER_SIZE),
                  paInsufficientMemory);

        pa_stream_connect_record(stream->inStream,
                                 l_ptrPulseAudioHostApi->
                                 pulseaudioDeviceNames[stream->device],
                                 &stream->bufferAttr,
                                 PA_STREAM_INTERPOLATE_TIMING |
                                 PA_STREAM_ADJUST_LATENCY |
                                 PA_STREAM_AUTO_TIMING_UPDATE |
                                 PA_STREAM_NO_REMIX_CHANNELS |
                                 PA_STREAM_NO_REMAP_CHANNELS);
        pa_stream_set_underflow_callback(stream->inStream,
                                         PulseAudioStreamUnderflowCb, stream);
    }

    pa_threaded_mainloop_unlock(l_ptrPulseAudioHostApi->mainloop);

    if (stream->outStream != NULL || stream->inStream != NULL)
    {
        while (1)
        {
            pa_threaded_mainloop_wait(l_ptrPulseAudioHostApi->mainloop);
            pa_threaded_mainloop_lock(l_ptrPulseAudioHostApi->mainloop);

            if (stream->outStream != NULL)
            {
                if (PA_STREAM_READY == pa_stream_get_state(stream->outStream))
                {
                    stream->isActive = 1;
                    stream->isStopped = 0;
                }
            }

            else if (stream->inStream != NULL)
            {
                if (PA_STREAM_READY == pa_stream_get_state(stream->inStream))
                {
                    stream->isActive = 1;
                    stream->isStopped = 0;
                }
            }

            else
            {
                break;
            }

            if (stream->isActive == 1)
            {
                break;
            }

            pa_threaded_mainloop_unlock(l_ptrPulseAudioHostApi->mainloop);

            usleep(1000);
        }

    }

    else
    {
        goto error;
    }

    // Allways unlock.. so we don't get locked
  end:
    pa_threaded_mainloop_unlock(l_ptrPulseAudioHostApi->mainloop);
    return result;
  error:

    if (streamStarted)
    {
        AbortStreamCb(stream);
    }

    stream->isActive = 0;
    result = paNotInitialized;

    goto end;
}

PaError RealStop(
    PaPulseAudioStream * stream,
    int abort
)
{
    PaError result = paNoError;
    PaPulseAudioHostApiRepresentation *l_ptrPulseAudioHostApi = stream->hostapi;
    pa_operation *l_ptrOperation = NULL;

    pa_threaded_mainloop_lock(l_ptrPulseAudioHostApi->mainloop);

    /* Wait for stream to be stopped */
    stream->isActive = 0;
    stream->isStopped = 1;

    /* We want playback just stop for a while */
    if (stream->outStream != NULL &&
        pa_stream_get_state(stream->outStream) == PA_STREAM_READY &&
        !pa_stream_is_corked(stream->outStream) &&
        !abort)
    {
       l_ptrOperation = pa_stream_cork(stream->outStream,
                                       1,
                                       NULL,
                                       NULL);        

        pa_threaded_mainloop_unlock(l_ptrPulseAudioHostApi->mainloop);
        
        while (pa_operation_get_state(l_ptrOperation) == PA_OPERATION_RUNNING)
        {
            pa_threaded_mainloop_wait(l_ptrPulseAudioHostApi->mainloop);
        }

        pa_threaded_mainloop_lock(l_ptrPulseAudioHostApi->mainloop);

        pa_operation_unref(l_ptrOperation);
        l_ptrOperation = NULL;
    }
        
    pa_threaded_mainloop_unlock(l_ptrPulseAudioHostApi->mainloop);

  error:
    stream->isActive = 0;
    stream->isStopped = 1;

    return result;
}

PaError PulseAudioStopStreamCb(
    PaStream * s
)
{
    return RealStop((PaPulseAudioStream *) s, 0);
}


PaError PulseAudioAbortStreamCb(
    PaStream * s
)
{
    return RealStop((PaPulseAudioStream *) s, 1);
}
