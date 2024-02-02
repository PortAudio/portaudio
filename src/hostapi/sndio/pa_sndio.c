/*
 * Copyright (c) 2009 Alexandre Ratchov <alex@caoua.org>
 * Copyright (c) 2021 Haelwenn (lanodan) Monnier <contact@hacktivis.me>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sndio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "pa_allocation.h"
#include "pa_debugprint.h"
#include "pa_hostapi.h"
#include "pa_process.h"
#include "pa_stream.h"
#include "pa_util.h"

/*
 * per-stream data
 */
typedef struct PaSndioStream
{
    PaUtilStreamRepresentation base;
    PaUtilBufferProcessor bufferProcessor; /* format conversion */
    struct sio_hdl *hdl; /* handle for device i/o */
    struct sio_par par; /* current device parameters */
    unsigned mode; /* SIO_PLAY, SIO_REC or both */
    int stopped; /* stop requested or not started */
    int active; /* thread is running */
    unsigned long long realpos; /* frame number h/w is processing */
    char *rbuf, *wbuf; /* bounce buffers for conversions */
    unsigned long long rpos, wpos; /* bytes read/written */
    pthread_t thread; /* thread of the callback interface */
} PaSndioStream;

/*
 * api "class" data, common to all streams
 */
typedef struct PaSndioHostApiRepresentation
{
    PaUtilHostApiRepresentation base;
    PaUtilStreamInterface callback;
    PaUtilStreamInterface blocking;
    /*
     * sndio has no device discovery mechanism and PortAudio has
     * no way of accepting raw device strings from users.
     * Normally we just expose the default device, which can be
     * changed via the AUDIODEVICE environment variable, but we
     * also allow specifying a list of up to 16 devices via the
     * PA_SNDIO_AUDIODEVICES environment variable.
     *
     * Example:
     * PA_SNDIO_AUDIODEVICES=default:snd/0.monitor:snd@remote/0
     */
#define PA_SNDIO_AUDIODEVICES_MAX 16
    PaDeviceInfo deviceInfos[PA_SNDIO_AUDIODEVICES_MAX];
    PaDeviceInfo *deviceInfoPtrs[PA_SNDIO_AUDIODEVICES_MAX];
    char *audioDevices;
} PaSndioHostApiRepresentation;

/*
 * callback invoked when blocks are processed by the hardware
 */
static void sndioOnMove( void *addr, int delta )
{
    PaSndioStream *sndioStream = (PaSndioStream *)addr;

    sndioStream->realpos += delta;
}

/*
 * convert PA encoding to sndio encoding, return true on success
 */
static int sndioSetFmt( struct sio_par *par, PaSampleFormat fmt )
{
    switch( fmt & ~paNonInterleaved )
    {
    case paInt32:
    case paFloat32:
        par->sig = 1;
        par->bits = 32;
        break;
    case paInt24:
        par->sig = 1;
        par->bits = 24;
        par->bps = 3; /* paInt24 is packed format */
        break;
    case paInt16:
        par->sig = 1;
        par->bits = 16;
        break;
    case paInt8:
        par->sig = 1;
        par->bits = 8;
        break;
    case paUInt8:
        par->sig = 0;
        par->bits = 8;
        break;
    default:
        PA_DEBUG( ( "sndioSetFmt: %x: unsupported\n", fmt ) );
        return 0;
    }
    par->le = SIO_LE_NATIVE;
    return 1;
}

/*
 * convert sndio encoding to PA encoding, return true on success
 */
