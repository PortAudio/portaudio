/*
 * $Id:
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 *
 * Android Oboe implementation of PortAudio.
 *
 ****************************************************************************************
 *      Author:                                                                         *
 *              Carlo Benfatti          <benfatti@netresults.it>                        *
 ****************************************************************************************
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

#ifndef PA_OBOE_H
#define PA_OBOE_H

/**
 * @file
 * @ingroup public_header
 * @brief Android Oboe-specific PortAudio API extension header file.
 */

#include "portaudio.h"

#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

#define TIMEOUT_NS 1000000000 //Arbitrary timeout of the read/write functions
#define LOW_LATENCY_MS 300.0 //Arbitrary value used to automatically determine if low latency performance mode is doable

/**
 * Enum class that emulates Oboe::Direction.
 */
enum class PaOboe_Direction: int32_t { Output = 0, Input = 1 };

/**
 * Enum class that emulates Oboe::Usage.
 */
enum class PaOboe_Usage : int32_t {
    Media = 1 , VoiceCommunication = 2 , VoiceCommunicationSignalling = 3 , Alarm = 4 ,
    Notification = 5 , NotificationRingtone = 6 , NotificationEvent = 10 , AssistanceAccessibility = 11 ,
    AssistanceNavigationGuidance = 12 , AssistanceSonification = 13 , Game = 14 , Assistant = 16
};

/**
 * Enum class that emulates Oboe::InputPreset
 */
enum class PaOboe_InputPreset : int32_t {
    Generic = 1 , Camcorder = 5 , VoiceRecognition = 6 , VoiceCommunication = 7 ,
    Unprocessed = 9 , VoicePerformance = 10
};

/**
 * Enum class that emulates Oboe::PerformanceMode
 */
enum class PaOboe_PerformanceMode : int32_t { None = 10 , PowerSaving = 11 , LowLatency = 12 };

/**
 *  The android stream type and recording preset as defined in Oboe.
 */
typedef struct PaOboeStreamInfo {
    PaOboe_Usage androidOutputUsage;
    PaOboe_InputPreset androidInputPreset;
} PaOboeStreamInfo;


/**
 * Provide PaOboe with the ID of the device the user chose - oboe cannot build a device list,
 * but can select the device if provided with its ID.
 * @param direction - the direction of the stream for which we want to set the device.
 * @param deviceID - the ID of the chosen device chosen by the user.
 */
void PaOboe_SetSelectedDevice(PaOboe_Direction direction, int32_t deviceID);


/**
 * \brief   Provide PaOboe with the performance mode chosen by the user. If this method isn't called, the default mode
 *          is LowLatency.
 * @param   direction - the direction of the stream for which we want to set the performance mode.
 * @param   performanceMode - the performance mode chosen by the user.
 */
void PaOboe_SetPerformanceMode(PaOboe_Direction direction, PaOboe_PerformanceMode performanceMode);


/**
 * Provide PA Oboe with native buffer information. If you call this function, you must do so before
 * calling Pa_Initialize. To have optimal latency, this function should be called - otherwise,
 * PA Oboe will use potentially non-optimal values as default.
 * @param bufferSize the native buffersize as returned by AudioManager's
 * PROPERTY_OUTPUT_FRAMES_PER_BUFFER. It is recommended you set the number of buffers to 1 if API>17
 * as well, and use the sample rate defined in AudioManager's android.media.property.OUTPUT_SAMPLE_RATE.
 * All three together will enable the AUDIO_OUTPUT_FLAG_FAST flag.
 */
void PaOboe_SetNativeBufferSize(unsigned long bufferSize);

/**
 * Provide PA Oboe with native buffer information. If you call this function, you must do so before
 * calling Pa_Initialize. To have optimal latency and enable the AUDIO_OUTPUT_FLAG_FAST flag, this
 * function should be called - otherwise, PA Oboe will use potentially non-optimal values (2) as default.
 * @param buffers The number of buffers can be reduced to 1 on API >17. Make sure you set the native
 * buffer size when doing this, and use the sample rate defined in AudioManager's
 * android.media.property.OUTPUT_SAMPLE_RATE.
 */
void PaOboe_SetNumberOfBuffers(unsigned numberOfBuffers);

#ifdef __cplusplus
}
#endif //__cplusplus

#endif //PA_OBOE_H
