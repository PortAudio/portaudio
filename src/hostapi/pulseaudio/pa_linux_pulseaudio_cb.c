
/*
 * PulseAudio host to play natively in Linux based systems without
 * ALSA emulation
 *
 * Copyright (c) 2014-2016 Tuukka Pasanen <tuukka.pasanen@ilmi.fi>
 * Copyright (c) 2016 Sqweek <sqweek@gmail.com>
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
 it has callback mode and normal write mode support
*/


#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

#include "pa_unix_util.h"
#include "pa_ringbuffer.h"

#include "pa_linux_pulseaudio_cb.h"


/* PulseAudio headers */
#include <string.h>
#include <unistd.h>

void PaPulseAudio_updateTimeInfo(
  pa_stream * s,
  PaStreamCallbackTimeInfo *timeInfo,
  int record
)
{
  unsigned int l_iNegative = 0;
  pa_usec_t l_lStreamTime = 0;
  pa_usec_t l_lStreamLatency = 0;

  if (pa_stream_get_time(s, &l_lStreamTime) ==
      -PA_ERR_NODATA)
  {
      PA_DEBUG(("Portaudio %s: No time available!\n", __FUNCTION__));
  }
  else
  {
    timeInfo->currentTime = ((float) l_lStreamTime / (float) 1000000);
  }

  if (pa_stream_get_latency(s, &l_lStreamLatency, &l_iNegative) ==
      -PA_ERR_NODATA)
  {
      PA_DEBUG(("Portaudio %s: No latency available!\n", __FUNCTION__));
  }
  else
  {
      if(record == 0)
      {
         timeInfo->outputBufferDacTime = ((float) l_lStreamLatency / (float) 1000000);
      }
      else
      {
        timeInfo->inputBufferAdcTime = ((float) l_lStreamLatency / (float) 1000000);
      }
  }

}


void PaPulseAudio_StreamRecordCb(
    pa_stream * s,
    size_t length,
    void *userdata
)
{
    PaPulseAudio_Stream *l_ptrStream = (PaPulseAudio_Stream *) userdata;
    PaStreamCallbackTimeInfo timeInfo = { 0, 0, 0 };    /* TODO: IMPLEMENT ME */
    int l_iResult = paContinue;
    long numFrames = 0;
    int i = 0;
    size_t l_lDataSize = 0;
    size_t l_lLocation = 0;
    size_t l_lBufferSize = 0;
    const void *l_ptrSampleData = NULL;


    if (l_ptrStream == NULL)
    {
        PA_DEBUG(("Portaudio %s: Stream is null!\n", __FUNCTION__));
        return;
    }


    memset(l_ptrStream->inBuffer, 0x00, PULSEAUDIO_BUFFER_SIZE);

    // Stream ain't active yet.
    // Don't call callback before we have settled down
    if(!l_ptrStream->isActive)
    {
      l_iResult = paComplete;
    }
    else
    {
      PaUtil_BeginCpuLoadMeasurement(&l_ptrStream->cpuLoadMeasurer);
    }

    PaPulseAudio_updateTimeInfo(s, &timeInfo, 1);

    // If stream activated or not.. we have to
    // read what there is coming to memory
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

    if(l_ptrStream->isActive)
    {
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
        }
        else
        {
            PaUtil_WriteRingBuffer(&l_ptrStream->inputRing, l_ptrStream->inBuffer,
                                   l_lBufferSize);
            // XXX should check whether all bytes were actually written
        }

        PaUtil_EndCpuLoadMeasurement(&l_ptrStream->cpuLoadMeasurer, numFrames);
    }

    if (l_iResult != paContinue)
    {
      // Eventually notify user all buffers have played
    	if (l_ptrStream->streamRepresentation.streamFinishedCallback && l_ptrStream->isActive)
    	{
    	   l_ptrStream->streamRepresentation.streamFinishedCallback(l_ptrStream->streamRepresentation.userData);
    	}

      l_ptrStream->isActive = 0;
    }

    pa_threaded_mainloop_signal(l_ptrStream->mainloop, 0);
}