static int sndioGetFmt( struct sio_par *par, PaSampleFormat *fmt )
{
    if( ( par->bps * 8 != par->bits && !par->msb ) || ( par->bps > 1 && par->le != SIO_LE_NATIVE ) )
    {
        PA_DEBUG( ( "sndioGetFmt: bits = %u, le = %u, msb = %u, bps = %u\n", par->bits, par->le, par->msb, par->bps ) );
        return 0;
    }

    switch( par->bits )
    {
    case 32:
        if( !par->sig )
            return 0;
        *fmt = paInt32;
        break;
    case 24:
        if( !par->sig )
            return 0;
        *fmt = ( par->bps == 3 ) ? paInt24 : paInt32;
        break;
    case 16:
        if( !par->sig )
            return 0;
        *fmt = paInt16;
        break;
    case 8:
        *fmt = par->sig ? paInt8 : paUInt8;
        break;
    default:
        PA_DEBUG( ( "sndioGetFmt: %u: unsupported\n", par->bits ) );
        return 0;
    }
    return 1;
}

/*
 * I/O loop for callback interface
 */
static void *sndioThread( void *arg )
{
    PaSndioStream *sndioStream = (PaSndioStream *)arg;
    PaStreamCallbackTimeInfo timeInfo;
    unsigned char *data;
    unsigned todo, rblksz, wblksz;
    int n, result;

    rblksz = sndioStream->par.round * sndioStream->par.rchan * sndioStream->par.bps;
    wblksz = sndioStream->par.round * sndioStream->par.pchan * sndioStream->par.bps;

    PA_DEBUG( ( "sndioThread: mode = %x, round = %u, rblksz = %u, wblksz = %u\n", sndioStream->mode,
                sndioStream->par.round, rblksz, wblksz ) );

    while( !sndioStream->stopped )
    {
        if( sndioStream->mode & SIO_REC )
        {
            todo = rblksz;
            data = sndioStream->rbuf;
            while( todo > 0 )
            {
                n = sio_read( sndioStream->hdl, data, todo );
                if( n == 0 )
                {
                    PA_DEBUG( ( "sndioThread: sio_read failed\n" ) );
                    goto failed;
                }
                todo -= n;
                data += n;
            }
            sndioStream->rpos += sndioStream->par.round;
            timeInfo.inputBufferAdcTime = (double)sndioStream->realpos / sndioStream->par.rate;
        }
        if( sndioStream->mode & SIO_PLAY )
        {
            timeInfo.outputBufferDacTime =
                (double)( sndioStream->realpos + sndioStream->par.bufsz ) / sndioStream->par.rate;
        }
        timeInfo.currentTime = sndioStream->realpos / (double)sndioStream->par.rate;
        PaUtil_BeginBufferProcessing( &sndioStream->bufferProcessor, &timeInfo, 0 );
        if( sndioStream->mode & SIO_PLAY )
        {
            PaUtil_SetOutputFrameCount( &sndioStream->bufferProcessor, sndioStream->par.round );
            PaUtil_SetInterleavedOutputChannels( &sndioStream->bufferProcessor, 0, sndioStream->wbuf,
                                                 sndioStream->par.pchan );
        }
        if( sndioStream->mode & SIO_REC )
        {
            PaUtil_SetInputFrameCount( &sndioStream->bufferProcessor, sndioStream->par.round );
            PaUtil_SetInterleavedInputChannels( &sndioStream->bufferProcessor, 0, sndioStream->rbuf,
                                                sndioStream->par.rchan );
        }
        result = paContinue;
        n = PaUtil_EndBufferProcessing( &sndioStream->bufferProcessor, &result );
        if( n != sndioStream->par.round )
        {
            PA_DEBUG( ( "sndioThread: %d < %u frames, result = %d\n", n, sndioStream->par.round, result ) );
        }
        if( result != paContinue )
        {
            break;
        }
        if( sndioStream->mode & SIO_PLAY )
        {
            n = sio_write( sndioStream->hdl, sndioStream->wbuf, wblksz );
            if( n < wblksz )
            {
                PA_DEBUG( ( "sndioThread: sio_write failed\n" ) );
                goto failed;
            }
            sndioStream->wpos += sndioStream->par.round;
        }
    }
failed:
    sndioStream->active = 0;
    PA_DEBUG( ( "sndioThread: done\n" ) );
    return NULL;
}

