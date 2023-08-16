
/*
 * PulseAudio host to play natively in Linux based systems without
 * ALSA emulation
 *
 * Copyright (c) 2014-2023 Tuukka Pasanen <tuukka.pasanen@ilmi.fi>
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

    /*
     * Idea behind this is that we fill ringbuffer with data
     * that comes from input device. When it's available
     * we'll fill it to callback or blocking reading
     */
    if( pa_stream_peek( stream->inputStream,
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

    pa_stream_drop( stream->inputStream );

    l_ptrSampleData = NULL;

}

static int _PaPulseAudio_ProcessAudio(PaPulseAudio_Stream *stream,
                                      size_t length)
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
    void *l_ptrData = NULL;
    size_t l_lWrittenBytes = 0;

    /* If there is no specified per host buffer then
     * just generate one or but correct one in place
     */
    if( l_lFramesPerHostBuffer == paFramesPerBufferUnspecified )
    {
        if( !stream->framesPerHostCallback )
        {
            int l_iCurrentFrameSize = stream->outputFrameSize;

            /* If we have output device then outputFrameSize > 0
             * Otherwise we should use inputFrameSize as we only
             * have input device
             */
            if( l_iCurrentFrameSize <= 0 )
            {
                l_iCurrentFrameSize = stream->inputFrameSize;
            }

            /* If everything else fails just have some sane default
             * This should not ever happen and probably it will fail in
             * somewhere else
             */
            if( l_iCurrentFrameSize  <= 0 )
            {
                return paNotInitialized;
            }

            /* This just good enough and most
             * Pulseaudio server and ALSA can handle it
             */
            l_lFramesPerHostBuffer = (128 / (l_iCurrentFrameSize * 2));

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


    if( stream->outputStream )
    {
        /* Calculate how many bytes goes to one frame */
        l_lInFrameBytes = l_lOutFrameBytes = (l_lFramesPerHostBuffer * stream->outputFrameSize);

        if( stream->bufferProcessor.streamCallback )
        {
            l_bOutputCb = 1;
        }
    }

    /* If we just want to have input but not output (Not Duplex)
     * Use this calculation
     */
    if( stream->inputStream )
    {
        l_lInFrameBytes = l_lOutFrameBytes = (l_lFramesPerHostBuffer * stream->inputFrameSize);

        if( stream->bufferProcessor.streamCallback )
        {
            l_bInputCb = 1;
        }
    }

    /*
     * When stopped we should stop feeding or recording right away
     */
    if( stream->isStopped )
    {
        return paStreamIsStopped;
    }

    /*
     * This can be called before we have reached out
     * starting Portaudio stream or Portaudio stream
     * is stopped
     */
    if( !stream->pulseaudioIsActive )
    {
        if(stream->outputStream)
        {
            l_ptrData = l_cBUffer;
            memset( l_ptrData, 0x00, length);

            pa_stream_write( stream->outputStream,
                             l_ptrData,
                             length,
                             NULL,
                             0,
                             PA_SEEK_RELATIVE );
        }

        return paContinue;
    }

    while(1)
    {
    /* There is only Record stream so
     * see if we have enough stuff to feed record stream
     * If not then bail out.
     */
    if( l_bInputCb &&
        PaUtil_GetRingBufferReadAvailable(&stream->inputRing) < l_lInFrameBytes )
    {
        if(l_bOutputCb && (l_lWrittenBytes < length) && !stream->missedBytes)
        {
            stream->missedBytes = length - l_lWrittenBytes;
        }
        else
        {
            stream->missedBytes = 0;
        }
        break;
    }
    else if( l_lWrittenBytes >= length)
    {
        stream->missedBytes = 0;
        break;
    }

    if(  stream->outputStream )
    {
        PaPulseAudio_updateTimeInfo( stream->outputStream,
                                     &timeInfo,
                                     0 );
    }

    if(  stream->inputStream )
    {
        PaPulseAudio_updateTimeInfo( stream->inputStream,
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
                                            stream->inputSampleSpec.channels );

        PaUtil_SetInputFrameCount( &stream->bufferProcessor,
                                   l_lFramesPerHostBuffer );

    }

    if( l_bOutputCb )
    {

        size_t l_lTmpSize = l_lOutFrameBytes;

        /* Allocate memory to make it faster to output stuff */
        pa_stream_begin_write( stream->outputStream, &l_ptrData, &l_lTmpSize );

        /* If l_ptrData is NULL then output is not ready
         * and we have to wait for it
         */
        if(!l_ptrData)
        {
            return paNotInitialized;
        }

        PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor,
                                             0,
                                             l_ptrData,
                                             stream->outputChannelCount );

        PaUtil_SetOutputFrameCount( &stream->bufferProcessor,
                                    l_lFramesPerHostBuffer );

        if( pa_stream_write( stream->outputStream,
                             l_ptrData,
                             l_lOutFrameBytes,
                             NULL,
                             0,
                             PA_SEEK_RELATIVE ) )
        {
            PA_DEBUG( ("Portaudio %s: Can't write audio!\n",
                      __FUNCTION__) );
        }

        l_lWrittenBytes += l_lOutFrameBytes;
    }

    l_lNumFrames =
        PaUtil_EndBufferProcessing( &stream->bufferProcessor,
                                    &l_iResult );

    PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer,
                                  l_lNumFrames );
    }

    return l_iResult;
}