void PaPulseAudio_StreamPlaybackCb(
    pa_stream * s,
    size_t length,
    void *userdata
)
{
    PaPulseAudio_Stream *l_ptrStream = (PaPulseAudio_Stream *) userdata;
    PaStreamCallbackTimeInfo timeInfo = { 0, 0, 0 };    /* TODO: IMPLEMENT ME */
    int l_iResult = paContinue;
    long numFrames = 0;
    unsigned int i = 0;

    if (l_ptrStream == NULL)
    {
        PA_DEBUG(("Portaudio %s: Stream is null!\n", __FUNCTION__));
        return;
    }

    // Stream ain't active yet.
    // Don't call callback before we have settled down
    if(!l_ptrStream->isActive)
    {
      l_iResult = paComplete;
    }

    memset(l_ptrStream->outBuffer, 0x00, PULSEAUDIO_BUFFER_SIZE);

    if(l_ptrStream->outputChannelCount == 1)
    {
        length /= 2;
    }

    PaPulseAudio_updateTimeInfo(s, &timeInfo, 0);

    if (l_ptrStream->bufferProcessor.streamCallback != NULL && l_ptrStream->isActive)
    {
        PaUtil_BeginBufferProcessing(&l_ptrStream->bufferProcessor, &timeInfo,
                                     0);
        PaUtil_BeginCpuLoadMeasurement(&l_ptrStream->cpuLoadMeasurer);

        PaUtil_SetInterleavedOutputChannels(&l_ptrStream->bufferProcessor,
                                            0,
                                            l_ptrStream->outBuffer,
                                            l_ptrStream->outputChannelCount);
        PaUtil_SetOutputFrameCount(&l_ptrStream->bufferProcessor,
                                   length / l_ptrStream->outputFrameSize);

        numFrames =
            PaUtil_EndBufferProcessing(&l_ptrStream->bufferProcessor,
                                       &l_iResult);

        /* We can't get as much we want let's calculate new size */
        if(numFrames != (length / l_ptrStream->outputFrameSize))
        {
            fprintf(stderr, "WANTED %d and got %d \n", numFrames, (length / l_ptrStream->outputFrameSize));
            length = numFrames * l_ptrStream->outputFrameSize;
        }
    }
    else if(!l_ptrStream->bufferProcessor.streamCallback && l_ptrStream->isActive)
    {
       /* This Shouldn't happen but we are here so note that and fill audio with silence */
       PA_DEBUG(("Portaudio %s: We are not in callback-mode but we are in callback!\n", __FUNCTION__));
    }

    // Stream callback wants this to end so
    // We are not allowed to call it again
    // isActive marks for that
    if (l_iResult != paContinue)
    {
        // Eventually notify user all buffers have played
        if (l_ptrStream->streamRepresentation.streamFinishedCallback && l_ptrStream->isActive)
        {
                  l_ptrStream->streamRepresentation.streamFinishedCallback(l_ptrStream->streamRepresentation.userData);
        }

        l_ptrStream->isActive = 0;
    }

    // If mono we assume to have stereo output
    // So we just copy to other channel..
    // Little bit hackish but works.. with float currently
    if(l_ptrStream->outputChannelCount == 1)
    {
        void *l_ptrStartOrig = l_ptrStream->outBuffer + length;
        void *l_ptrStartStereo = l_ptrStream->outBuffer;
        memcpy(l_ptrStartOrig, l_ptrStartStereo, length);

        for(i = 0; i < length; i += l_ptrStream->outputFrameSize)
        {
            memcpy(l_ptrStartStereo, l_ptrStartOrig, l_ptrStream->outputFrameSize);
            l_ptrStartStereo += l_ptrStream->outputFrameSize;
            memcpy(l_ptrStartStereo, l_ptrStartOrig, l_ptrStream->outputFrameSize);
            l_ptrStartStereo += l_ptrStream->outputFrameSize;
            l_ptrStartOrig += l_ptrStream->outputFrameSize;
        }
        length *= 2;
        memcpy(l_ptrStartStereo, l_ptrStartOrig, length);
    }

    if (pa_stream_write(s, l_ptrStream->outBuffer, length, NULL, 0, PA_SEEK_RELATIVE))
    {
        PA_DEBUG(("Portaudio %s: Can't write audio!\n", __FUNCTION__));
    }

    if (l_ptrStream->isActive)
    {
        PaUtil_EndCpuLoadMeasurement(&l_ptrStream->cpuLoadMeasurer, numFrames);
    }

    pa_threaded_mainloop_signal(l_ptrStream->mainloop, 0);
}


