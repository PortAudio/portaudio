
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
    unsigned int isNegative = 0;
    pa_usec_t pulseaudioStreamTime = 0;
    pa_usec_t pulseaudioStreamLatency = 0;

    if( pa_stream_get_time( s,
                            &pulseaudioStreamTime ) == -PA_ERR_NODATA )
    {
        return -PA_ERR_NODATA;
    }
    else
    {
    timeInfo->currentTime = ((PaTime) pulseaudioStreamTime / (PaTime) 1000000);
    }

    if( pa_stream_get_latency( s,
                               &pulseaudioStreamLatency,
                               &isNegative ) == -PA_ERR_NODATA )
    {
        return -PA_ERR_NODATA;
    }
    else
    {
        if( record == 0 )
        {
            timeInfo->outputBufferDacTime = timeInfo->currentTime + ((PaTime) pulseaudioStreamLatency / (PaTime) 1000000);
        }
        else
        {
            timeInfo->inputBufferAdcTime = timeInfo->currentTime - ((PaTime) pulseaudioStreamLatency / (PaTime) 1000000);
        }
    }
    return 0;
}

/* Release pa_operation always same way */
void PaPulseAudio_ReleaseOperation(PaPulseAudio_HostApiRepresentation *hostapi,
                                  pa_operation **operation)
{
    unsigned int waitOperation = 1000;
    pa_operation *localOperation = (*operation);
    pa_operation_state_t localOperationState = PA_OPERATION_RUNNING;

    // As mainly operation is done when running locally
    // done after 1-3 then 1000 is enough to wait if
    // something goes wrong

    while( waitOperation > 0 )
    {

        PaPulseAudio_Lock( hostapi->mainloop );
        localOperationState = pa_operation_get_state( localOperation );

        if( localOperationState == PA_OPERATION_RUNNING )
        {
            pa_threaded_mainloop_wait( hostapi->mainloop );
        }
        else
        {
            // Result is DONE or CANCEL
            PaPulseAudio_UnLock( hostapi->mainloop );
            break;
        }
        PaPulseAudio_UnLock( hostapi->mainloop );

        waitOperation --;
    }

    // No wait if operation have been DONE or CANCELLED
    if( localOperationState == PA_OPERATION_RUNNING)
    {
        PA_DEBUG( ( "Portaudio %s: Operation still running %d!\n",
        __FUNCTION__, localOperationState ) );
    }

    PaPulseAudio_Lock( hostapi->mainloop );
    pa_operation_unref( localOperation );
    operation = NULL;
    PaPulseAudio_UnLock( hostapi->mainloop );
}


/* locks the Pulse Main loop when not called from it */
void PaPulseAudio_Lock( pa_threaded_mainloop *mainloop )
{
    if( !pa_threaded_mainloop_in_thread( mainloop ) ) {
        pa_threaded_mainloop_lock( mainloop );
    }
    else
    {
        PA_DEBUG( ("Portaudio %s: Called from event loop thread as value is: %d (not locked)\n",
            __FUNCTION__,
            pa_threaded_mainloop_in_thread( mainloop )) );
    }
}

