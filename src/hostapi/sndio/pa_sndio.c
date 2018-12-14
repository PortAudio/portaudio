/*
 * Copyright (c) 2009 Alexandre Ratchov <alex@caoua.org>
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
#include <sys/types.h>
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sndio.h>

#include "pa_util.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_process.h"
#include "pa_allocation.h"

#if 0
#define DPR(...) do { fprintf(stderr, __VA_ARGS__); } while (0)
#else
#define DPR(...) do {} while (0)
#endif

/*
 * per-stream data
 */
typedef struct PaSndioStream
{
	PaUtilStreamRepresentation base;
	PaUtilBufferProcessor bufproc;	/* format conversion */
	struct sio_hdl *hdl;		/* handle for device i/o */
	struct sio_par par;		/* current device parameters */	
	unsigned mode;			/* SIO_PLAY, SIO_REC or both */
	int stopped;			/* stop requested or not started */
	int active;			/* thread is running */
	unsigned long long realpos;	/* frame number h/w is processing */
	char *rbuf, *wbuf;		/* bounce buffers for conversions */
	unsigned long long rpos, wpos;	/* bytes read/written */
	pthread_t thread;		/* thread of the callback interface */
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
	 * sndio has no device discovery mechanism, so expose only
	 * the default device, the user will have a chance to change it
	 * using the environment variable
	 */
	PaDeviceInfo *infos[1], default_info;
} PaSndioHostApiRepresentation;

/*
 * callback invoked when blocks are processed by the hardware
 */
static void
sndioOnMove(void *addr, int delta)
{
	PaSndioStream *s = (PaSndioStream *)addr;

	s->realpos += delta;
}

/*
 * convert PA encoding to sndio encoding, return true on success
 */
static int
sndioSetFmt(struct sio_par *sio, PaSampleFormat fmt)
{
	switch (fmt & ~paNonInterleaved) {
	case paInt32:
		sio->sig = 1;
		sio->bits = 32;
		break;
	case paInt24:
		sio->sig = 1;
		sio->bits = 24;
		sio->bps = 3;	/* paInt24 is packed format */
		break;
	case paInt16:
	case paFloat32:
		sio->sig = 1;
		sio->bits = 16;
		break;
	case paInt8:
		sio->sig = 1;
		sio->bits = 8;
		break;
	case paUInt8:
		sio->sig = 0;
		sio->bits = 8;
		break;
	default:
		DPR("sndioSetFmt: %x: unsupported\n", fmt);
		return 0;
	}
	sio->le = SIO_LE_NATIVE;
	return 1;
}

/*
 * convert sndio encoding to PA encoding, return true on success
 */
static int
sndioGetFmt(struct sio_par *sio, PaSampleFormat *fmt)
{
	if ((sio->bps * 8 != sio->bits && !sio->msb) ||
	    (sio->bps > 1 && sio->le != SIO_LE_NATIVE)) {
		DPR("sndioGetFmt: bits = %u, le = %u, msb = %u, bps = %u\n",
		    sio->bits, sio->le, sio->msb, sio->bps);
		return 0;
	}

	switch (sio->bits) {
	case 32:
		if (!sio->sig)
			return 0;
		*fmt = paInt32;
		break;
	case 24:
		if (!sio->sig)
			return 0;
		*fmt = (sio->bps == 3) ? paInt24 : paInt32;
		break;
	case 16:
		if (!sio->sig)
			return 0;
		*fmt = paInt16;
		break;
	case 8:
		*fmt = sio->sig ? paInt8 : paUInt8;
		break;
	default:
		DPR("sndioGetFmt: %u: unsupported\n", sio->bits);
		return 0;
	}
	return 1;
}

/*
 * I/O loop for callback interface
 */