static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi, PaStream **paStream,
                           const PaStreamParameters *inputPar, const PaStreamParameters *outputPar, double sampleRate,
                           unsigned long framesPerBuffer, PaStreamFlags streamFlags, PaStreamCallback *streamCallback,
                           void *userData )
{
    PaSndioHostApiRepresentation *sndioHostApi = (PaSndioHostApiRepresentation *)hostApi;
    PaSndioStream *sndioStream;
    PaError err;
    struct sio_hdl *hdl;
    struct sio_par par;
    unsigned mode;
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputFormat, outputFormat, sndioFormat;
    const char *dev;

    PA_DEBUG( ( "OpenStream:\n" ) );

    mode = 0;
    inputChannelCount = outputChannelCount = 0;
    inputFormat = outputFormat = 0;
    sio_initpar( &par );

    if( outputPar && outputPar->channelCount > 0 )
    {
        if( outputPar->device >= sndioHostApi->base.info.deviceCount )
        {
            PA_DEBUG( ( "OpenStream: %d: bad output device\n", outputPar->device ) );
            return paInvalidDevice;
        }
        if( outputPar->hostApiSpecificStreamInfo )
        {
            PA_DEBUG( ( "OpenStream: output specific info\n" ) );
            return paIncompatibleHostApiSpecificStreamInfo;
        }
        if( !sndioSetFmt( &par, outputPar->sampleFormat ) )
        {
            return paSampleFormatNotSupported;
        }
        outputFormat = outputPar->sampleFormat;
        outputChannelCount = par.pchan = outputPar->channelCount;
        mode |= SIO_PLAY;
    }
    if( inputPar && inputPar->channelCount > 0 )
    {
        if( inputPar->device >= sndioHostApi->base.info.deviceCount )
        {
            PA_DEBUG( ( "OpenStream: %d: bad input device\n", inputPar->device ) );
            return paInvalidDevice;
        }
        if( inputPar->hostApiSpecificStreamInfo )
        {
            PA_DEBUG( ( "OpenStream: input specific info\n" ) );
            return paIncompatibleHostApiSpecificStreamInfo;
        }
        if( !sndioSetFmt( &par, inputPar->sampleFormat ) )
        {
            return paSampleFormatNotSupported;
        }
        inputFormat = inputPar->sampleFormat;
        inputChannelCount = par.rchan = inputPar->channelCount;
        mode |= SIO_REC;
    }
    par.rate = sampleRate;
    if( framesPerBuffer != paFramesPerBufferUnspecified )
        par.round = framesPerBuffer;

    PA_DEBUG( ( "OpenStream: mode = %x, trying rate = %u\n", mode, par.rate ) );

    if( outputPar )
    {
        dev = sndioHostApi->deviceInfos[outputPar->device].name;
    }
    else if( inputPar )
    {
        dev = sndioHostApi->deviceInfos[inputPar->device].name;
    }
    else
    {
        return paUnanticipatedHostError;
    }
    hdl = sio_open( dev, mode, 0 );
    if( hdl == NULL )
        return paUnanticipatedHostError;
    if( !sio_setpar( hdl, &par ) )
    {
        sio_close( hdl );
        return paUnanticipatedHostError;
    }
    if( !sio_getpar( hdl, &par ) )
    {
        sio_close( hdl );
        return paUnanticipatedHostError;
    }
    if( !sndioGetFmt( &par, &sndioFormat ) )
    {
        sio_close( hdl );
        return paSampleFormatNotSupported;
    }
    if( ( mode & SIO_REC ) && par.rchan != inputPar->channelCount )
    {
        PA_DEBUG( ( "OpenStream: rchan(%u) != %d\n", par.rchan, inputPar->channelCount ) );
        sio_close( hdl );
        return paInvalidChannelCount;
    }
    if( ( mode & SIO_PLAY ) && par.pchan != outputPar->channelCount )
    {
        PA_DEBUG( ( "OpenStream: pchan(%u) != %d\n", par.pchan, outputPar->channelCount ) );
        sio_close( hdl );
        return paInvalidChannelCount;
    }
    if( (double)par.rate < sampleRate * 0.995 || (double)par.rate > sampleRate * 1.005 )
    {
        PA_DEBUG( ( "OpenStream: rate(%u) != %g\n", par.rate, sampleRate ) );
        sio_close( hdl );
        return paInvalidSampleRate;
    }

    sndioStream = (PaSndioStream *)PaUtil_AllocateZeroInitializedMemory( sizeof( PaSndioStream ) );
    if( sndioStream == NULL )
    {
        sio_close( hdl );
        return paInsufficientMemory;
    }
    PaUtil_InitializeStreamRepresentation( &sndioStream->base,
                                           streamCallback ? &sndioHostApi->callback : &sndioHostApi->blocking,
                                           streamCallback, userData );
    PA_DEBUG( ( "inputChannelCount = %d, outputChannelCount = %d, inputFormat = "
                "%x, outputFormat = %x\n",
                inputChannelCount, outputChannelCount, inputFormat, outputFormat ) );
    err = PaUtil_InitializeBufferProcessor( &sndioStream->bufferProcessor, inputChannelCount, inputFormat, sndioFormat,
                                            outputChannelCount, outputFormat, sndioFormat, sampleRate, streamFlags,
                                            framesPerBuffer, par.round, paUtilFixedHostBufferSize, streamCallback,
                                            userData );
    if( err )
    {
        PA_DEBUG( ( "OpenStream: PaUtil_InitializeBufferProcessor failed\n" ) );
        PaUtil_FreeMemory( sndioStream );
        sio_close( hdl );
        return err;
    }
    if( mode & SIO_REC )
    {
        sndioStream->rbuf = malloc( par.round * par.rchan * par.bps );
        if( sndioStream->rbuf == NULL )
        {
            PA_DEBUG( ( "OpenStream: failed to allocate rbuf\n" ) );
            PaUtil_FreeMemory( sndioStream );
            sio_close( hdl );
            return paInsufficientMemory;
        }
    }
    if( mode & SIO_PLAY )
    {
        sndioStream->wbuf = malloc( par.round * par.pchan * par.bps );
        if( sndioStream->wbuf == NULL )
        {
            PA_DEBUG( ( "OpenStream: failed to allocate wbuf\n" ) );
            free( sndioStream->rbuf );
            PaUtil_FreeMemory( sndioStream );
            sio_close( hdl );
            return paInsufficientMemory;
        }
    }
    sndioStream->base.streamInfo.inputLatency = 0;
    sndioStream->base.streamInfo.outputLatency =
        ( mode & SIO_PLAY )
            ? (double)( par.bufsz + PaUtil_GetBufferProcessorOutputLatencyFrames( &sndioStream->bufferProcessor ) ) /
                  (double)par.rate
            : 0;
    sndioStream->base.streamInfo.sampleRate = par.rate;
    sndioStream->active = 0;
    sndioStream->stopped = 1;
    sndioStream->mode = mode;
    sndioStream->hdl = hdl;
    sndioStream->par = par;
    *paStream = sndioStream;
    PA_DEBUG( ( "OpenStream: done\n" ) );
    return paNoError;
}