/* unlocks the Pulse Main loop when not called from it */
void PaPulseAudio_UnLock( pa_threaded_mainloop *mainloop )
{
    if( !pa_threaded_mainloop_in_thread( mainloop ) ) {
        pa_threaded_mainloop_unlock( mainloop );
    }
    else
    {
        PA_DEBUG( ("Portaudio %s: Called from event loop thread as value is: %d (not unlocked)\n",
            __FUNCTION__,
            pa_threaded_mainloop_in_thread( mainloop )) );
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
    if( PaUtil_GetRingBufferWriteAvailable( ringbuffer ) < length )
    {
        uint8_t tmpBuffer[ PULSEAUDIO_BUFFER_SIZE ];
        PaUtil_ReadRingBuffer( ringbuffer,
                               tmpBuffer,
                               length );
    }

    PaUtil_WriteRingBuffer( ringbuffer,
                            buffer,
                            length );

}

void _PaPulseAudio_Read( PaPulseAudio_Stream *stream,
                         size_t length )
{
    const void *pulseaudioData = NULL;

    /*
     * Idea behind this is that we fill ringbuffer with data
     * that comes from input device. When it's available
     * we'll fill it to callback or blocking reading
     */
    if( pa_stream_peek( stream->inputStream,
                        &pulseaudioData,
                        &length ))
    {
        PA_DEBUG( ("Portaudio %s: Can't read audio!\n",
                  __FUNCTION__) );
    }
    else
    {
        _PaPulseAudio_WriteRingBuffer( &stream->inputRing, pulseaudioData, length );
    }

    pa_stream_drop( stream->inputStream );

    pulseaudioData = NULL;

}

static int _PaPulseAudio_ProcessAudio(PaPulseAudio_Stream *stream,
                                      size_t length)
{
    uint8_t pulseaudioSampleBuffer[PULSEAUDIO_BUFFER_SIZE];
    size_t hostFramesPerBuffer = stream->bufferProcessor.framesPerHostBuffer;
    size_t pulseaudioOutputBytes = 0;
    size_t pulseaudioInputBytes = 0;
    size_t hostFrameCount = 0;
    int isOutputCb = 0;
    int isInputCb = 0;
    PaStreamCallbackTimeInfo timeInfo;
    int ret = paContinue;
    void *bufferData = NULL;
    size_t pulseaudioOutputWritten = 0;

    /* If there is no specified per host buffer then
     * just generate one or but correct one in place
     */
    if( hostFramesPerBuffer == paFramesPerBufferUnspecified )
    {
        if( !stream->framesPerHostCallback )
        {
            /* This just good enough and most
             * Pulseaudio server and ALSA can handle it
             *
             * We should never get here but this is ultimate
             * backup.
             */
            hostFramesPerBuffer = PAPULSEAUDIO_FRAMESPERBUFFERUNSPEC;

            stream->framesPerHostCallback = hostFramesPerBuffer;
        }
        else
        {
            hostFramesPerBuffer = stream->framesPerHostCallback;
        }
    }

    if( stream->outputStream )
    {
        /* Calculate how many bytes goes to one frame */
        pulseaudioInputBytes = pulseaudioOutputBytes = (hostFramesPerBuffer * stream->outputFrameSize);

        if( stream->bufferProcessor.streamCallback )
        {
            isOutputCb = 1;
        }
    }

    /* If we just want to have input but not output (Not Duplex)
     * Use this calculation
     */
    if( stream->inputStream )
    {
        pulseaudioInputBytes = pulseaudioOutputBytes = (hostFramesPerBuffer * stream->inputFrameSize);

        if( stream->bufferProcessor.streamCallback )
        {
            isInputCb = 1;
        }
    }

    /* If we have input which is mono and
     * output which is stereo. We have to copy
     * mono to monomono which is stereo.
     * Then just read half and copy
     */
    if( isOutputCb &&
        stream->outputSampleSpec.channels == 2 &&
        stream->inputSampleSpec.channels == 1)
    {
        pulseaudioInputBytes /= 2;
    }

    if( !stream->isActive && stream->pulseaudioIsActive && stream->outputStream)
    {
        size_t tmpSize = length;

        /* Allocate memory to make it faster to output stuff */
        pa_stream_begin_write( stream->outputStream, &bufferData, &tmpSize );

        /* If bufferData is NULL then output is not ready
         * and we have to wait for it
         */
        if(!bufferData)
        {
            return paNotInitialized;
        }

        memset( bufferData, 0x00, tmpSize);

        pa_stream_write( stream->outputStream,
                         bufferData,
                         length,
                         NULL,
                         0,
                         PA_SEEK_RELATIVE );

        return paContinue;
    }


    do
    {
        /* Make sure that bufferData is NULL that marks there
         * is nothing to output
         */
        if( isOutputCb )
        {
            bufferData = NULL;
        }

        /*
         * If everything fail like stream vanish or mainloop
         * is in error state try to handle error
         */
        PA_PULSEAUDIO_IS_ERROR( stream, paStreamIsStopped )

        /* There is only Record stream so
         * see if we have enough stuff to feed record stream
         * If not then bail out.
         */
        if( isInputCb &&
            PaUtil_GetRingBufferReadAvailable(&stream->inputRing) < pulseaudioInputBytes )
        {
            if(isOutputCb && (pulseaudioOutputWritten < length) && !stream->missedBytes)
            {
                stream->missedBytes = length - pulseaudioOutputWritten;
            }
            else
            {
                stream->missedBytes = 0;
            }
            break;
        }
        else if( pulseaudioOutputWritten >= length)
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
        if( isInputCb )
        {
            PaUtil_ReadRingBuffer( &stream->inputRing,
                                   pulseaudioSampleBuffer,
                                   pulseaudioInputBytes);

            PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor,
                                                0,
                                                pulseaudioSampleBuffer,
                                                stream->inputSampleSpec.channels );

            PaUtil_SetInputFrameCount( &stream->bufferProcessor,
                                       hostFramesPerBuffer );
        }

        /* Is output is enable and buffer data is not NULL
         * then crap pointer to output ringbuffer
         */
        if( isOutputCb )
        {

            size_t tmpSize = pulseaudioOutputBytes;

            /* Allocate memory to make it faster to output stuff */
            pa_stream_begin_write( stream->outputStream, &bufferData, &tmpSize );

            PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor,
                                                 0,
                                                 bufferData,
                                                 stream->outputChannelCount );

            PaUtil_SetOutputFrameCount( &stream->bufferProcessor,
                                        hostFramesPerBuffer );

        }

        hostFrameCount =
                PaUtil_EndBufferProcessing( &stream->bufferProcessor,
                                            &ret );

        PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer,
                                      hostFrameCount );

        /*
         * pa_stream_begin_write gives pointer to ring
         * buffer in Pulseaudio. WhenPaUtil_EndBufferProcessing
         * returns paContinue then is ok to write output.
         *
         * When it's something else than paContinue
         * then we have to cancel writing with
         * with pa_stream_cancel_write.
         *
         * If there is no space in output ringbuffer or for
         * example USB device has dissapiered and pointer
         * is NULL then we just get out of loop as there is not much
         * we can't do
         */
        if( ret == paContinue && isOutputCb && bufferData )
        {
            if( pa_stream_write( stream->outputStream,
                                 bufferData,
                                 pulseaudioOutputBytes,
                                 NULL,
                                 0,
                                 PA_SEEK_RELATIVE ) )
            {
                PA_DEBUG( ("Portaudio %s: Can't write audio!\n",
                          __FUNCTION__) );
            }

            pulseaudioOutputWritten += pulseaudioOutputBytes;
        }
        else if( ret != paContinue && isOutputCb && bufferData )
        {
            pa_stream_cancel_write( stream->outputStream );
            bufferData = NULL;
        }
        else if( isOutputCb && !bufferData )
        {
            ret = -1;
        }
    }
    while( ret == paContinue );



    return ret;
}