static void *
sndioThread(void *arg)
{
	PaSndioStream *s = (PaSndioStream *)arg;
	PaStreamCallbackTimeInfo ti;
	unsigned char *data;
	unsigned todo, rblksz, wblksz;
	int n, result;
	
	rblksz = s->par.round * s->par.rchan * s->par.bps;
	wblksz = s->par.round * s->par.pchan * s->par.bps;
	
	DPR("sndioThread: mode = %x, round = %u, rblksz = %u, wblksz = %u\n",
	    s->mode, s->par.round, rblksz, wblksz);
	
	while (!s->stopped) {
		if (s->mode & SIO_REC) {
			todo = rblksz;
			data = s->rbuf;
			while (todo > 0) {
				n = sio_read(s->hdl, data, todo);
				if (n == 0) {
					DPR("sndioThread: sio_read failed\n");
					goto failed;
				}
				todo -= n;
				data += n;
			}
			s->rpos += s->par.round;
			ti.inputBufferAdcTime = 
			    (double)s->realpos / s->par.rate;
		}
		if (s->mode & SIO_PLAY) {
			ti.outputBufferDacTime =
			    (double)(s->realpos + s->par.bufsz) / s->par.rate;
		}
		ti.currentTime = s->realpos / (double)s->par.rate;
		PaUtil_BeginBufferProcessing(&s->bufproc, &ti, 0);
		if (s->mode & SIO_PLAY) {
			PaUtil_SetOutputFrameCount(&s->bufproc, s->par.round);
			PaUtil_SetInterleavedOutputChannels(&s->bufproc,
			    0, s->wbuf, s->par.pchan);
		}
		if (s->mode & SIO_REC) {
			PaUtil_SetInputFrameCount(&s->bufproc, s->par.round);
			PaUtil_SetInterleavedInputChannels(&s->bufproc,
			    0, s->rbuf, s->par.rchan);
		}
		result = paContinue;
		n = PaUtil_EndBufferProcessing(&s->bufproc, &result);
		if (n != s->par.round) {
			DPR("sndioThread: %d < %u frames, result = %d\n",
			    n, s->par.round, result);
		}
		if (result != paContinue) {
			break;
		}
		if (s->mode & SIO_PLAY) {
			n = sio_write(s->hdl, s->wbuf, wblksz);
			if (n < wblksz) {
				DPR("sndioThread: sio_write failed\n");
				goto failed;
			}
			s->wpos += s->par.round;
		}
	}
 failed:
	s->active = 0;
	DPR("sndioThread: done\n");
}

