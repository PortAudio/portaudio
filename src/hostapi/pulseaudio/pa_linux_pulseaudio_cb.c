
/*
 * PulseAudio host to play natively in Linux based systems without
 * ALSA emulation
 *
 * Copyright (c) 2014-2022 Tuukka Pasanen <tuukka.pasanen@ilmi.fi>
 * Copyright (c) 2016 Sqweek
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

#include "pa_linux_pulseaudio_cb_internal.h"


/* PulseAudio headers */
#include <string.h>
#include <unistd.h>

void PaPulseAudio_updateTimeInfo( pa_stream * s,
                                  PaStreamCallbackTimeInfo *timeInfo,
                                  int record )
{
  unsigned int l_iNegative = 0;
  pa_usec_t l_lStreamTime = 0;
  pa_usec_t l_lStreamLatency = 0;

  if( pa_stream_get_time( s,
                          &l_lStreamTime ) == -PA_ERR_NODATA )
  {
      PA_DEBUG( ("Portaudio %s: No time available!\n",
                __FUNCTION__) );
  }
  else
  {
    timeInfo->currentTime = ((PaTime) l_lStreamTime / (PaTime) 1000000);
  }

  if( pa_stream_get_latency( s,
                             &l_lStreamLatency,
                             &l_iNegative ) == -PA_ERR_NODATA )
  {
      PA_DEBUG( ("Portaudio %s: No latency available!\n",
                __FUNCTION__) );
  }
  else
  {
      if( record == 0 )
      {
          timeInfo->outputBufferDacTime = timeInfo->currentTime + ((PaTime) l_lStreamLatency / (PaTime) 1000000);
      }
      else
      {
          timeInfo->inputBufferAdcTime = timeInfo->currentTime - ((PaTime) l_lStreamLatency / (PaTime) 1000000);
      }
  }
}


/* locks the Pulse Main loop when not called from it */
void PaPulseAudio_Lock( pa_threaded_mainloop *mainloop )
{
    if( !pa_threaded_mainloop_in_thread( mainloop ) ) {
        pa_threaded_mainloop_lock( mainloop );
    }
}

/* unlocks the Pulse Main loop when not called from it */
void PaPulseAudio_UnLock( pa_threaded_mainloop *mainloop )
{
    if( !pa_threaded_mainloop_in_thread( mainloop ) ) {
        pa_threaded_mainloop_unlock( mainloop );
    }
}