void PaPulseAudio_StreamRecordCb( pa_stream * s,
                                  size_t length,
                                  void *userdata )
{
    PaPulseAudio_Stream *pulseaudioStream = (PaPulseAudio_Stream *) userdata;

    _PaPulseAudio_Read( pulseaudioStream, length );

    /* Let's handle when output happens if Duplex
     *
     * Also there is no callback there is no meaning to continue
     * as we have blocking reading
     */
    if( pulseaudioStream->bufferProcessor.streamCallback )
    {
        _PaPulseAudio_ProcessAudio( pulseaudioStream, length );
    }

    pa_threaded_mainloop_signal( pulseaudioStream->mainloop,
                                 0 );
}

void PaPulseAudio_StreamPlaybackCb( pa_stream * s,
                                    size_t length,
                                    void *userdata )
{
    PaPulseAudio_Stream *pulseaudioStream = (PaPulseAudio_Stream *) userdata;

    if( pulseaudioStream->bufferProcessor.streamCallback )
    {
        _PaPulseAudio_ProcessAudio( pulseaudioStream, length );
    }

    pa_threaded_mainloop_signal( pulseaudioStream->mainloop,
                                 0 );
}

/* This is left for future use! */
static void PaPulseAudio_StreamSuccessCb( pa_stream * s,
                                          int success,
                                          void *userdata )
{
    PaPulseAudio_Stream *pulseaudioStream = (PaPulseAudio_Stream *) userdata;
    PA_DEBUG( ("Portaudio %s: %d\n", __FUNCTION__,
              success) );
    pa_threaded_mainloop_signal( pulseaudioStream->mainloop,
                                 0 );
}

/* This is left for future use! */
static void PaPulseAudio_CorkSuccessCb(
    pa_stream * s,
    int success,
    void *userdata
)
{
    PaPulseAudio_Stream *pulseaudioStream = (PaPulseAudio_Stream *) userdata;
    pa_threaded_mainloop_signal( pulseaudioStream->mainloop,
                                 0 );
}