void PaPulseAudio_StreamRecordCb( pa_stream * s,
                                  size_t length,
                                  void *userdata )
{
    PaPulseAudio_Stream *l_ptrStream = (PaPulseAudio_Stream *) userdata;

    if( !l_ptrStream->pulseaudioIsActive )
    {
        l_ptrStream->pulseaudioIsActive = 1;
        l_ptrStream->pulseaudioIsStopped= 0;
    }

    _PaPulseAudio_Read( l_ptrStream, length );

    /* Let's handle when output happens if Duplex
     *
     * Also there is no callback there is no meaning to continue
     * as we have blocking reading
     */
    if( l_ptrStream->bufferProcessor.streamCallback )
    {
        _PaPulseAudio_ProcessAudio( l_ptrStream, length );
    }

    pa_threaded_mainloop_signal( l_ptrStream->mainloop,
                                 0 );
}

void PaPulseAudio_StreamPlaybackCb( pa_stream * s,
                                    size_t length,
                                    void *userdata )
{
    PaPulseAudio_Stream *l_ptrStream = (PaPulseAudio_Stream *) userdata;
    uint8_t l_cBUffer[PULSEAUDIO_BUFFER_SIZE];

    if( !l_ptrStream->inputStream && !l_ptrStream->pulseaudioIsActive )
    {
        l_ptrStream->pulseaudioIsActive = 1;
        l_ptrStream->pulseaudioIsStopped = 0;
    }

    if( l_ptrStream->bufferProcessor.streamCallback )
    {
        _PaPulseAudio_ProcessAudio( l_ptrStream, length );
    }

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
    stream->pulseaudioIsActive = 0;
    stream->pulseaudioIsStopped = 1;

    if( stream->outputStream != NULL
        && pa_stream_get_state( stream->outputStream ) == PA_STREAM_READY )
    {
        PaPulseAudio_Lock(stream->mainloop);

        /* Then we cancel all writing (if there is any)
         *  and wait for disconnetion (TERMINATION) which
         * can take a while for ethernet connections
         */
        pa_stream_cancel_write( stream->outputStream );
        pa_stream_disconnect( stream->outputStream );
        PaPulseAudio_UnLock( stream->mainloop );
    }

    if( stream->inputStream != NULL
        && pa_stream_get_state( stream->inputStream ) == PA_STREAM_READY )
    {
        PaPulseAudio_Lock( stream->mainloop );

        /* Then we disconnect stream and wait for
         * Termination
         */
        pa_stream_disconnect( stream->inputStream );

        PaPulseAudio_UnLock( stream->mainloop );

    }

    /* Wait for termination for both */
    while(!l_iLoop)
    {
        PaPulseAudio_Lock( stream->mainloop );
        if( stream->inputStream != NULL
            && pa_stream_get_state( stream->inputStream ) == PA_STREAM_TERMINATED )
        {
            pa_stream_unref( stream->inputStream );
            stream->inputStream = NULL;
        }
        PaPulseAudio_UnLock( stream->mainloop );

        PaPulseAudio_Lock( stream->mainloop );
        if( stream->outputStream != NULL
            && pa_stream_get_state(stream->outputStream) == PA_STREAM_TERMINATED )
        {
            pa_stream_unref( stream->outputStream );
            stream->outputStream = NULL;
        }
        PaPulseAudio_UnLock( stream->mainloop );

        if((stream->outputStream == NULL
           && stream->inputStream == NULL)
           || l_iError >= 5000 )
        {
            l_iLoop = 1;
        }

        l_iError ++;
        usleep(10000);
    }

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );

    PaUtil_FreeMemory( stream->inputStreamName );
    PaUtil_FreeMemory( stream->outputStreamName );
    PaUtil_FreeMemory( stream );

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
    unsigned int l_lRequestFrameSize = (1024 * 2);

    stream->isActive = 0;
    stream->isStopped = 1;
    stream->pulseaudioIsActive = 0;
    stream->pulseaudioIsStopped = 1;
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
    stream->outputBufferAttr.maxlength = (uint32_t)-1;
    stream->inputBufferAttr.maxlength = (uint32_t)-1;

    stream->outputBufferAttr.tlength = (uint32_t)-1;
    stream->inputBufferAttr.tlength = (uint32_t)-1;

    stream->outputBufferAttr.fragsize = (uint32_t)-1;
    stream->inputBufferAttr.fragsize = (uint32_t)-1;

    stream->outputBufferAttr.prebuf = (uint32_t)-1;
    stream->inputBufferAttr.prebuf = (uint32_t)-1;

    stream->outputBufferAttr.minreq = (uint32_t)-1;
    stream->inputBufferAttr.minreq = (uint32_t)-1;

    stream->outputUnderflows = 0;
    PaPulseAudio_UnLock( l_ptrPulseAudioHostApi->mainloop );

    pa_stream_flags_t l_iFlags = PA_STREAM_INTERPOLATE_TIMING |
                                 PA_STREAM_AUTO_TIMING_UPDATE |
                                 PA_STREAM_NO_REMIX_CHANNELS |
                                 PA_STREAM_NO_REMAP_CHANNELS |
                                 PA_STREAM_DONT_MOVE;

    if( stream->inputStream )
    {
        stream->outputBufferAttr.fragsize = 0;

        /* Default input reads 65,535 bytes setting fragsize
         * fragments request to smaller chunks of data so it's
         * easier to get nicer looking timestamps with current
         * callback system
         */
        stream->inputBufferAttr.fragsize = pa_usec_to_bytes( l_lRequestFrameSize,
                                                             &stream->inputSampleSpec );
        stream->inputBufferAttr.prebuf = pa_usec_to_bytes( l_lRequestFrameSize,
                                                           &stream->inputSampleSpec );

        if( stream->inputDevice != paNoDevice)
        {
            PA_DEBUG( ("Portaudio %s: %d (%s)\n", __FUNCTION__, stream->inputDevice,
                      l_ptrPulseAudioHostApi->pulseaudioDeviceNames[stream->
                                                                    inputDevice]) );
        }

        pa_stream_set_read_callback( stream->inputStream,
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
        if( result == paNoError && stream->inputDevice != defaultInputDevice )
        {
            l_strName = l_ptrPulseAudioHostApi->
                        pulseaudioDeviceNames[stream->inputDevice];
        }

        if ( result == paNoError )
        {
            PaPulseAudio_Lock( l_ptrPulseAudioHostApi->mainloop );
            /* Zero means success */
            if( ! pa_stream_connect_record( stream->inputStream,
                                      l_strName,
                                      &stream->inputBufferAttr,
                                      l_iFlags) )
            {
                pa_stream_set_started_callback( stream->inputStream,
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
                l_SState = pa_stream_get_state( stream->inputStream );
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

    if( stream->outputStream )
    {
        /* tlength does almost the same as fragsize in record.
         * See reasoning up there in comments.
         *
         * In future this should we tuned when things changed
         * this just 'good' default
         */
        if( !stream->inputStream )
        {
            stream->outputBufferAttr.tlength = pa_usec_to_bytes( l_lRequestFrameSize,
                                                                 &stream->outputSampleSpec );
            stream->outputBufferAttr.prebuf = pa_usec_to_bytes( l_lRequestFrameSize,
                                                                &stream->outputSampleSpec );
        }

        pa_stream_set_write_callback( stream->outputStream,
                                      PaPulseAudio_StreamPlaybackCb,
                                      stream );

        /* Just keep on trucking if we are just corked */
        if( pa_stream_get_state( stream->outputStream ) == PA_STREAM_READY
            && pa_stream_is_corked( stream->outputStream ) )
        {
            PaPulseAudio_Lock( l_ptrPulseAudioHostApi->mainloop );
            l_ptrOperation = pa_stream_cork( stream->outputStream,
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
            if( stream->outputDevice != paNoDevice )
            {
                PA_DEBUG( ("Portaudio %s: %d (%s)\n",
                          __FUNCTION__,
                          stream->outputDevice,
                          l_ptrPulseAudioHostApi->pulseaudioDeviceNames[stream->
                                                                        outputDevice]) );
            }

            PaDeviceIndex defaultOutputDevice;
            PaError result = PaUtil_DeviceIndexToHostApiDeviceIndex(&defaultOutputDevice,
                             l_ptrPulseAudioHostApi->inheritedHostApiRep.info.defaultOutputDevice,
                             &(l_ptrPulseAudioHostApi->inheritedHostApiRep) );

            /* NULL means default device */
            l_strName = NULL;

            /* If default device is not requested then change to wanted device */
            if( result == paNoError && stream->outputDevice != defaultOutputDevice )
            {
                l_strName = l_ptrPulseAudioHostApi->
                            pulseaudioDeviceNames[stream->outputDevice];
            }

            if(result == paNoError)
            {
                PaPulseAudio_Lock( l_ptrPulseAudioHostApi->mainloop );

                if ( ! pa_stream_connect_playback( stream->outputStream,
                                                   l_strName,
                                                   &stream->outputBufferAttr,
                                                   l_iFlags,
                                                   NULL,
                                                   NULL ) )
                {
                    pa_stream_set_underflow_callback( stream->outputStream,
                                                      PaPulseAudio_StreamUnderflowCb,
                                                      stream);
                    pa_stream_set_started_callback( stream->outputStream,
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
                    l_SState = pa_stream_get_state( stream->outputStream );
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

    if( !stream->outputStream &&
        !stream->inputStream )
    {
        PA_DEBUG( ("Portaudio %s: Streams not initialized!\n",
                  __FUNCTION__) );
        goto startstreamcb_error;
    }

    /* Make sure we pass no error on intialize */
    result = paNoError;

    /* Stream is now active */
    stream->isActive = 1;
    stream->isStopped = 0;

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
    stream->pulseaudioIsActive = 0;
    stream->pulseaudioIsStopped = 1;

    stream->missedBytes = 0;

    /* Test if there is something that we can play */
    if( stream->outputStream
        && pa_stream_get_state( stream->outputStream ) == PA_STREAM_READY
        && !pa_stream_is_corked( stream->outputStream )
        && !abort )
    {
        l_ptrOperation = pa_stream_cork( stream->outputStream,
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
    stream->pulseaudioIsActive = 0;
    stream->pulseaudioIsStopped = 1;

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