int _PaPulseAudio_processAudioInputOutput( PaPulseAudio_Stream *stream,
                                           size_t writableBytes,
                                           size_t readableBytes )
{
    PaStreamCallbackTimeInfo timeInfo = { 0, 0, 0 };
    size_t l_lOutFrameBytes = 0;
    size_t l_lInFrameBytes = 0;
    int l_iResult = paContinue;
    long numFrames = 0;
    int i = 0;

    uint32_t l_lFramesPerHostBuffer = stream->bufferProcessor.framesPerHostBuffer;

    int32_t l_lBytesLeft = 0;
    size_t l_lBytesToProcess = 0;

    void *l_vBuffer = NULL;
    uint8_t l_cBUffer[PULSEAUDIO_BUFFER_SIZE];

    int l_bOutputCb = 0;
    int l_bInputCb = 0;

    /* If we just want to have input but not output (Not Duplex)
     * Use this calculation
     */
    if( stream->inStream && readableBytes)
    {
        if( l_lFramesPerHostBuffer == paFramesPerBufferUnspecified )
        {
            l_lFramesPerHostBuffer = readableBytes / stream->inputFrameSize;
        }

        l_lInFrameBytes = (l_lFramesPerHostBuffer * stream->inputFrameSize);

        l_lBytesToProcess = l_lBytesLeft = PaUtil_GetRingBufferReadAvailable(&stream->inputRing);
    }

    if(  stream->inStream && stream->bufferProcessor.streamCallback )
    {
        l_bInputCb = 1;
    }

    if( stream->outStream )
    {
        if( l_lFramesPerHostBuffer == paFramesPerBufferUnspecified )
        {
            l_lFramesPerHostBuffer = writableBytes / stream->outputFrameSize;
        }

        l_lInFrameBytes = l_lOutFrameBytes = (l_lFramesPerHostBuffer * stream->outputFrameSize);

        /* Get buffer to write straight to pulseaudio output stream
           we need to calculate correct size with how manyt times
           our l_lOutFrameBytes goes to needed output */

        l_lBytesToProcess = l_lBytesLeft = (1 + (writableBytes / l_lOutFrameBytes)) * l_lOutFrameBytes;

        if( stream->bufferProcessor.streamCallback )
        {
            l_bOutputCb = 1;
        }
    }

    /*
     * This is for Portaudio Duplex
     *
     * As first Playback is initialized and after that callback
     * will called. We have to feed to input same amount or Portaudio
     * will assert.
     *
     * We are not initialized yet but we have to output leght (or over it)
     * or else playback callback will be paralyzed...
     */
    if( !stream->isActive &&
        (stream->inStream && stream->outStream) &&
        writableBytes )
    {
        memset(l_cBUffer, 0x00, PULSEAUDIO_BUFFER_SIZE);
        PaUtil_WriteRingBuffer( &stream->inputRing,
                                l_cBUffer,
                                l_lBytesToProcess);
    }


    /* We have to fullfill reading or writing callback or it does not ask
     * anymore. So there can come many reads before it's fullfilled
     */
    do {
        /* There is only Record stream so
         * see if we have enough stuff to feed record stream
         * If not then bail out.
         */
        if( (l_bInputCb && !l_bOutputCb) &&
            PaUtil_GetRingBufferReadAvailable(&stream->inputRing) < l_lInFrameBytes )
        {
            break;
        }

        if(  stream->outStream )
        {
            PaPulseAudio_updateTimeInfo( stream->outStream,
                                         &timeInfo,
                                         0 );
        }

        if(  stream->inStream )
        {
            PaPulseAudio_updateTimeInfo( stream->inStream,
                                         &timeInfo,
                                         1 );
        }

        PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );

        /* When doing Portaudio Duplex one has to write and read same amount of data
         * if not done that way Portaudio will go boo boo and nothing works.
         * This is why this is done as it's done
         *
         * Pulseaudio does not care and this is something that needs small adjusting
         */
        PaUtil_BeginBufferProcessing( &stream->bufferProcessor,
                                      &timeInfo,
                                      0 );

        /* Read of ther is something to read */
        if( l_bInputCb )
        {
            PaUtil_ReadRingBuffer(&stream->inputRing,
                                  l_cBUffer,
                                  l_lInFrameBytes);

            PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor,
                                                0,
                                                l_cBUffer,
                                                stream->inSampleSpec.channels );

            PaUtil_SetInputFrameCount( &stream->bufferProcessor,
                                       l_lFramesPerHostBuffer );

            if(!l_bOutputCb && l_lBytesLeft)
            {
                l_lBytesLeft -= l_lInFrameBytes;
            }
        }

        if( l_bOutputCb )
        {
            PaPulseAudio_Lock( stream->mainloop );
            if( ! pa_stream_begin_write( stream->outStream,
                                       &l_vBuffer,
                                       &l_lOutFrameBytes) )
            {

                PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor,
                                                     0,
                                                     l_vBuffer,
                                                     stream->outputChannelCount );
                PaUtil_SetOutputFrameCount( &stream->bufferProcessor,
                                            l_lFramesPerHostBuffer );

                l_lBytesLeft -= l_lOutFrameBytes;

                /* As patest_mono works as expected
                 * @TODO add it if needed
                 * If mono we assume to have stereo output
                 * So we just copy to other channel..
                 * Little bit hackish but works.. with float currently
                 * if( l_ptrStream->outputChannelCount == 1 )
                 * {
                 *     void *l_ptrStartOrig = l_cBUffer + l_lOutFrameBytes;
                 *     void *l_ptrStartStereo = l_cBUffer;
                 *     memcpy(l_ptrStartOrig, l_ptrStartStereo, l_lOutFrameBytes);
                 *
                 *     for(i = 0; i < l_lOutFrameBytes; i += stream->outputFrameSize)
                 *     {
                 *         memcpy( l_ptrStartStereo,
                 *                 l_ptrStartOrig,
                 *                 l_ptrStream->outputFrameSize );
                 *         l_ptrStartStereo += stream->outputFrameSize;
                 *         memcpy( l_ptrStartStereo,
                 *                 l_ptrStartOrig,
                 *                 l_ptrStream->outputFrameSize );
                 *         l_ptrStartStereo += stream->outputFrameSize;
                 *         l_ptrStartOrig += stream->outputFrameSize;
                 *     }
                 *
                 *     memcpy(l_ptrStartStereo, l_ptrStartOrig, l_lOutFrameBytes);
                 * }
                 */

                if( pa_stream_write( stream->outStream,
                                     l_vBuffer,
                                     l_lOutFrameBytes,
                                     NULL,
                                     0,
                                     PA_SEEK_RELATIVE) )
                {
                    PA_DEBUG( ("Portaudio %s: Can't write audio!\n",
                              __FUNCTION__) );
                }

                l_vBuffer = NULL;
            }
            else
            {
                PA_DEBUG( ("Portaudio %s: Can't alloc writing buffer!\n",
                          __FUNCTION__) );
            }

            PaPulseAudio_UnLock( stream->mainloop );
        }

        numFrames =
            PaUtil_EndBufferProcessing( &stream->bufferProcessor,
                                        &l_iResult );

        PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer,
                                      numFrames );

        if( l_iResult != paContinue )
        {
            /* Eventually notify user all buffers have played */
            if( stream->streamRepresentation.streamFinishedCallback
                && stream->isActive )
            {
                stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
            }

            stream->isActive = 0;
            break;
        }

    } while ( l_lBytesLeft > 0 );

    l_vBuffer = NULL;

    return paNoError;
}