static PaError BlockingReadStream( PaStream *paStream, void *data, unsigned long numFrames )
{
    PaSndioStream *sndioStream = (PaSndioStream *)paStream;
    unsigned n, res, todo;
    void *buf;

    while( numFrames > 0 )
    {
        n = sndioStream->par.round;
        if( n > numFrames )
            n = numFrames;
        buf = sndioStream->rbuf;
        todo = n * sndioStream->par.rchan * sndioStream->par.bps;
        while( todo > 0 )
        {
            res = sio_read( sndioStream->hdl, buf, todo );
            if( res == 0 )
                return paUnanticipatedHostError;
            buf = (char *)buf + res;
            todo -= res;
        }
        sndioStream->rpos += n;
        PaUtil_SetInputFrameCount( &sndioStream->bufferProcessor, n );
        PaUtil_SetInterleavedInputChannels( &sndioStream->bufferProcessor, 0, sndioStream->rbuf,
                                            sndioStream->par.rchan );
        res = PaUtil_CopyInput( &sndioStream->bufferProcessor, &data, n );
        if( res != n )
        {
            PA_DEBUG( ( "BlockingReadStream: copyInput: %u != %u\n" ) );
            return paUnanticipatedHostError;
        }
        numFrames -= n;
    }
    return paNoError;
}