static PaError
OpenStream(struct PaUtilHostApiRepresentation *hostApi,
    PaStream **stream,
    const PaStreamParameters *inputPar,
    const PaStreamParameters *outputPar,
    double sampleRate,
    unsigned long framesPerBuffer,
    PaStreamFlags streamFlags,
    PaStreamCallback *streamCallback,
    void *userData)
{
	PaSndioHostApiRepresentation *sndioHostApi = (PaSndioHostApiRepresentation *)hostApi;
	PaSndioStream *s;
	PaError err;
	struct sio_hdl *hdl;
	struct sio_par par;
	unsigned mode;
	int inch, onch;
	PaSampleFormat ifmt, ofmt, siofmt;

	DPR("OpenStream:\n");

	mode = 0;
	inch = onch = 0;
	ifmt = ofmt = 0;
	sio_initpar(&par);

	if (outputPar && outputPar->channelCount > 0) {
		if (outputPar->device != 0) {
			DPR("OpenStream: %d: bad output device\n", outputPar->device);
			return paInvalidDevice;
		}
		if (outputPar->hostApiSpecificStreamInfo) {
			DPR("OpenStream: output specific info\n");
			return paIncompatibleHostApiSpecificStreamInfo;
		}
		if (!sndioSetFmt(&par, outputPar->sampleFormat)) {
			return paSampleFormatNotSupported;
		}
		ofmt = outputPar->sampleFormat;
		onch = par.pchan = outputPar->channelCount;
		mode |= SIO_PLAY;
	}
	if (inputPar && inputPar->channelCount > 0) {
		if (inputPar->device != 0) {
			DPR("OpenStream: %d: bad input device\n", inputPar->device);
			return paInvalidDevice;
		}
		if (inputPar->hostApiSpecificStreamInfo) {
			DPR("OpenStream: input specific info\n");
			return paIncompatibleHostApiSpecificStreamInfo;
		}
		if (!sndioSetFmt(&par, inputPar->sampleFormat)) {
			return paSampleFormatNotSupported;
		}
		ifmt = inputPar->sampleFormat;
		inch = par.rchan = inputPar->channelCount;
		mode |= SIO_REC;
	}
	par.rate = sampleRate;
	if (framesPerBuffer != paFramesPerBufferUnspecified)
		par.round = framesPerBuffer;

	DPR("OpenStream: mode = %x, trying rate = %u\n", mode, par.rate);

	hdl = sio_open(SIO_DEVANY, mode, 0);
	if (hdl == NULL)
		return paUnanticipatedHostError;
	if (!sio_setpar(hdl, &par)) {
		sio_close(hdl);
		return paUnanticipatedHostError;
	}
	if (!sio_getpar(hdl, &par)) {
		sio_close(hdl);
		return paUnanticipatedHostError;
	}
	if (!sndioGetFmt(&par, &siofmt)) {
		sio_close(hdl);
		return paSampleFormatNotSupported;
	}
	if ((mode & SIO_REC) && par.rchan != inputPar->channelCount) {
		DPR("OpenStream: rchan(%u) != %d\n", par.rchan, inputPar->channelCount);
		sio_close(hdl);
		return paInvalidChannelCount;
	}
	if ((mode & SIO_PLAY) && par.pchan != outputPar->channelCount) {
		DPR("OpenStream: pchan(%u) != %d\n", par.pchan, outputPar->channelCount);
		sio_close(hdl);
		return paInvalidChannelCount;
	}
	if ((double)par.rate < sampleRate * 0.995 ||
	    (double)par.rate > sampleRate * 1.005) {
		DPR("OpenStream: rate(%u) != %g\n", par.rate, sampleRate);
		sio_close(hdl);
		return paInvalidSampleRate;
	}
	
	s = (PaSndioStream *)PaUtil_AllocateMemory(sizeof(PaSndioStream));
	if (s == NULL) {
		sio_close(hdl);
		return paInsufficientMemory;
	}
	PaUtil_InitializeStreamRepresentation(&s->base, 
	    streamCallback ? &sndioHostApi->callback : &sndioHostApi->blocking,
	    streamCallback, userData);
	DPR("inch = %d, onch = %d, ifmt = %x, ofmt = %x\n", 
	    inch, onch, ifmt, ofmt);
	err = PaUtil_InitializeBufferProcessor(&s->bufproc,
	    inch, ifmt, siofmt,
	    onch, ofmt, siofmt,
	    sampleRate,
	    streamFlags,
	    framesPerBuffer,
	    par.round,
	    paUtilFixedHostBufferSize, 
	    streamCallback, userData);
	if (err) {
		DPR("OpenStream: PaUtil_InitializeBufferProcessor failed\n");
		PaUtil_FreeMemory(s);
		sio_close(hdl);
		return err;
	}
	if (mode & SIO_REC) {
		s->rbuf = malloc(par.round * par.rchan * par.bps);
		if (s->rbuf == NULL) {
			DPR("OpenStream: failed to allocate rbuf\n");
			PaUtil_FreeMemory(s);
			sio_close(hdl);
			return paInsufficientMemory;
		}
	}
	if (mode & SIO_PLAY) {
		s->wbuf = malloc(par.round * par.pchan * par.bps);
		if (s->wbuf == NULL) {
			DPR("OpenStream: failed to allocate wbuf\n");
			free(s->rbuf);
			PaUtil_FreeMemory(s);
			sio_close(hdl);
			return paInsufficientMemory;
		}
	}	
	s->base.streamInfo.inputLatency = 0;
	s->base.streamInfo.outputLatency = (mode & SIO_PLAY) ?
	    (double)(par.bufsz + PaUtil_GetBufferProcessorOutputLatencyFrames(&s->bufproc)) / (double)par.rate : 0;
	s->base.streamInfo.sampleRate = par.rate;
	s->active = 0;
	s->stopped = 1;
	s->mode = mode;
	s->hdl = hdl;
	s->par = par;
	*stream = s;	
	DPR("OpenStream: done\n");
	return paNoError;
}

static PaError
BlockingReadStream(PaStream *stream, void *data, unsigned long numFrames)
{
	PaSndioStream *s = (PaSndioStream *)stream;
	unsigned n, res, todo;
	void *buf;
	
	while (numFrames > 0) {
		n = s->par.round;
		if (n > numFrames)
			n = numFrames;
		buf = s->rbuf;
		todo = n * s->par.rchan * s->par.bps;
		while (todo > 0) {
			res = sio_read(s->hdl, buf, todo);
			if (res == 0)
				return paUnanticipatedHostError;
			buf = (char *)buf + res;
			todo -= res;
		}
		s->rpos += n;
		PaUtil_SetInputFrameCount(&s->bufproc, n);
		PaUtil_SetInterleavedInputChannels(&s->bufproc, 0, s->rbuf, s->par.rchan);
		res = PaUtil_CopyInput(&s->bufproc, &data, n);
		if (res != n) {
			DPR("BlockingReadStream: copyInput: %u != %u\n");
			return paUnanticipatedHostError;
		}
		numFrames -= n;
	}
	return paNoError;
}

