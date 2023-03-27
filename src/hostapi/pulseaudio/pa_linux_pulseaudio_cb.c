
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

int PaPulseAudio_updateTimeInfo( pa_stream * s,
                                 PaStreamCallbackTimeInfo *timeInfo,
                                 int record )
{
    unsigned int l_iNegative = 0;
    pa_usec_t l_lStreamTime = 0;
    pa_usec_t l_lStreamLatency = 0;
    int l_iRtn = 0;

    if( pa_stream_get_time( s,
                          &l_lStreamTime ) == -PA_ERR_NODATA )
    {
        return -PA_ERR_NODATA;
    }
    else
    {
    timeInfo->currentTime = ((PaTime) l_lStreamTime / (PaTime) 1000000);
    }

    if( pa_stream_get_latency( s,
                             &l_lStreamLatency,
                             &l_iNegative ) == -PA_ERR_NODATA )
    {
        return -PA_ERR_NODATA;
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
    return 0;
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

static int _PaPulseAudio_Write( PaPulseAudio_Stream *stream,
                                size_t length )
{
    void *l_ptrData = NULL;
    size_t l_lBytes = length;
    size_t l_lReadableBytes = PaUtil_GetRingBufferReadAvailable( &stream->outputRing );

    if( l_lReadableBytes < l_lBytes )
    {
        PA_DEBUG( ("Portaudio %s: Too less buffered: %ld which is less than %ld\n",
                __FUNCTION__, l_lReadableBytes, l_lBytes) );

        if( !l_lReadableBytes )
        {
            return paOutputUnderflowed;
        }

        l_lBytes = l_lReadableBytes;
    }


    /* Allocate memory to make it faster to output stuff */
    pa_stream_begin_write( stream->outStream, &l_ptrData, &l_lBytes );

    /* If we are not in active state (mainly duplex)
     * we write just zero (nothing) to output
     * until we are
     */
    if( stream->isActive )
    {

        PaUtil_ReadRingBuffer( &stream->outputRing,
                               l_ptrData,
                               l_lBytes     );
    }
    else
    {
        memset( l_ptrData, 0x00, l_lBytes);
    }

    if( pa_stream_write( stream->outStream,
                         l_ptrData,
                         l_lBytes,
                         NULL,
                         0,
                         PA_SEEK_RELATIVE) )
    {
        PA_DEBUG( ("Portaudio %s: Can't write audio!\n",
                  __FUNCTION__) );
    }

    return paNoError;
}

static size_t _PaPulseAudio_CheckWrite( PaPulseAudio_Stream *stream,
                                        size_t length)
{
    size_t l_lOutReadableBytes = PaUtil_GetRingBufferReadAvailable(&stream->outputRing);
    size_t l_lLeft = length;

    if( (length != (size_t) -1 && length > 0) && l_lOutReadableBytes > 0)
    {
        if( l_lLeft <= l_lOutReadableBytes )
        {
            _PaPulseAudio_Write( stream, l_lLeft );
            l_lLeft = 0;
        }
        else
        {
            _PaPulseAudio_Write( stream, l_lOutReadableBytes );
            l_lLeft -= l_lOutReadableBytes;
        }
    }
    else
    {
        return (size_t) -1;
    }

    return l_lLeft;
}

void _PaPulseAudio_WriteRingBuffer( PaUtilRingBuffer *ringbuffer,
                                    const void *buffer,
                                    size_t length )
{
    /*
     * If there is not enough room. Read from ringbuffer to make
     * sure that if not full and audio will just underrun
     *
     * If you try to read too much and there is no room then this
     * will fail. But I don't know how to get into that?
     */
    if( PaUtil_GetRingBufferWriteAvailable(ringbuffer) < length )
    {
        uint8_t l_cBUffer[ PULSEAUDIO_BUFFER_SIZE ];
        PaUtil_ReadRingBuffer( ringbuffer,
                               l_cBUffer,
                               length );
    }

    PaUtil_WriteRingBuffer( ringbuffer,
                            buffer,
                            length );

}

void _PaPulseAudio_Read( PaPulseAudio_Stream *stream,
                         size_t length )
{
    const void *l_ptrSampleData = NULL;

    if( pa_stream_peek( stream->inStream,
                        &l_ptrSampleData,
                        &length ))
    {
        PA_DEBUG( ("Portaudio %s: Can't read audio!\n",
                  __FUNCTION__) );
    }
    else
    {
        _PaPulseAudio_WriteRingBuffer( &stream->inputRing, l_ptrSampleData, length );
    }

    pa_stream_drop( stream->inStream );

    l_ptrSampleData = NULL;

}

static int _PaPulseAudio_ProcessAudio(PaPulseAudio_Stream *stream)
{
    uint8_t l_cBUffer[PULSEAUDIO_BUFFER_SIZE];
    size_t l_lFramesPerHostBuffer = stream->bufferProcessor.framesPerHostBuffer;
    size_t l_lOutFrameBytes = 0;
    size_t l_lInFrameBytes = 0;
    size_t l_lNumFrames = 0;
    int l_bOutputCb = 0;
    int l_bInputCb = 0;
    PaStreamCallbackTimeInfo timeInfo;
    int l_iResult = paContinue;
    size_t l_lWritableBytesSize = (size_t) -1;
    size_t l_lReadableBytesSize = (size_t) -1;
    size_t l_lInReadableBytes = PaUtil_GetRingBufferReadAvailable(&stream->inputRing);

    /* If we just want to have input but not output (Not Duplex)
     * Use this calculation
     */
    if( stream->inStream )
    {
        if( l_lFramesPerHostBuffer == paFramesPerBufferUnspecified )
        {
            l_lFramesPerHostBuffer = (512 / (stream->inputFrameSize * 2));

            if( (l_lFramesPerHostBuffer % 2) )
            {
                l_lFramesPerHostBuffer ++;
            }
        }

        l_lInFrameBytes = (l_lFramesPerHostBuffer * stream->inputFrameSize);

        if( stream->bufferProcessor.streamCallback )
        {
            l_bInputCb = 1;
        }
    }

    if( stream->outStream )
    {
        if( l_lFramesPerHostBuffer == paFramesPerBufferUnspecified )
        {
            if( !stream->framesPerHostCallback )
            {
                l_lFramesPerHostBuffer = (512 / (stream->outputFrameSize * 2));

                if( (l_lFramesPerHostBuffer % 2) )
                {
                    l_lFramesPerHostBuffer ++;
                }

                stream->framesPerHostCallback = l_lFramesPerHostBuffer;
            }
            else
            {
                l_lFramesPerHostBuffer = stream->framesPerHostCallback;
            }
        }

        l_lInFrameBytes = l_lOutFrameBytes = (l_lFramesPerHostBuffer * stream->outputFrameSize);

        if( stream->bufferProcessor.streamCallback )
        {
            l_bOutputCb = 1;
        }
    }

    if( !stream->isActive &&
        (stream->inStream && stream->outStream) )
    {
        return paContinue;
    }
    else if( !stream->isActive &&
             stream->outStream )
    {
        stream->isActive = 1;
        stream->isStopped = 0;
    }

    if( stream->inStream )
    {
        l_lReadableBytesSize = pa_stream_readable_size(stream->inStream);

        if( l_lReadableBytesSize != (size_t) -1 && l_lReadableBytesSize > 0 )
        {
            _PaPulseAudio_Read( stream, l_lReadableBytesSize );
        }
    }

    if( stream->outStream )
    {
        size_t l_lTmpWritableBytesSize = pa_stream_writable_size( stream->outStream );
        if( l_lTmpWritableBytesSize != (size_t) -1 && l_lTmpWritableBytesSize > 0)
        {
            /*if( stream->missedBytes )
            {
                l_lWritableBytesSize = _PaPulseAudio_CheckWrite( stream, stream->missedBytes );
                if( l_lWritableBytesSize != (size_t) -1)
                {
                    stream->missedBytes = l_lWritableBytesSize;
                }
            }*/

            l_lWritableBytesSize = _PaPulseAudio_CheckWrite( stream, (l_lTmpWritableBytesSize + stream->missedBytes));

            if( l_lWritableBytesSize != (size_t) -1 && l_lWritableBytesSize > 0 )
            {
                stream->missedBytes += l_lWritableBytesSize;
            }
        }
    }

    /* There is only Record stream so
     * see if we have enough stuff to feed record stream
     * If not then bail out.
     */
    if( (l_bInputCb && !l_bOutputCb) &&
        PaUtil_GetRingBufferReadAvailable(&stream->inputRing) < l_lInFrameBytes )
    {
        PA_DEBUG( ("Portaudio %s: Input buffer underflowed when Duplex play!\n",
        __FUNCTION__) );
        return paBufferTooSmall;
    }

    if( l_lInReadableBytes < l_lInFrameBytes )
    {
        //PA_DEBUG( ("Portaudio %s: Input buffer underflowed when Duplex play %ld/%ld!\n",
        //__FUNCTION__, PaUtil_GetRingBufferReadAvailable(&stream->inputRing), l_lInFrameBytes) );

        return paBufferTooSmall;
    }

    if(  stream->outStream )
    {
        if( PaPulseAudio_updateTimeInfo( stream->outStream,
                                         &timeInfo,
                                         0 ) == -PA_ERR_NODATA )
        {
            PA_DEBUG( ("Portaudio %s: No output stream timing info available!\n",
                    __FUNCTION__) );
            return paNotInitialized;
        }
    }
    if(  stream->inStream )
    {
        if( PaPulseAudio_updateTimeInfo( stream->inStream,
                                     &timeInfo,
                                     1 ) == -PA_ERR_NODATA )
        {
            PA_DEBUG( ("Portaudio %s: No input stream timing info available!\n",
                    __FUNCTION__) );
            return paNotInitialized;
        }
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

    }

    if( l_bOutputCb )
    {

        PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor,
                                             0,
                                             l_cBUffer,
                                             stream->outputChannelCount );
        PaUtil_SetOutputFrameCount( &stream->bufferProcessor,
                                    l_lFramesPerHostBuffer );

        /* Ringbuffer them for playing few moments after this */
        PaUtil_WriteRingBuffer( &stream->outputRing,
                                l_cBUffer,
                                l_lOutFrameBytes );

        if( stream->missedBytes )
        {
            /*
             * We already missed stuff
             * So write more than needed that we are now underflow output
             * This can be done now that buffer is full of stuff
             *
             * Using 1/4 sound nice and works at testing. There is
             * no magic behind this.
             */
            size_t l_lOutReadableBytes = PaUtil_GetRingBufferReadAvailable(&stream->outputRing);
            size_t l_lLimit = (l_lOutReadableBytes / 4);
            if( (stream->missedBytes + l_lLimit) < l_lOutReadableBytes)
            {
                stream->missedBytes = (stream->missedBytes + l_lLimit);
            }
            stream->missedBytes = _PaPulseAudio_CheckWrite( stream, stream->missedBytes );
        }


    }

    l_lNumFrames =
        PaUtil_EndBufferProcessing( &stream->bufferProcessor,
                                    &l_iResult );

    PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer,
                                  l_lNumFrames );

    return l_iResult;
}

