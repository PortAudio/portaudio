#ifndef PA_OPENSLES_H
#define PA_OPENSLES_H

/*
 * $Id:
 * PortAudio Portable Real-Time Audio Library
 * Android OpenSLES-specific extensions
 *
 * Copyright (c) 1999-2000 Ross Bencina and Phil Burk
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
 @ingroup public_header
 @brief Android OpenSLES-specific PortAudio API extension header file.
*/

#include <SLES/OpenSLES.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The android stream type and recording preset as defined in
 * OpenSLES_AndroidConfiguration.h
 */
typedef struct PaOpenslesStreamInfo {
    SLint32 androidPlaybackStreamType;
    SLint32 androidRecordingPreset;
} PaOpenslesStreamInfo;

/** Provide PA OpenSLES with native buffer information. This function must be called before Pa_Initialize.
 * To have optimal latency, this function should be called. Otherwise PA OpenSLES will use non-optimal values
 * as default.
 * @param bufferSize the native buffersize as returned by AudioManager's PROPERTY_OUTPUT_FRAMES_PER_BUFFER. It is recommended you set the number of buffers to 1 if API>17 as well, and use the sample rate defined in AudioManager's android.media.property.OUTPUT_SAMPLE_RATE. All three together will enable the AUDIO_OUTPUT_FLAG_FAST flag.
 */
void PaOpenSLES_SetNativeBufferSize( unsigned long bufferSize );

/** Provide PA OpenSLES with native buffer information. This function must be called before Pa_Initialize.
 * To have optimal latency and enable the AUDIO_OUTPUT_FLAG_FAST flag, this function should be called. Otherwise PA OpenSLES will use non-optimal values (2) as default.
 * @param The number of buffers can be reduced to 1 on API >17. Make sure you set the native buffer size when doing this, and use the sample rate defined in AudioManager's android.media.property.OUTPUT_SAMPLE_RATE.
 */
void PaOpenSLES_SetNumberOfBuffers( unsigned buffers );

#ifdef __cplusplus
}
#endif

#endif