/* This is left for future use! */
void PaPulseAudio_StreamStartedCb( pa_stream * stream,
                                   void *userdata )
{
    PaPulseAudio_Stream *pulseaudioStream = (PaPulseAudio_Stream *) userdata;
    pa_threaded_mainloop_signal( pulseaudioStream->mainloop,
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
    PaPulseAudio_HostApiRepresentation *pulseaudioHostApi = stream->hostapi;
    pa_operation *pulseaudioOperation = NULL;
    int waitLoop = 0;
    int pulseaudioError = 0;

    /* Wait for stream to be stopped */
    stream->isActive = 0;
    stream->isStopped = 1;
    stream->pulseaudioIsActive = 0;
    stream->pulseaudioIsStopped = 1;

    if( stream->outputStream != NULL
        && PA_STREAM_IS_GOOD( pa_stream_get_state( stream->outputStream ) ) )
    {
        PaPulseAudio_Lock(stream->mainloop);
        /* Pause stream so it stops faster */
        pulseaudioOperation = pa_stream_cork( stream->outputStream,
                                              1,
                                              PaPulseAudio_CorkSuccessCb,
                                              stream );
        PaPulseAudio_UnLock( stream->mainloop );

        PaPulseAudio_ReleaseOperation( pulseaudioHostApi,
                                       &pulseaudioOperation );

        PaPulseAudio_Lock(stream->mainloop);

        pa_stream_disconnect( stream->outputStream );
        PaPulseAudio_UnLock( stream->mainloop );
    }

    if( stream->inputStream != NULL
        && PA_STREAM_IS_GOOD( pa_stream_get_state( stream->inputStream ) ) )
    {
        PaPulseAudio_Lock( stream->mainloop );
        /* Pause stream so it stops so it stops faster */
        pulseaudioOperation = pa_stream_cork( stream->inputStream,
                                              1,
                                              PaPulseAudio_CorkSuccessCb,
                                              stream );
        PaPulseAudio_UnLock( stream->mainloop );

        PaPulseAudio_ReleaseOperation( pulseaudioHostApi,
                                       &pulseaudioOperation );

        PaPulseAudio_Lock( stream->mainloop );

        /* Then we disconnect stream and wait for
         * Termination
         */
        pa_stream_disconnect( stream->inputStream );

        PaPulseAudio_UnLock( stream->mainloop );

    }

    /* Wait for termination for both */
    while(!waitLoop)
    {
        PaPulseAudio_Lock( stream->mainloop );
        if( stream->inputStream != NULL
            && !PA_STREAM_IS_GOOD( pa_stream_get_state( stream->inputStream ) ) )
        {
            pa_stream_unref( stream->inputStream );
            stream->inputStream = NULL;
        }
        PaPulseAudio_UnLock( stream->mainloop );

        PaPulseAudio_Lock( stream->mainloop );
        if( stream->outputStream != NULL
            && !PA_STREAM_IS_GOOD( pa_stream_get_state( stream->outputStream ) ) )
        {
            pa_stream_unref( stream->outputStream );
            stream->outputStream = NULL;
        }
        PaPulseAudio_UnLock( stream->mainloop );

        if((stream->outputStream == NULL
           && stream->inputStream == NULL)
           || pulseaudioError >= 5000 )
        {
            waitLoop = 1;
        }

        pulseaudioError ++;
        usleep(10000);
    }

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );

    PaUtil_FreeMemory( stream->inputStreamName );
    PaUtil_FreeMemory( stream->outputStreamName );
    PaUtil_FreeMemory( stream );

    return result;
}

PaError _PaPulseAudio_WaitStreamState( pa_threaded_mainloop *mainloop, pa_stream * stream )
{
    pa_stream_state_t state = PA_STREAM_UNCONNECTED;
    unsigned int wait = 0;
    PaError result = paNoError;

    while( wait < 1000 )
    {
        pa_threaded_mainloop_wait( mainloop );
        PaPulseAudio_Lock( mainloop );
        state = pa_stream_get_state( stream );
        PaPulseAudio_UnLock( mainloop );

        switch(state)
        {
            case PA_STREAM_READY:
                result = paNoError;
                wait = 10000;
                break;
            case PA_STREAM_FAILED:
                PA_DEBUG( ("Portaudio %s: Creating stream failed. (PA_STREAM_FAILED)",
                           __FUNCTION__) );
                result = paNotInitialized;
                wait = 10000;
                break;
            case PA_STREAM_TERMINATED:
                PA_DEBUG( ("Portaudio %s: Stream terminated. (PA_STREAM_TERMINATED)",
                           __FUNCTION__) );
                result = paNotInitialized;
                wait = 10000;
                break;
        }

        /* Creating can take some time */
        if( state != PA_STREAM_CREATING )
        {
            wait ++;
        }
    }

    return result;
}