void _PaPulseAudio_TimeEventCb( pa_mainloop_api *mainloopApi,
                                pa_time_event *event,
                                const struct timeval *tv,
                                void *userdata)
{
    PaPulseAudio_Stream *l_ptrStream = (PaPulseAudio_Stream *) userdata;
    int l_iRtn = paContinue;
    struct timeval l_SNow;

    l_iRtn = _PaPulseAudio_ProcessAudio( l_ptrStream );

    if( l_iRtn == paComplete || l_iRtn == paAbort )
    {
        /* Eventually notify user all buffers have played */
        if( l_ptrStream->streamRepresentation.streamFinishedCallback
            && l_ptrStream->isActive )
        {
            l_ptrStream->streamRepresentation.streamFinishedCallback( l_ptrStream->streamRepresentation.userData );
        }

        l_ptrStream->isActive = 0;
    }
    else
    {
        gettimeofday(&l_SNow, NULL);
        pa_timeval_add(&l_SNow, PULSEAUDIO_TIME_EVENT_USEC);
        mainloopApi->time_restart(event, &l_SNow);
    }
    pa_threaded_mainloop_signal( l_ptrStream->mainloop,
                                 0 );
}

void PaPulseAudio_StreamRecordCb( pa_stream * s,
                                  size_t length,
                                  void *userdata )
{
    PaPulseAudio_Stream *l_ptrStream = (PaPulseAudio_Stream *) userdata;

    if( !l_ptrStream->isActive )
    {
        l_ptrStream->isActive = 1;
        l_ptrStream->isStopped = 0;
    }

    /* Let's handle when output happens if Duplex
     *
     * Also there is no callback there is no meaning to continue
     * as we have blocking reading
     */
    //if( !l_ptrStream->outStream && l_ptrStream->bufferProcessor.streamCallback )
    //{
    //    _PaPulseAudio_processAudioInputOutput( l_ptrStream, 0, length );
    //}


    pa_threaded_mainloop_signal( l_ptrStream->mainloop,
                                 0 );
}