static PaError BlockingWriteStream( PaStream *paStream, const void *data, unsigned long numFrames )
{
    PaSndioStream *sndioStream = (PaSndioStream *)paStream;
    unsigned n, res;

    while( numFrames > 0 )
    {
        n = sndioStream->par.round;
        if( n > numFrames )
            n = numFrames;
        PaUtil_SetOutputFrameCount( &sndioStream->bufferProcessor, n );
        PaUtil_SetInterleavedOutputChannels( &sndioStream->bufferProcessor, 0, sndioStream->wbuf,
                                             sndioStream->par.pchan );
        res = PaUtil_CopyOutput( &sndioStream->bufferProcessor, &data, n );
        if( res != n )
        {
            PA_DEBUG( ( "BlockingWriteStream: copyOutput: %u != %u\n" ) );
            return paUnanticipatedHostError;
        }
        res = sio_write( sndioStream->hdl, sndioStream->wbuf, n * sndioStream->par.pchan * sndioStream->par.bps );
        if( res == 0 )
            return paUnanticipatedHostError;
        sndioStream->wpos += n;
        numFrames -= n;
    }
    return paNoError;
}

static signed long BlockingGetStreamReadAvailable( PaStream *paStream )
{
    PaSndioStream *sndioStream = (PaSndioStream *)paStream;
    struct pollfd pfd;
    int n, events;

    n = sio_pollfd( sndioStream->hdl, &pfd, POLLIN );
    while( poll( &pfd, n, 0 ) < 0 )
    {
        if( errno == EINTR )
            continue;
        perror( "poll" );
        abort();
    }
    events = sio_revents( sndioStream->hdl, &pfd );
    if( !( events & POLLIN ) )
        return 0;

    return sndioStream->realpos - sndioStream->rpos;
}

static signed long BlockingGetStreamWriteAvailable( PaStream *paStream )
{
    PaSndioStream *sndioStream = (PaSndioStream *)paStream;
    struct pollfd pfd;
    int n, events;

    n = sio_pollfd( sndioStream->hdl, &pfd, POLLOUT );
    while( poll( &pfd, n, 0 ) < 0 )
    {
        if( errno == EINTR )
            continue;
        perror( "poll" );
        abort();
    }
    events = sio_revents( sndioStream->hdl, &pfd );
    if( !( events & POLLOUT ) )
        return 0;

    return sndioStream->par.bufsz - ( sndioStream->wpos - sndioStream->realpos );
}

static PaError BlockingWaitEmpty( PaStream *paStream )
{
    PaSndioStream *sndioStream = (PaSndioStream *)paStream;

    /*
     * drain playback buffers; sndio always does it in background
     * and there is no way to wait for completion
     */
    PA_DEBUG( ( "BlockingWaitEmpty: s=%d, a=%d\n", sndioStream->stopped, sndioStream->active ) );

    return paNoError;
}