static PaError
BlockingWriteStream(PaStream* stream, const void *data, unsigned long numFrames)
{
	PaSndioStream *s = (PaSndioStream *)stream;
	unsigned n, res;

	while (numFrames > 0) {
		n = s->par.round;
		if (n > numFrames)
			n = numFrames;
		PaUtil_SetOutputFrameCount(&s->bufproc, n);
		PaUtil_SetInterleavedOutputChannels(&s->bufproc, 0, s->wbuf, s->par.pchan);
		res = PaUtil_CopyOutput(&s->bufproc, &data, n);
		if (res != n) {
			DPR("BlockingWriteStream: copyOutput: %u != %u\n");
			return paUnanticipatedHostError;
		}
		res = sio_write(s->hdl, s->wbuf, n * s->par.pchan * s->par.bps);
		if (res == 0)
			return paUnanticipatedHostError;		
		s->wpos += n;
		numFrames -= n;
	}
	return paNoError;
}

static signed long
BlockingGetStreamReadAvailable(PaStream *stream)
{
	PaSndioStream *s = (PaSndioStream *)stream;
	struct pollfd pfd;
	int n, events;

	n = sio_pollfd(s->hdl, &pfd, POLLIN);
	while (poll(&pfd, n, 0) < 0) {
		if (errno == EINTR)
			continue;
		perror("poll");
		abort();
	}
	events = sio_revents(s->hdl, &pfd);
	if (!(events & POLLIN))
		return 0;

	return s->realpos - s->rpos;
}

static signed long
BlockingGetStreamWriteAvailable(PaStream *stream)
{
	PaSndioStream *s = (PaSndioStream *)stream;
	struct pollfd pfd;
	int n, events;

	n = sio_pollfd(s->hdl, &pfd, POLLOUT);
	while (poll(&pfd, n, 0) < 0) {
		if (errno == EINTR)
			continue;
		perror("poll");
		abort();
	}
	events = sio_revents(s->hdl, &pfd);
	if (!(events & POLLOUT))
		return 0;

	return s->par.bufsz - (s->wpos - s->realpos);
}

static PaError
BlockingWaitEmpty( PaStream *stream )
{
	PaSndioStream *s = (PaSndioStream *)stream;

	/*
	 * drain playback buffers; sndio always does it in background
	 * and there is no way to wait for completion
	 */
	DPR("BlockingWaitEmpty: s=%d, a=%d\n", s->stopped, s->active);

	return paNoError;
}

static PaError
StartStream(PaStream *stream)
{
	PaSndioStream *s = (PaSndioStream *)stream;
	unsigned primes, wblksz;
	int err;

	DPR("StartStream: s=%d, a=%d\n", s->stopped, s->active);

	if (!s->stopped) {
		DPR("StartStream: already started\n");
		return paNoError;
	}
	s->stopped = 0;
	s->active = 1;
	s->realpos = 0;
	s->wpos = 0;
	s->rpos = 0;
	PaUtil_ResetBufferProcessor(&s->bufproc);
	if (!sio_start(s->hdl))
		return paUnanticipatedHostError;

	/*
	 * send a complete buffer of silence
	 */
	if (s->mode & SIO_PLAY) {
		wblksz = s->par.round * s->par.pchan * s->par.bps;
		memset(s->wbuf, 0, wblksz);
		for (primes = s->par.bufsz / s->par.round; primes > 0; primes--)
			s->wpos += sio_write(s->hdl, s->wbuf, wblksz);
	}
	if (s->base.streamCallback) {
		err = pthread_create(&s->thread, NULL, sndioThread, s);
		if (err) {
			DPR("SndioStartStream: couldn't create thread\n");
			return paUnanticipatedHostError;
		}
		DPR("StartStream: started...\n");
	}
	return paNoError;
}

static PaError
StopStream(PaStream *stream)
{
	PaSndioStream *s = (PaSndioStream *)stream;
	void *ret;
	int err;

	DPR("StopStream: s=%d, a=%d\n", s->stopped, s->active);

	if (s->stopped) {
		DPR("StartStream: already started\n");
		return paNoError;
	}
	s->stopped = 1;
	if (s->base.streamCallback) {
		err = pthread_join(s->thread, &ret);
		if (err) {
			DPR("SndioStop: couldn't join thread\n");
			return paUnanticipatedHostError;
		}
	}
	if (!sio_stop(s->hdl))
		return paUnanticipatedHostError;
	return paNoError;
}