void PaPulseAudio_StreamPlaybackCb( pa_stream * s,
                                    size_t length,
                                    void *userdata )
{
    PaPulseAudio_Stream *l_ptrStream = (PaPulseAudio_Stream *) userdata;

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
        usleep(10000);
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
    struct timeval l_SNow;

    stream->isActive = 0;
    stream->isStopped = 1;
    stream->missedBytes = 0;

    /* Ready the processor */
    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );

    PaPulseAudio_Lock( l_ptrPulseAudioHostApi->mainloop );
    /* Adjust latencies if that is wanted
     * https://www.freedesktop.org/wiki/Software/PulseAudio/Documentation/Developer/Clients/LatencyControl/
     *
     * tlength is for Playback
     * fragsize if for Record
     */
    stream->outBufferAttr.maxlength = (uint32_t)-1;
    stream->inBufferAttr.maxlength = (uint32_t)-1;
    stream->outBufferAttr.tlength = (uint32_t)-1;
    stream->inBufferAttr.tlength = (uint32_t)-1;
    stream->outBufferAttr.fragsize = (uint32_t)-1;
    stream->inBufferAttr.fragsize = (uint32_t)-1;
    stream->outBufferAttr.prebuf = (uint32_t)-1;
    stream->inBufferAttr.prebuf = (uint32_t)-1;
    stream->outBufferAttr.minreq = (uint32_t)-1;
    stream->inBufferAttr.minreq = (uint32_t)-1;

    stream->outputUnderflows = 0;
    PaPulseAudio_UnLock( l_ptrPulseAudioHostApi->mainloop );

    if( stream->inStream )
    {
        stream->outBufferAttr.fragsize = 0;

        /* Only change fragsize if latency if more than Zero */
        if ( stream->framesPerHostCallback )
        {
            stream->inBufferAttr.fragsize = (stream->framesPerHostCallback * stream->inputFrameSize);
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
                                      &stream->inBufferAttr,
                                      PA_STREAM_INTERPOLATE_TIMING |
                                      PA_STREAM_ADJUST_LATENCY |
                                      PA_STREAM_AUTO_TIMING_UPDATE |
                                      PA_STREAM_NO_REMIX_CHANNELS |
                                      PA_STREAM_NO_REMAP_CHANNELS |
                                      PA_STREAM_DONT_MOVE) )
            {
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

            for( l_iLoop = 0; l_iLoop < 100; l_iLoop ++ )
            {
                PaPulseAudio_Lock( l_ptrPulseAudioHostApi->mainloop );
                l_SState = pa_stream_get_state( stream->inStream );
                PaPulseAudio_UnLock( l_ptrPulseAudioHostApi->mainloop );

                if( l_SState == PA_STREAM_READY )
                {
                    break;
                }
                else if( l_SState == PA_STREAM_FAILED ||
                         l_SState == PA_STREAM_TERMINATED )
                {
                    goto startstreamcb_error;
                }

                usleep(10000);
            }
        }
        else
        {
            goto startstreamcb_error;
        }

    }

    if( stream->outStream )
    {
        stream->outBufferAttr.tlength = 0;

        /* Only change tlength if latency if more than Zero */
        if( stream->framesPerHostCallback )
        {
            stream->outBufferAttr.tlength = (stream->framesPerHostCallback * stream->outputFrameSize);
        }

        //pa_stream_set_write_callback( stream->outStream,
        //                              PaPulseAudio_StreamPlaybackCb,
        //                              stream );

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
                                                   &stream->outBufferAttr,
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

                for( l_iLoop = 0; l_iLoop < 100; l_iLoop ++ )
                {
                    PaPulseAudio_Lock( l_ptrPulseAudioHostApi->mainloop );
                    l_SState = pa_stream_get_state( stream->outStream );
                    PaPulseAudio_UnLock( l_ptrPulseAudioHostApi->mainloop );

                    if( l_SState = PA_STREAM_READY )
                    {
                        break;
                    }
                    else if( l_SState == PA_STREAM_FAILED  ||
                             l_SState == PA_STREAM_TERMINATED )
                    {
                        goto startstreamcb_error;
                    }

                    usleep(10000);
                }

            }
            else
            {
                goto startstreamcb_error;
            }
        }
    }

    if( !stream->outStream &&
        !stream->inStream )
    {
        PA_DEBUG( ("Portaudio %s: Streams not initialized!\n",
                  __FUNCTION__) );
        goto startstreamcb_error;
    }

    if( stream->hostapi->mainloopApi )
    {
        gettimeofday(&l_SNow, NULL);
        pa_timeval_add(&l_SNow, PULSEAUDIO_TIME_EVENT_USEC * 5000);

        if( !(stream->hostapi->mainloopApi->time_new(stream->hostapi->mainloopApi, &l_SNow, _PaPulseAudio_TimeEventCb, stream) ) )
        {
            PA_DEBUG( ("Portaudio %s: Can't initialize time event!\n",
                      __FUNCTION__) );
            goto startstreamcb_error;
        }
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

static PaError RequestStop( PaPulseAudio_Stream * stream,
                     int abort )
{
    PaError result = paNoError;
    PaPulseAudio_HostApiRepresentation *l_ptrPulseAudioHostApi = stream->hostapi;
    pa_operation *l_ptrOperation = NULL;

    PaPulseAudio_Lock( l_ptrPulseAudioHostApi->mainloop );

    /* Wait for stream to be stopped */
    stream->isActive = 0;
    stream->isStopped = 1;
    stream->missedBytes = 0;

    /* If we are in Callback mode then
     * make sure that time event is turned off
     */
    if( stream->hostapi->mainloopApi && stream->hostapi->timeEvent )
    {
        stream->hostapi->mainloopApi->time_free( stream->timeEvent );
        stream->timeEvent = NULL;
    }

    /* Test if there is something that we can play */
    if( stream->outStream
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