static PaError StartStream( PaStream *paStream )
{
    PaSndioStream *sndioStream = (PaSndioStream *)paStream;
    unsigned primes, wblksz;
    int err;

    PA_DEBUG( ( "StartStream: s=%d, a=%d\n", sndioStream->stopped, sndioStream->active ) );

    if( !sndioStream->stopped )
    {
        PA_DEBUG( ( "StartStream: already started\n" ) );
        return paNoError;
    }
    sndioStream->stopped = 0;
    sndioStream->active = 1;
    sndioStream->realpos = 0;
    sndioStream->wpos = 0;
    sndioStream->rpos = 0;
    PaUtil_ResetBufferProcessor( &sndioStream->bufferProcessor );
    if( !sio_start( sndioStream->hdl ) )
        return paUnanticipatedHostError;

    /*
     * send a complete buffer of silence
     */
    if( sndioStream->mode & SIO_PLAY )
    {
        wblksz = sndioStream->par.round * sndioStream->par.pchan * sndioStream->par.bps;
        memset( sndioStream->wbuf, 0, wblksz );
        for( primes = sndioStream->par.bufsz / sndioStream->par.round; primes > 0; primes-- )
            sndioStream->wpos += sio_write( sndioStream->hdl, sndioStream->wbuf, wblksz );
    }
    if( sndioStream->base.streamCallback )
    {
        err = pthread_create( &sndioStream->thread, NULL, sndioThread, sndioStream );
        if( err )
        {
            PA_DEBUG( ( "SndioStartStream: couldn't create thread\n" ) );
            return paUnanticipatedHostError;
        }
        PA_DEBUG( ( "StartStream: started...\n" ) );
    }
    return paNoError;
}

static PaError StopStream( PaStream *paStream )
{
    PaSndioStream *sndioStream = (PaSndioStream *)paStream;
    void *ret;
    int err;

    PA_DEBUG( ( "StopStream: s=%d, a=%d\n", sndioStream->stopped, sndioStream->active ) );

    if( sndioStream->stopped )
    {
        PA_DEBUG( ( "StartStream: already started\n" ) );
        return paNoError;
    }
    sndioStream->stopped = 1;
    if( sndioStream->base.streamCallback )
    {
        err = pthread_join( sndioStream->thread, &ret );
        if( err )
        {
            PA_DEBUG( ( "SndioStop: couldn't join thread\n" ) );
            return paUnanticipatedHostError;
        }
    }
    if( !sio_stop( sndioStream->hdl ) )
        return paUnanticipatedHostError;
    return paNoError;
}

static PaError CloseStream( PaStream *paStream )
{
    PaSndioStream *sndioStream = (PaSndioStream *)paStream;

    PA_DEBUG( ( "CloseStream:\n" ) );

    if( !sndioStream->stopped )
        StopStream( paStream );

    if( sndioStream->mode & SIO_REC )
        free( sndioStream->rbuf );
    if( sndioStream->mode & SIO_PLAY )
        free( sndioStream->wbuf );
    sio_close( sndioStream->hdl );
    PaUtil_TerminateStreamRepresentation( &sndioStream->base );
    PaUtil_TerminateBufferProcessor( &sndioStream->bufferProcessor );
    PaUtil_FreeMemory( sndioStream );
    return paNoError;
}

static PaError AbortStream( PaStream *paStream )
{
    PA_DEBUG( ( "AbortStream:\n" ) );

    return StopStream( paStream );
}

static PaError IsStreamStopped( PaStream *paStream )
{
    PaSndioStream *sndioStream = (PaSndioStream *)paStream;

    // PA_DEBUG(("IsStreamStopped: s=%d, a=%d\n", sndioStream->stopped,
    // sndioStream->active));

    return sndioStream->stopped;
}

static PaError IsStreamActive( PaStream *paStream )
{
    PaSndioStream *sndioStream = (PaSndioStream *)paStream;

    // PA_DEBUG(("IsStreamActive: s=%d, a=%d\n", sndioStream->stopped,
    // sndioStream->active));

    return sndioStream->active;
}

static PaTime GetStreamTime( PaStream *paStream )
{
    PaSndioStream *sndioStream = (PaSndioStream *)paStream;

    return (double)sndioStream->realpos / sndioStream->base.streamInfo.sampleRate;
}

static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi, const PaStreamParameters *inputPar,
                                  const PaStreamParameters *outputPar, double sampleRate )
{
    return paFormatIsSupported;
}