PaError PaPulseAudio_StartStreamCb( PaStream * s )
{
    PaError ret = paNoError;
    PaPulseAudio_Stream *stream = (PaPulseAudio_Stream *) s;
    int pulseaudioPlaybackStarted = 0;
    int pulseaudioRecordStarted = 0;
    pa_stream_state_t pulseaudioState = PA_STREAM_UNCONNECTED;
    PaPulseAudio_HostApiRepresentation *pulseaudioHostApi = stream->hostapi;
    const char *pulseaudioName = NULL;
    pa_operation *pulseaudioOperation = NULL;
    unsigned int pulseaudioReqFrameSize = stream->suggestedLatencyUSecs;

    stream->isActive = 0;
    stream->isStopped = 1;
    stream->pulseaudioIsActive = 1;
    stream->pulseaudioIsStopped = 0;
    stream->missedBytes = 0;

    /* Ready the processor */
    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );

    PaPulseAudio_Lock( pulseaudioHostApi->mainloop );
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
    PaPulseAudio_UnLock( pulseaudioHostApi->mainloop );

    pa_stream_flags_t pulseaudioStreamFlags = PA_STREAM_INTERPOLATE_TIMING |
                                 PA_STREAM_AUTO_TIMING_UPDATE |
                                 PA_STREAM_ADJUST_LATENCY |
                                 PA_STREAM_NO_REMIX_CHANNELS |
                                 PA_STREAM_NO_REMAP_CHANNELS |
                                 PA_STREAM_DONT_MOVE;

    if( stream->inputStream )
    {
        /* Default input reads 65,535 bytes setting fragsize
         * fragments request to smaller chunks of data so it's
         * easier to get nicer looking timestamps with current
         * callback system
         */
        stream->inputBufferAttr.fragsize = pa_usec_to_bytes( pulseaudioReqFrameSize,
                                                             &stream->inputSampleSpec );

        if( stream->inputDevice != paNoDevice)
        {
            PA_DEBUG( ("Portaudio %s: %d (%s)\n", __FUNCTION__, stream->inputDevice,
                      pulseaudioHostApi->pulseaudioDeviceNames[stream->
                                                                    inputDevice]) );
        }

        PaDeviceIndex defaultInputDevice;
        PaError result = PaUtil_DeviceIndexToHostApiDeviceIndex(
                &defaultInputDevice,
                pulseaudioHostApi->inheritedHostApiRep.info.defaultInputDevice,
                &(pulseaudioHostApi->inheritedHostApiRep) );

        /* NULL means default device */
        pulseaudioName = NULL;

        /* If default device is not requested then change to wanted device */
        if( result == paNoError && stream->inputDevice != defaultInputDevice )
        {
            pulseaudioName = pulseaudioHostApi->
                        pulseaudioDeviceNames[stream->inputDevice];
        }

        if ( result == paNoError )
        {
            PaPulseAudio_Lock( pulseaudioHostApi->mainloop );
            /* Zero means success */
            if( pa_stream_connect_record( stream->inputStream,
                                          pulseaudioName,
                                          &stream->inputBufferAttr,
                                          pulseaudioStreamFlags ) )
            {
                PA_DEBUG( ("Portaudio %s: Can't read audio!\n",
                          __FUNCTION__) );
                PaPulseAudio_UnLock( pulseaudioHostApi->mainloop );

                goto startstreamcb_error;
            }
            PaPulseAudio_UnLock( pulseaudioHostApi->mainloop );

            if( _PaPulseAudio_WaitStreamState( pulseaudioHostApi->mainloop, stream->inputStream ) != paNoError )
            {
                goto startstreamcb_error;
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
        stream->outputBufferAttr.tlength = pa_usec_to_bytes( pulseaudioReqFrameSize,
                                                             &stream->outputSampleSpec );

        /* Just keep on trucking if we are just corked */
        if( pa_stream_get_state( stream->outputStream ) == PA_STREAM_READY
            && pa_stream_is_corked( stream->outputStream ) )
        {
            PaPulseAudio_Lock( pulseaudioHostApi->mainloop );
            pulseaudioOperation = pa_stream_cork( stream->outputStream,
                                            0,
                                            PaPulseAudio_CorkSuccessCb,
                                            stream );
            PaPulseAudio_UnLock( pulseaudioHostApi->mainloop );

            PaPulseAudio_ReleaseOperation( pulseaudioHostApi,
                                           &pulseaudioOperation );
        }
        else
        {
            if( stream->outputDevice != paNoDevice )
            {
                PA_DEBUG( ("Portaudio %s: %d (%s)\n",
                          __FUNCTION__,
                          stream->outputDevice,
                          pulseaudioHostApi->pulseaudioDeviceNames[stream->
                                                            outputDevice]) );
            }

            PaDeviceIndex defaultOutputDevice;
            PaError result = PaUtil_DeviceIndexToHostApiDeviceIndex( &defaultOutputDevice,
                             pulseaudioHostApi->inheritedHostApiRep.info.defaultOutputDevice,
                             &(pulseaudioHostApi->inheritedHostApiRep) );

            /* NULL means default device */
            pulseaudioName = NULL;

            /* If default device is not requested then change to wanted device */
            if( result == paNoError && stream->outputDevice != defaultOutputDevice )
            {
                pulseaudioName = pulseaudioHostApi->
                                    pulseaudioDeviceNames[stream->outputDevice];
            }

            if(result == paNoError)
            {
                PaPulseAudio_Lock( pulseaudioHostApi->mainloop );

                /* This is only needed when making non duplex
                 * as when duplexing then input should feed
                 * output and we don't need playback callback
                 */
                if( !stream->inputStream )
                {
                    pa_stream_set_write_callback( stream->outputStream,
                                                  PaPulseAudio_StreamPlaybackCb,
                                                  stream );
                }

                if ( pa_stream_connect_playback( stream->outputStream,
                                                   pulseaudioName,
                                                   &stream->outputBufferAttr,
                                                   pulseaudioStreamFlags,
                                                   NULL,
                                                   NULL ) )
                {
                    PA_DEBUG( ("Portaudio %s: Can't write audio!\n",
                              __FUNCTION__) );
                    PaPulseAudio_UnLock( pulseaudioHostApi->mainloop );
                    goto startstreamcb_error;
                }
                PaPulseAudio_UnLock( pulseaudioHostApi->mainloop );

                if( _PaPulseAudio_WaitStreamState( pulseaudioHostApi->mainloop, stream->outputStream ) != paNoError )
                {
                    goto startstreamcb_error;
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
    ret = paNoError;

    /* Stream is now active */
    stream->isActive = 1;
    stream->isStopped = 0;

    /* Start callback here after we can be
     * sure that everything is correct
     */
    if( stream->inputStream )
    {
        pa_stream_set_read_callback( stream->inputStream,
                                     PaPulseAudio_StreamRecordCb,
                                     stream );
    }

    /* Allways unlock.. so we don't get locked */
    startstreamcb_end:
    return ret;

    error:
    startstreamcb_error:
    PA_DEBUG( ("Portaudio %s: Can't start audio!\n",
              __FUNCTION__) );

    if( pulseaudioPlaybackStarted || pulseaudioRecordStarted )
    {
        PaPulseAudio_AbortStreamCb( stream );
    }

    stream->isActive = 0;
    stream->isStopped = 1;
    ret = paNotInitialized;

    goto startstreamcb_end;
}

static PaError RequestStop( PaPulseAudio_Stream * stream,
                     int abort )
{
    PaError ret = paNoError;
    PaPulseAudio_HostApiRepresentation *pulseaudioHostApi = stream->hostapi;
    pa_operation *pulseaudioOperation = NULL;

    PaPulseAudio_Lock( pulseaudioHostApi->mainloop );

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
        pulseaudioOperation = pa_stream_cork( stream->outputStream,
                                              1,
                                              PaPulseAudio_CorkSuccessCb,
                                              stream );

        PaPulseAudio_UnLock( pulseaudioHostApi->mainloop );
        PaPulseAudio_ReleaseOperation( pulseaudioHostApi,
                                       &pulseaudioOperation );
        PaPulseAudio_Lock( pulseaudioHostApi->mainloop );
    }

    requeststop_error:
    PaPulseAudio_UnLock( pulseaudioHostApi->mainloop );
    stream->isActive = 0;
    stream->isStopped = 1;
    stream->pulseaudioIsActive = 0;
    stream->pulseaudioIsStopped = 1;

    return ret;
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