void PaPulseAudio_StreamRecordCb( pa_stream * s,
                                  size_t length,
                                  void *userdata )
{
    PaPulseAudio_Stream *l_ptrStream = (PaPulseAudio_Stream *) userdata;
    const void *l_ptrSampleData = NULL;

    PaPulseAudio_Lock( l_ptrStream->mainloop );

    if( pa_stream_peek( l_ptrStream->inStream,
                        &l_ptrSampleData,
                        &length ))
    {
        PA_DEBUG( ("Portaudio %s: Can't read audio!\n",
                  __FUNCTION__) );
    }
    else
    {
        /*
         * If there is not enough room. Read from ringbuffer to make
         * sure that if not full and audio will just underrun
         *
         * If you try to read too much and there is no room then this
         * will fail. But I don't know how to get into that?
         */
        if( PaUtil_GetRingBufferWriteAvailable(&l_ptrStream->inputRing) < length )
        {
            uint8_t l_cBUffer[PULSEAUDIO_BUFFER_SIZE];
            PaUtil_ReadRingBuffer(&l_ptrStream->inputRing,
                                  l_cBUffer,
                                  length);
        }

        PaUtil_WriteRingBuffer( &l_ptrStream->inputRing,
                                l_ptrSampleData,
                                length);
    }

    pa_stream_drop( l_ptrStream->inStream );

    l_ptrSampleData = NULL;

    PaPulseAudio_UnLock( l_ptrStream->mainloop );

    /* Let's handle when output happens if Duplex
     *
     * Also there is no callback there is no meaning to continue
     * as we have blocking reading
     */
    if( !l_ptrStream->outStream && l_ptrStream->bufferProcessor.streamCallback )
    {
        _PaPulseAudio_processAudioInputOutput( l_ptrStream, 0, length );
    }


    pa_threaded_mainloop_signal( l_ptrStream->mainloop,
                                 0 );
}

void PaPulseAudio_StreamPlaybackCb( pa_stream * s,
                                    size_t length,
                                    void *userdata )
{
    PaPulseAudio_Stream *l_ptrStream = (PaPulseAudio_Stream *) userdata;
    _PaPulseAudio_processAudioInputOutput( l_ptrStream, length, 0 );
    pa_threaded_mainloop_signal( l_ptrStream->mainloop,
                                 0 );
}