static PaError
CloseStream(PaStream *stream)
{
	PaSndioStream *s = (PaSndioStream *)stream;

	DPR("CloseStream:\n");

	if (!s->stopped)
		StopStream(stream);

	if (s->mode & SIO_REC)
		free(s->rbuf);
	if (s->mode & SIO_PLAY)
		free(s->wbuf);
	sio_close(s->hdl);
        PaUtil_TerminateStreamRepresentation(&s->base);
	PaUtil_TerminateBufferProcessor(&s->bufproc);
	PaUtil_FreeMemory(s);
	return paNoError;
}

static PaError
AbortStream(PaStream *stream)
{
	DPR("AbortStream:\n");

	return StopStream(stream);
}

static PaError
IsStreamStopped(PaStream *stream)
{
	PaSndioStream *s = (PaSndioStream *)stream;

	//DPR("IsStreamStopped: s=%d, a=%d\n", s->stopped, s->active);

	return s->stopped;
}

static PaError
IsStreamActive(PaStream *stream)
{
	PaSndioStream *s = (PaSndioStream *)stream;

	//DPR("IsStreamActive: s=%d, a=%d\n", s->stopped, s->active);

	return s->active;
}

static PaTime
GetStreamTime(PaStream *stream)
{
	PaSndioStream *s = (PaSndioStream *)stream;

	return (double)s->realpos / s->base.streamInfo.sampleRate;
}

static PaError
IsFormatSupported(struct PaUtilHostApiRepresentation *hostApi,
    const PaStreamParameters *inputPar,
    const PaStreamParameters *outputPar,
    double sampleRate)
{
	return paFormatIsSupported;
}

static void
Terminate(struct PaUtilHostApiRepresentation *hostApi)
{
	PaUtil_FreeMemory(hostApi);
}

PaError
PaSndio_Initialize(PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex)
{
	PaSndioHostApiRepresentation *sndioHostApi;
	PaDeviceInfo *info;
	struct sio_hdl *hdl;
	
	DPR("PaSndio_Initialize: initializing...\n");

	/* unusable APIs should return paNoError and a NULL hostApi */
	*hostApi = NULL;

	sndioHostApi = PaUtil_AllocateMemory(sizeof(PaSndioHostApiRepresentation));
	if (sndioHostApi == NULL)
		return paNoError;

	info = &sndioHostApi->default_info;
	info->structVersion = 2;
	info->name = "default";
	info->hostApi = hostApiIndex;
	info->maxInputChannels = 128;
	info->maxOutputChannels = 128;
	info->defaultLowInputLatency = 0.01;
	info->defaultLowOutputLatency = 0.01;
	info->defaultHighInputLatency = 0.5;
	info->defaultHighOutputLatency = 0.5;
	info->defaultSampleRate = 48000;
	sndioHostApi->infos[0] = info;
	
	*hostApi = &sndioHostApi->base;
	(*hostApi)->info.structVersion = 1;
	(*hostApi)->info.type = paSndio;
	(*hostApi)->info.name = "sndio";
	(*hostApi)->info.deviceCount = 1;
	(*hostApi)->info.defaultInputDevice = 0;
	(*hostApi)->info.defaultOutputDevice = 0;
	(*hostApi)->deviceInfos = sndioHostApi->infos;
	(*hostApi)->Terminate = Terminate;
	(*hostApi)->OpenStream = OpenStream;
	(*hostApi)->IsFormatSupported = IsFormatSupported;
	
	PaUtil_InitializeStreamInterface(&sndioHostApi->blocking,
	    CloseStream,
	    StartStream,
	    StopStream,
	    AbortStream,
	    IsStreamStopped,
	    IsStreamActive,
	    GetStreamTime,
	    PaUtil_DummyGetCpuLoad,
	    BlockingReadStream,
	    BlockingWriteStream,
	    BlockingGetStreamReadAvailable,
	    BlockingGetStreamWriteAvailable);

	PaUtil_InitializeStreamInterface(&sndioHostApi->callback,
	    CloseStream,
	    StartStream,
	    StopStream,
	    AbortStream,
	    IsStreamStopped,
	    IsStreamActive,
	    GetStreamTime,
	    PaUtil_DummyGetCpuLoad,
	    PaUtil_DummyRead,
	    PaUtil_DummyWrite,
	    PaUtil_DummyGetReadAvailable,
	    PaUtil_DummyGetWriteAvailable);

	DPR("PaSndio_Initialize: done\n");
	return paNoError;
}
