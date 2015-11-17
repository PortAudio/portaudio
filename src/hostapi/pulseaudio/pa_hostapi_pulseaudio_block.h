#ifndef _PA_HOSTAPI_PULSEAUDIO_BLOCK_H_
#define _PA_HOSTAPI_PULSEAUDIO_BLOCK_H_

#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

#include "pa_unix_util.h"
#include "pa_ringbuffer.h"

/* PulseAudio headers */
#include <stdio.h>
#include <string.h>
#include <pulse/pulseaudio.h>

#include "pa_hostapi_pulseaudio.h"

#ifdef __cplusplus
extern "C"
{
#endif                          /* __cplusplus */

    PaError PulseAudioCloseStreamBlock(
    PaStream * stream
    );
    PaError PulseAudioStartStreamBlock(
    PaStream * stream
    );
    PaError PulseAudioStopStreamBlock(
    PaStream * stream
    );
    PaError PulseAudioAbortStreamBlock(
    PaStream * stream
    );
    PaError PulseAudioReadStreamBlock(
    PaStream * stream,
    void *buffer,
    unsigned long frames
    );
    PaError PulseAudioWriteStreamBlock(
    PaStream * stream,
    const void *buffer,
    unsigned long frames
    );
    signed long PulseAudioGetStreamReadAvailableBlock(
    PaStream * stream
    );
    signed long PulseAudioGetStreamWriteAvailableBlock(
    PaStream * stream
    );

#ifdef __cplusplus
}
#endif                          /* __cplusplus */


#endif