/* This is left for future use! */
static void PaPulseAudio_StreamSuccessCb( pa_stream * s,
                                          int success,
                                          void *userdata )
{
    PaPulseAudio_Stream *l_ptrStream = (PaPulseAudio_Stream *) userdata;
    PA_DEBUG( ("Portaudio %s: %d\n", __FUNCTION__,
              success) );
    pa_threaded_mainloop_signal( l_ptrStream->mainloop,
                                 0 );
}

/* This is left for future use! */
static void PaPulseAudio_CorkSuccessCb(
    pa_stream * s,
    int success,
    void *userdata
)
{
    PaPulseAudio_Stream *l_ptrStream = (PaPulseAudio_Stream *) userdata;
    pa_threaded_mainloop_signal( l_ptrStream->mainloop,
                                 0 );
}


/* This is left for future use! */
void PaPulseAudio_StreamStartedCb( pa_stream * stream,
                                   void *userdata )
{
    PaPulseAudio_Stream *l_ptrStream = (PaPulseAudio_Stream *) userdata;
    pa_threaded_mainloop_signal( l_ptrStream->mainloop,
                                 0 );
}


/*
    When CloseStream() is called, the multi-api layer ensures that
    the stream has already been stopped or aborted.
*/
PaError PaPulseAudio_CloseStreamCb( PaStream * s )
{
    PaError result = paNoError;
    PaPulseAudio_Stream *stream = (PaPulseAudio_Stream *) s;
    PaPulseAudio_HostApiRepresentation *l_ptrPulseAudioHostApi = stream->hostapi;
    pa_operation *l_ptrOperation = NULL;
    int l_iLoop = 0;
    int l_iError = 0;

    /* Wait for stream to be stopped */
    stream->isActive = 0;
    stream->isStopped = 1;

    if( stream->outStream != NULL
        && pa_stream_get_state( stream->outStream ) == PA_STREAM_READY )
    {
        PaPulseAudio_Lock(stream->mainloop);

        /* Then we cancel all writing (if there is any)
         *  and wait for disconnetion (TERMINATION) which
         * can take a while for ethernet connections
         */
        pa_stream_cancel_write(stream->outStream);
        pa_stream_disconnect(stream->outStream);
        PaPulseAudio_UnLock(stream->mainloop);
    }

    if( stream->inStream != NULL
        && pa_stream_get_state( stream->inStream ) == PA_STREAM_READY )
    {
        PaPulseAudio_Lock( stream->mainloop );

        /* Then we disconnect stream and wait for
         * Termination
         */
        pa_stream_disconnect( stream->inStream );

        PaPulseAudio_UnLock( stream->mainloop );

    }

    /* Wait for termination for both */
    while(!l_iLoop)
    {
        PaPulseAudio_Lock( stream->mainloop );
        if( stream->inStream != NULL
            && pa_stream_get_state( stream->inStream ) == PA_STREAM_TERMINATED )
        {
            pa_stream_unref( stream->inStream );
            stream->inStream = NULL;
        }
        PaPulseAudio_UnLock( stream->mainloop );

        PaPulseAudio_Lock( stream->mainloop );
        if( stream->outStream != NULL
            && pa_stream_get_state(stream->outStream) == PA_STREAM_TERMINATED )
        {
            pa_stream_unref( stream->outStream );
            stream->outStream = NULL;
        }
        PaPulseAudio_UnLock( stream->mainloop );

        if((stream->outStream == NULL
           && stream->inStream == NULL)
           || l_iError >= 5000 )
        {
              l_iLoop = 1;
        }

        l_iError ++;
        usleep(500);
    }

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );

    PaUtil_FreeMemory( stream->sourceStreamName );
    PaUtil_FreeMemory( stream->sinkStreamName );
    PaUtil_FreeMemory(stream);

    return result;
}