/* This is left for future use! */
static void PaPulseAudio_StreamSuccessCb(
    pa_stream * s,
    int success,
    void *userdata
)
{
    PA_DEBUG(("Portaudio %s: %d\n", __FUNCTION__, success));
}

/* This is left for future use! */
static void PaPulseAudio_CorkSuccessCb(
    pa_stream * s,
    int success,
    void *userdata
)
{
    PaPulseAudio_Stream *l_ptrStream = (PaPulseAudio_Stream *) userdata;
    pa_threaded_mainloop_signal(l_ptrStream->mainloop, 0);
}


/* This is left for future use! */
void PaPulseAudio_StreamStartedCb(
    pa_stream * stream,
    void *userdata
)
{
    PaPulseAudio_Stream *l_ptrStream = (PaPulseAudio_Stream *) userdata;
    pa_threaded_mainloop_signal(l_ptrStream->mainloop, 0);
}


/*
    When CloseStream() is called, the multi-api layer ensures that
    the stream has already been stopped or aborted.
*/
PaError PaPulseAudio_CloseStreamCb(
    PaStream * s
)
{
    PaError result = paNoError;
    PaPulseAudio_Stream *stream = (PaPulseAudio_Stream *) s;
    PaPulseAudio_HostApiRepresentation *l_ptrPulseAudioHostApi = stream->hostapi;
    pa_operation *l_ptrOperation = NULL;
    int l_iLoop = 0;
    int l_iError = 0;

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
    }

    if (stream->inStream != NULL && pa_stream_get_state(stream->inStream) == PA_STREAM_READY)
    {
        pa_threaded_mainloop_lock(stream->mainloop);

        /* Then we disconnect stream and wait for
         * Termination
         */
        pa_stream_disconnect(stream->inStream);

        pa_threaded_mainloop_unlock(stream->mainloop);

    }

    /* Wait for termination for both */
    while(!l_iLoop)
    {
        if (stream->inStream != NULL && pa_stream_get_state(stream->inStream) == PA_STREAM_TERMINATED)
        {
            pa_stream_unref(stream->inStream);
            stream->inStream = NULL;

            PaUtil_FreeMemory(stream->inBuffer);
            stream->inBuffer = NULL;

        }

        if (stream->outStream != NULL && pa_stream_get_state(stream->outStream) == PA_STREAM_TERMINATED)
        {
            pa_stream_unref(stream->outStream);
            stream->outStream = NULL;

            PaUtil_FreeMemory(stream->outBuffer);
            stream->outBuffer = NULL;
        }

        if((stream->outStream == NULL && stream->inStream == NULL) || l_iError >= 5000)
        {
              l_iLoop = 1;
        }

        l_iError ++;
        usleep(100);
    }

    PaUtil_TerminateBufferProcessor(&stream->bufferProcessor);
    PaUtil_TerminateStreamRepresentation(&stream->streamRepresentation);
    PaUtil_FreeMemory(stream);

    return result;
}