static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaSndioHostApiRepresentation *sndioHostApi;
    sndioHostApi = (PaSndioHostApiRepresentation *)hostApi;
    free( sndioHostApi->audioDevices );
    PaUtil_FreeMemory( hostApi );
}

static void InitDeviceInfo( PaDeviceInfo *info, PaHostApiIndex hostApiIndex, const char *name )
{
    info->structVersion = 2;
    info->name = name;
    info->hostApi = hostApiIndex;
    info->maxInputChannels = 128;
    info->maxOutputChannels = 128;
    info->defaultLowInputLatency = 0.01;
    info->defaultLowOutputLatency = 0.01;
    info->defaultHighInputLatency = 0.5;
    info->defaultHighOutputLatency = 0.5;
    info->defaultSampleRate = 48000;
}

PaError PaSndio_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaSndioHostApiRepresentation *sndioHostApi;
    PaDeviceInfo *info;
    struct sio_hdl *hdl;
    char *audioDevices;
    char *device;
    size_t deviceCount;

    PA_DEBUG( ( "PaSndio_Initialize: initializing...\n" ) );

    /* unusable APIs should return paNoError and a NULL hostApi */
    *hostApi = NULL;

    sndioHostApi = PaUtil_AllocateZeroInitializedMemory( sizeof( PaSndioHostApiRepresentation ) );
    if( sndioHostApi == NULL )
        return paNoError;

    // Add default device
    info = &sndioHostApi->deviceInfos[0];
    InitDeviceInfo( info, hostApiIndex, SIO_DEVANY );
    sndioHostApi->deviceInfoPtrs[0] = info;
    deviceCount = 1;

    // Add additional devices as specified in the PA_SNDIO_AUDIODEVICES
    // environment variable as a colon separated list
    sndioHostApi->audioDevices = NULL;
    audioDevices = getenv( "PA_SNDIO_AUDIODEVICES" );
    if( audioDevices != NULL )
    {
        sndioHostApi->audioDevices = strdup( audioDevices );
        if( sndioHostApi->audioDevices == NULL )
            return paNoError;

        audioDevices = sndioHostApi->audioDevices;
        while( ( device = strsep( &audioDevices, ":" ) ) != NULL && deviceCount < PA_SNDIO_AUDIODEVICES_MAX )
        {
            if( *device == '\0' )
                continue;
            info = &sndioHostApi->deviceInfos[deviceCount];
            InitDeviceInfo( info, hostApiIndex, device );
            sndioHostApi->deviceInfoPtrs[deviceCount] = info;
            deviceCount++;
        }
    }

    *hostApi = &sndioHostApi->base;
    ( *hostApi )->info.structVersion = 1;
    ( *hostApi )->info.type = paSndio;
    ( *hostApi )->info.name = "sndio";
    ( *hostApi )->info.deviceCount = deviceCount;
    ( *hostApi )->info.defaultInputDevice = 0;
    ( *hostApi )->info.defaultOutputDevice = 0;
    ( *hostApi )->deviceInfos = sndioHostApi->deviceInfoPtrs;
    ( *hostApi )->Terminate = Terminate;
    ( *hostApi )->OpenStream = OpenStream;
    ( *hostApi )->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface( &sndioHostApi->blocking, CloseStream, StartStream, StopStream, AbortStream,
                                      IsStreamStopped, IsStreamActive, GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      BlockingReadStream, BlockingWriteStream, BlockingGetStreamReadAvailable,
                                      BlockingGetStreamWriteAvailable );

    PaUtil_InitializeStreamInterface( &sndioHostApi->callback, CloseStream, StartStream, StopStream, AbortStream,
                                      IsStreamStopped, IsStreamActive, GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      PaUtil_DummyRead, PaUtil_DummyWrite, PaUtil_DummyGetReadAvailable,
                                      PaUtil_DummyGetWriteAvailable );

    PA_DEBUG( ( "PaSndio_Initialize: done\n" ) );
    return paNoError;
}