PaError PaPulseAudio_StartStreamCb( PaStream * s )
{
    PaError result = paNoError;
    PaPulseAudio_Stream *stream = (PaPulseAudio_Stream *) s;
    int l_iPlaybackStreamStarted = 0;
    int l_iRecordStreamStarted = 0;
    pa_stream_state_t l_SState = PA_STREAM_UNCONNECTED;
    PaPulseAudio_HostApiRepresentation *l_ptrPulseAudioHostApi = stream->hostapi;
    const char *l_strName = NULL;
    pa_operation *l_ptrOperation = NULL;
    int l_iLoop = 0;

    /* Ready the processor */
    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );

    PaPulseAudio_Lock( l_ptrPulseAudioHostApi->mainloop );
    /* Adjust latencies if that is wanted
     * https://www.freedesktop.org/wiki/Software/PulseAudio/Documentation/Developer/Clients/LatencyControl/
     *
     * tlength is for Playback
     * fragsize if for Record
     */
    stream->bufferAttr.maxlength = (uint32_t)-1;
    /* @TODO There is mixed documentation on this!
     *
     * API documentation tlength and fragsize
     * should be '(uint32_t)-1' also but as '0' works
     * but it breaks something this should be
     * done as in documentation:
     * https://freedesktop.org/software/pulseaudio/doxygen/structpa__buffer__attr.html
     */
    stream->bufferAttr.tlength = 0;
    stream->bufferAttr.fragsize = 0;
    stream->bufferAttr.prebuf = (uint32_t)-1;
    stream->bufferAttr.minreq = (uint32_t)-1;
    stream->outputUnderflows = 0;
    PaPulseAudio_UnLock( l_ptrPulseAudioHostApi->mainloop );

    if( stream->outStream != NULL )
    {
        /* Only change tlength if latency if more than Zero */
        if( stream->latency > 0 )
        {
            stream->bufferAttr.tlength = pa_usec_to_bytes( (pa_usec_t)(stream->latency * PA_USEC_PER_SEC), &stream->outSampleSpec );
        }

        pa_stream_set_write_callback( stream->outStream,
                                      PaPulseAudio_StreamPlaybackCb,
                                      stream );

        /* Just keep on trucking if we are just corked*/
        if( pa_stream_get_state( stream->outStream ) == PA_STREAM_READY
            && pa_stream_is_corked( stream->outStream ) )
        {
            PaPulseAudio_Lock( l_ptrPulseAudioHostApi->mainloop );
            l_ptrOperation = pa_stream_cork( stream->outStream,
                                            0,
                                            PaPulseAudio_CorkSuccessCb,
                                            stream );
            PaPulseAudio_UnLock( l_ptrPulseAudioHostApi->mainloop );

            while( pa_operation_get_state( l_ptrOperation ) == PA_OPERATION_RUNNING)
            {
                pa_threaded_mainloop_wait( l_ptrPulseAudioHostApi->mainloop );
            }

            pa_operation_unref( l_ptrOperation );
            l_ptrOperation = NULL;
        }
        else
        {
            if( stream->outDevice != paNoDevice )
            {
                PA_DEBUG( ("Portaudio %s: %d (%s)\n",
                          __FUNCTION__,
                          stream->outDevice,
                          l_ptrPulseAudioHostApi->pulseaudioDeviceNames[stream->
                                                                        outDevice]) );
            }

            PaDeviceIndex defaultOutputDevice;
            PaError result = PaUtil_DeviceIndexToHostApiDeviceIndex(&defaultOutputDevice,
                             l_ptrPulseAudioHostApi->inheritedHostApiRep.info.defaultOutputDevice,
                             &(l_ptrPulseAudioHostApi->inheritedHostApiRep) );

            /* NULL means default device */
            l_strName = NULL;

            /* If default device is not requested then change to wanted device */
            if( result == paNoError && stream->outDevice != defaultOutputDevice )
            {
                l_strName = l_ptrPulseAudioHostApi->
                            pulseaudioDeviceNames[stream->outDevice];
            }

            if(result == paNoError)
            {
                PaPulseAudio_Lock( l_ptrPulseAudioHostApi->mainloop );

                if ( ! pa_stream_connect_playback( stream->outStream,
                                                   l_strName,
                                                   &stream->bufferAttr,
                                                   PA_STREAM_INTERPOLATE_TIMING |
                                                   PA_STREAM_ADJUST_LATENCY |
                                                   PA_STREAM_AUTO_TIMING_UPDATE |
                                                   PA_STREAM_NO_REMIX_CHANNELS |
                                                   PA_STREAM_NO_REMAP_CHANNELS |
                                                   PA_STREAM_DONT_MOVE,
                                                   NULL,
                                                   NULL ) )
                {
                    pa_stream_set_underflow_callback( stream->outStream,
                                                      PaPulseAudio_StreamUnderflowCb,
                                                      stream);
                    pa_stream_set_started_callback( stream->outStream,
                                                    PaPulseAudio_StreamStartedCb,
                                                    stream );
                }
                else
                {
                    PA_DEBUG( ("Portaudio %s: Can't write audio!\n",
                              __FUNCTION__) );
                    goto startstreamcb_error;
                }
                PaPulseAudio_UnLock( l_ptrPulseAudioHostApi->mainloop );
            }
            else
            {
                goto startstreamcb_error;
            }

        }
    }

    if( stream->inStream != NULL )
    {
        /* Only change fragsize if latency if more than Zero */
        if ( stream->latency > 0 )
        {
            stream->bufferAttr.fragsize = pa_usec_to_bytes( (pa_usec_t)(stream->latency * PA_USEC_PER_SEC), &stream->inSampleSpec );
        }

        if( stream->inDevice != paNoDevice)
        {
            PA_DEBUG(("Portaudio %s: %d (%s)\n", __FUNCTION__, stream->inDevice,
                      l_ptrPulseAudioHostApi->pulseaudioDeviceNames[stream->
                                                                    inDevice]));
        }

        pa_stream_set_read_callback( stream->inStream,
                                     PaPulseAudio_StreamRecordCb,
                                     stream );

        PaDeviceIndex defaultInputDevice;
        PaError result = PaUtil_DeviceIndexToHostApiDeviceIndex(
                &defaultInputDevice,
                l_ptrPulseAudioHostApi->inheritedHostApiRep.info.defaultInputDevice,
                &(l_ptrPulseAudioHostApi->inheritedHostApiRep) );

        /* NULL means default device */
        l_strName = NULL;

        /* If default device is not requested then change to wanted device */
        if( result == paNoError && stream->inDevice != defaultInputDevice )
        {
            l_strName = l_ptrPulseAudioHostApi->
                        pulseaudioDeviceNames[stream->inDevice];
        }

        if ( result == paNoError )
        {
            PaPulseAudio_Lock( l_ptrPulseAudioHostApi->mainloop );
            /* Zero means success */
            if( ! pa_stream_connect_record( stream->inStream,
                                      l_strName,
                                      &stream->bufferAttr,
                                      PA_STREAM_INTERPOLATE_TIMING |
                                      PA_STREAM_ADJUST_LATENCY |
                                      PA_STREAM_AUTO_TIMING_UPDATE |
                                      PA_STREAM_NO_REMIX_CHANNELS |
                                      PA_STREAM_NO_REMAP_CHANNELS |
                                      PA_STREAM_DONT_MOVE) )
            {
                pa_stream_set_underflow_callback( stream->inStream,
                                                  PaPulseAudio_StreamUnderflowCb,
                                                  stream);

                pa_stream_set_started_callback( stream->inStream,
                                                PaPulseAudio_StreamStartedCb,
                                                stream );
            }
            else
            {
                PA_DEBUG( ("Portaudio %s: Can't read audio!\n",
                          __FUNCTION__) );

                goto startstreamcb_error;
            }
            PaPulseAudio_UnLock( l_ptrPulseAudioHostApi->mainloop );
        }
        else
        {
            goto startstreamcb_error;
        }

    }

    if( stream->outStream ||
        stream->inStream )
    {
        stream->isActive = 0;
        stream->isStopped = 1;

        while( 1 )
        {
            if( stream->outStream != NULL )
            {
                PaPulseAudio_Lock( l_ptrPulseAudioHostApi->mainloop );
                l_SState = pa_stream_get_state( stream->outStream );
                PaPulseAudio_UnLock( l_ptrPulseAudioHostApi->mainloop );

                if( PA_STREAM_READY == l_SState &&
                    !l_iPlaybackStreamStarted )
                {
                    l_iPlaybackStreamStarted = 1;
                }
                else if( PA_STREAM_FAILED == l_SState ||
                         PA_STREAM_TERMINATED == l_SState )
                {
                    goto startstreamcb_error;
                }
            }

            if( stream->inStream != NULL )
            {
                PaPulseAudio_Lock( l_ptrPulseAudioHostApi->mainloop );
                l_SState = pa_stream_get_state( stream->inStream );
                PaPulseAudio_UnLock( l_ptrPulseAudioHostApi->mainloop );

                if( PA_STREAM_READY == l_SState &&
                    !l_iRecordStreamStarted )
                {
                    l_iRecordStreamStarted = 1;
                }
                else if( PA_STREAM_FAILED == l_SState ||
                         PA_STREAM_TERMINATED == l_SState )
                {
                    goto startstreamcb_error;
                }

            }

            if( !stream->inStream && !stream->outStream)
            {
                goto startstreamcb_error;
            }

            if( (l_iPlaybackStreamStarted || stream->outStream == NULL) && 
                (l_iRecordStreamStarted || stream->inStream == NULL) )
            {
                stream->isActive = 1;
                stream->isStopped = 0;
                break;
            }

            l_iLoop ++;

            if( l_iLoop >= 100)
            {
                PA_DEBUG( ("Portaudio %s: Can't connect streams!\n",
                          __FUNCTION__) );
                goto startstreamcb_error;
            }

            usleep(1000);
        }
    }
    else
    {
        PA_DEBUG( ("Portaudio %s: Streams not initialized!\n",
                  __FUNCTION__) );
        goto startstreamcb_error;
    }

    /* Make sure we pass no error on intialize */
    result = paNoError;

    /* Allways unlock.. so we don't get locked */
    startstreamcb_end:
    return result;

    error:
    startstreamcb_error:
    PA_DEBUG( ("Portaudio %s: Can't start audio!\n",
              __FUNCTION__) );

    if( l_iPlaybackStreamStarted || l_iRecordStreamStarted )
    {
        PaPulseAudio_AbortStreamCb( stream );
    }

    stream->isActive = 0;
    stream->isStopped = 1;
    result = paNotInitialized;

    goto startstreamcb_end;
}