PaError PaPulseAudio_StartStreamCb(
    PaStream * s
)
{
    PaError result = paNoError;
    PaPulseAudio_Stream *stream = (PaPulseAudio_Stream *) s;
    int streamStarted = 0;      /* So we can know whether we need to take the stream down */
    PaPulseAudio_HostApiRepresentation *l_ptrPulseAudioHostApi = stream->hostapi;
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
                                           PaPulseAudio_CorkSuccessCb,
                                           stream);

            while (pa_operation_get_state(l_ptrOperation) == PA_OPERATION_RUNNING)
            {
                pa_threaded_mainloop_wait(l_ptrPulseAudioHostApi->mainloop);
            }

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

            if (stream->outDevice != paNoDevice)
            {
                PA_DEBUG(("Portaudio %s: %d (%s)\n", __FUNCTION__, stream->outDevice,
                          l_ptrPulseAudioHostApi->pulseaudioDeviceNames[stream->
                                                                        outDevice]));
            }

            pa_stream_connect_playback(stream->outStream,
                                       l_ptrPulseAudioHostApi->
                                       pulseaudioDeviceNames[stream->outDevice],
                                       &stream->bufferAttr,
                                       PA_STREAM_INTERPOLATE_TIMING |
                                       PA_STREAM_ADJUST_LATENCY |
                                       PA_STREAM_AUTO_TIMING_UPDATE |
                                       PA_STREAM_NO_REMIX_CHANNELS |
                                       PA_STREAM_NO_REMAP_CHANNELS, NULL, NULL);

            pa_stream_set_underflow_callback(stream->outStream,
                                             PaPulseAudio_StreamUnderflowCb, stream);
            pa_stream_set_started_callback(stream->outStream,
                                             PaPulseAudio_StreamStartedCb, stream);

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

        if (stream->inDevice != paNoDevice)
        {
            PA_DEBUG(("Portaudio %s: %d (%s)\n", __FUNCTION__, stream->inDevice,
                      l_ptrPulseAudioHostApi->pulseaudioDeviceNames[stream->
                                                                    inDevice]));
        }

        pa_stream_connect_record(stream->inStream,
                                 l_ptrPulseAudioHostApi->
                                 pulseaudioDeviceNames[stream->inDevice],
                                 &stream->bufferAttr,
                                 PA_STREAM_INTERPOLATE_TIMING |
                                 PA_STREAM_ADJUST_LATENCY |
                                 PA_STREAM_AUTO_TIMING_UPDATE |
                                 PA_STREAM_NO_REMIX_CHANNELS |
                                 PA_STREAM_NO_REMAP_CHANNELS);
        pa_stream_set_underflow_callback(stream->inStream,
                                         PaPulseAudio_StreamUnderflowCb, stream);

        pa_stream_set_started_callback(stream->inStream,
                                       PaPulseAudio_StreamStartedCb, stream);

    }

    pa_threaded_mainloop_unlock(l_ptrPulseAudioHostApi->mainloop);

    if (stream->outStream != NULL || stream->inStream != NULL)
    {
        while (1)
        {
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
        PaPulseAudio_AbortStreamCb(stream);
    }

    stream->isActive = 0;
    result = paNotInitialized;

    goto end;
}

PaError RequestStop(
    PaPulseAudio_Stream * stream,
    int abort
)
{
    PaError result = paNoError;
    PaPulseAudio_HostApiRepresentation *l_ptrPulseAudioHostApi = stream->hostapi;
    pa_operation *l_ptrOperation = NULL;

    pa_threaded_mainloop_lock(l_ptrPulseAudioHostApi->mainloop);

    /* Wait for stream to be stopped */
    stream->isActive = 0;
    stream->isStopped = 1;

    /* Test if there is something that we can play */
    if (stream->outStream != NULL &&
        pa_stream_get_state(stream->outStream) == PA_STREAM_READY &&
        !pa_stream_is_corked(stream->outStream) &&
        !abort)
    {
       l_ptrOperation = pa_stream_cork(stream->outStream,
                                       1,
                                       PaPulseAudio_CorkSuccessCb,
                                       stream);

        while (pa_operation_get_state(l_ptrOperation) == PA_OPERATION_RUNNING)
        {
            pa_threaded_mainloop_wait(l_ptrPulseAudioHostApi->mainloop);
        }

        pa_operation_unref(l_ptrOperation);

        l_ptrOperation = NULL;
    }

  error:
    pa_threaded_mainloop_unlock(l_ptrPulseAudioHostApi->mainloop);
    stream->isActive = 0;
    stream->isStopped = 1;

    return result;
}

PaError PaPulseAudio_StopStreamCb(
    PaStream * s
)
{
    return RequestStop((PaPulseAudio_Stream *) s, 0);
}


PaError PaPulseAudio_AbortStreamCb(
    PaStream * s
)
{
    return RequestStop((PaPulseAudio_Stream *) s, 1);
}