PaError RequestStop( PaPulseAudio_Stream * stream,
                     int abort )
{
    PaError result = paNoError;
    PaPulseAudio_HostApiRepresentation *l_ptrPulseAudioHostApi = stream->hostapi;
    pa_operation *l_ptrOperation = NULL;

    PaPulseAudio_Lock( l_ptrPulseAudioHostApi->mainloop );

    /* Wait for stream to be stopped */
    stream->isActive = 0;
    stream->isStopped = 1;

    /* Test if there is something that we can play */
    if( stream->outStream != NULL
        && pa_stream_get_state( stream->outStream ) == PA_STREAM_READY
        && !pa_stream_is_corked( stream->outStream )
        && !abort )
    {
       l_ptrOperation = pa_stream_cork( stream->outStream,
                                        1,
                                        PaPulseAudio_CorkSuccessCb,
                                        stream );

        while( pa_operation_get_state( l_ptrOperation ) == PA_OPERATION_RUNNING )
        {
            pa_threaded_mainloop_wait( l_ptrPulseAudioHostApi->mainloop );
        }

        pa_operation_unref( l_ptrOperation );

        l_ptrOperation = NULL;
    }

  requeststop_error:
    PaPulseAudio_UnLock( l_ptrPulseAudioHostApi->mainloop );
    stream->isActive = 0;
    stream->isStopped = 1;

    return result;
}

PaError PaPulseAudio_StopStreamCb( PaStream * s )
{
    return RequestStop( (PaPulseAudio_Stream *) s,
                        0 );
}


PaError PaPulseAudio_AbortStreamCb( PaStream * s )
{
    return RequestStop( (PaPulseAudio_Stream *) s,
                        1 );
}
