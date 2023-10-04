/*
 * $Id$
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
 @ingroup hostapi_src
 @brief Oboe implementation of support for a host API.
*/
#include "pa_allocation.h"
#include "pa_cpuload.h"
#include "pa_debugprint.h"
#include "pa_hostapi.h"
#include "pa_process.h"
#include "pa_stream.h"
#include "pa_unix_util.h"
#include "pa_util.h"

#include <pthread.h>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <cstring>
#include <memory>
#include <cstdint>
#include <vector>
#include "oboe/Oboe.h"

#include <android/log.h>
#include <android/api-level.h>

#include "pa_oboe.h"

#define MODULE_NAME "PaOboe"

#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, MODULE_NAME, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, MODULE_NAME, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, MODULE_NAME, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,MODULE_NAME, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR,MODULE_NAME, __VA_ARGS__)
#define LOGF(...) __android_log_print(ANDROID_LOG_FATAL,MODULE_NAME, __VA_ARGS__)

// Copied from @{pa_opensles.c}.
#define ENSURE(expr, errorText)                                             \
    do                                                                      \
    {                                                                       \
        PaError m_err;                                                      \
        if (UNLIKELY((m_err = (expr)) < paNoError))                         \
        {                                                                   \
            PaUtil_DebugPrint(("Expression '" #expr "' failed in '" __FILE__ "', line: " PA_STRINGIZE( \
                                                                __LINE__ ) "\n")); \
            PaUtil_SetLastHostErrorInfo(paInDevelopment, m_err, errorText); \
            m_error = m_err;                                               \
            goto error;                                                     \
        }                                                                   \
    } while (0);

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

PaError PaOboe_Initialize(PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex);

#ifdef __cplusplus
}
#endif /* __cplusplus */

static void Terminate(struct PaUtilHostApiRepresentation *hostApi);

static PaError IsFormatSupported(struct PaUtilHostApiRepresentation *hostApi,
                                 const PaStreamParameters *inputParameters,
                                 const PaStreamParameters *outputParameters,
                                 double sampleRate);

static PaError OpenStream(struct PaUtilHostApiRepresentation *hostApi,
                          PaStream **s,
                          const PaStreamParameters *inputParameters,
                          const PaStreamParameters *outputParameters,
                          double sampleRate,
                          unsigned long framesPerBuffer,
                          PaStreamFlags streamFlags,
                          PaStreamCallback *streamCallback,
                          void *userData);

static PaError CloseStream(PaStream *stream);

static PaError StartStream(PaStream *stream);

static PaError StopStream(PaStream *stream);

static PaError AbortStream(PaStream *stream);

static PaError IsStreamStopped(PaStream *s);

static PaError IsStreamActive(PaStream *stream);

static PaTime GetStreamTime(PaStream *stream);

static double GetStreamCpuLoad(PaStream *stream);

static PaError ReadStream(PaStream *stream, void *buffer, unsigned long frames);

static PaError WriteStream(PaStream *stream, const void *buffer, unsigned long frames);

static void StreamProcessingCallback(void *userData);

static signed long GetStreamReadAvailable(PaStream *stream);

static signed long GetStreamWriteAvailable(PaStream *stream);

static unsigned long GetApproximateLowBufferSize();

// Commonly used parameters initialized.
static unsigned long g_nativeBufferSize = 0;
static unsigned g_numberOfBuffers = 2;

using namespace oboe;

//Useful global variables
int32_t g_inputDeviceId = kUnspecified;
int32_t g_outputDeviceId = kUnspecified;

PerformanceMode g_inputPerfMode = PerformanceMode::LowLatency;
PerformanceMode g_outputPerfMode = PerformanceMode::LowLatency;

class OboeEngine;
class OboeCallback;

/**
 * Stream structure, useful to store relevant information. It's needed by Portaudio.
 */
typedef struct OboeStream {
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

    bool isBlocking;
    bool isStopped;
    bool isActive;
    bool doStop;
    bool doAbort;
    bool hasOutput;
    bool hasInput;

    int callbackResult;
    DataCallbackResult oboeCallbackResult;
    PaStreamCallbackFlags cbFlags;

    PaSampleFormat inputFormat;
    PaSampleFormat outputFormat;

    OboeCallback *oboeCallback;
    // Buffers are managed by the callback function in Oboe.
    void **outputBuffers;
    int currentOutputBuffer;
    void **inputBuffers;
    int currentInputBuffer;

    unsigned long framesPerHostCallback;
    unsigned bytesPerFrame;

    OboeEngine *getEngineAddress() { return oboeEngineAddress; }

    void setEngineAddress(OboeEngine *i_oboeEngine) { oboeEngineAddress = i_oboeEngine; }

    //The only instances of output and input streams that will be used, and their builders
    std::shared_ptr <AudioStream> outputStream;
    AudioStreamBuilder outputBuilder;
    std::shared_ptr <AudioStream> inputStream;
    AudioStreamBuilder inputBuilder;

private:
    OboeEngine *oboeEngineAddress;
} OboeStream;

/**
 * Callback class for OboeStream. Will be used for non-blocking streams.
 */
class OboeCallback: public AudioStreamCallback {
public:
    OboeCallback(){ m_oboeStreamHolder = (OboeStream *) PaUtil_AllocateZeroInitializedMemory(sizeof(OboeStream)); }
    //Callback function for non-blocking streams
    DataCallbackResult onAudioReady(AudioStream *audioStream, void *audioData,
                                    int32_t numFrames) override;

    void onErrorAfterClose(AudioStream *audioStream, oboe::Result error) override;

    void setStreamHolder(OboeStream* oboeStream){ m_oboeStreamHolder = oboeStream; }

    void resetCallbackCounters();

private:
    //callback utils
    OboeStream *m_oboeStreamHolder;
    unsigned long m_framesProcessed{};
    PaStreamCallbackTimeInfo m_timeInfo{};
    struct timespec m_timeSpec{};
};


/**
 * Stream engine of the host API - Oboe. We allocate only one instance of the engine per OboeStream, and
 * we call its functions when we want to operate directly on Oboe. More infos on each function are
 * provided right before its implementation.
 */
class OboeEngine {
public:
    OboeEngine();

    //Stream-managing functions
    bool tryStream(Direction direction, int32_t sampleRate, int32_t channelCount);

    PaError openStream(OboeStream *oboeStream, Direction direction, int32_t sampleRate,
                       Usage outputUsage, InputPreset inputPreset);

    bool startStream(OboeStream *oboeStream);

    bool stopStream(OboeStream *oboeStream);

    bool restartStream(OboeStream *oboeStream, int direction);

    bool closeStream(OboeStream *oboeStream);

    bool abortStream(OboeStream *oboeStream);

    //Blocking read/write functions
    bool writeStream(OboeStream *oboeStream, const void *buffer, int32_t framesToWrite);

    bool readStream(OboeStream *oboeStream, void *buffer, int32_t framesToRead);

    //Engine utils
    OboeStream *allocateOboeStream();

private:
    std::shared_ptr <AudioStream> m_testStream;
    AudioStreamBuilder m_testBuilder;

    //Conversion utils
    static AudioFormat PaToOboeFormat(PaSampleFormat paFormat);

    //device selection implementation
    int32_t getSelectedDevice(oboe::Direction direction);
};


/**
 * Structure used by Portaudio to interface with the HostApi - in this case, Oboe.
 */
typedef struct PaOboeHostApiRepresentation {
    PaUtilHostApiRepresentation inheritedHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilStreamInterface blockingStreamInterface;

    PaUtilAllocationGroup *allocations;

    OboeEngine *oboeEngine;
} PaOboeHostApiRepresentation;


/*----------------------------- OboeEngine functions implementation -----------------------------*/


/**
 * \brief   Constructor.
 */
OboeEngine::OboeEngine() {}


/**
 * \brief   Tries to open a stream with the direction i_direction, sample rate i_sampleRate and/or
 *          channel count i_channelCount. It then checks if the stream was in fact opened with the
 *          desired settings, and then closes the stream. It's used to see if the requested
 *          parameters are supported by the devices that are going to be used.
 * @param   direction the Direction of the stream;
 * @param   sampleRate the sample rate we want to try;
 * @param   channelCount the channel count we want to try;
 * @return  true if the requested sample rate / channel count is supported by the device, false if
 *          they aren't, or if tryStream couldn't open a stream.
 */
bool OboeEngine::tryStream(Direction i_direction, int32_t i_sampleRate, int32_t i_channelCount) {
    Result result;
    bool outcome = false;

    m_testBuilder.setDeviceId(getSelectedDevice(i_direction))
            // Arbitrary format usually broadly supported. Later, we'll open streams with correct formats.
            ->setFormat(AudioFormat::Float)
            ->setDirection(i_direction)
            ->setSampleRate(i_sampleRate)
            ->setChannelCount(i_channelCount)
            ->openStream(m_testStream);

    if (result != Result::OK) {
        LOGE("[OboeEngine::TryStream]\t Couldn't open the stream in TryStream. Error: %s",
             convertToText(result));
        return outcome;
    }

    if (i_sampleRate != kUnspecified) {
        outcome = (i_sampleRate == m_testBuilder.getSampleRate());
        if (!outcome) {
            LOGW("[OboeEngine::TryStream]\t Tried sampleRate = %d, built sampleRate = %d",
                 i_sampleRate, m_testBuilder.getSampleRate());
        }
    } else if (i_channelCount != kUnspecified) {
        outcome = (i_channelCount == m_testBuilder.getChannelCount());
        if (!outcome) {
            LOGW("[OboeEngine::TryStream]\t Tried channelCount = %d, built channelCount = %d",
                 i_channelCount, m_testBuilder.getChannelCount());
        }
    } else {
        LOGE("[OboeEngine::TryStream]\t Logical failure. This message should NEVER occur.");
        outcome = false;
    }

    m_testStream->close();

    return outcome;
}


/**
 * \brief   Opens an audio stream with a specific direction, sample rate and,
 *          depending on the direction of the stream, sets its usage (if
 *          direction == Ditrction::Output) or its preset (if direction == Direction::Input).
 *          Moreover, this function checks if the stream is blocking, and sets its callback
 *          function if not.
 * @param   oboeStream The stream we want to open
 * @param   direction The Oboe::Direction of the stream we want to open;
 * @param   sampleRate The sample rate of the stream we want to open;
 * @param   androidOutputUsage The Oboe::Usage of the output stream we want to open
 *              (only matters with Android Api level >= 28);
 * @param   androidInputPreset The Preset of the input stream we want to open
 *              (only matters with Android Api level >= 28).
 * @return  paNoError if everything goes as expected, paUnanticipatedHostError if Oboe fails to open
 *          a stream, and paInsufficientMemory if the memory allocation of the buffers fails.
 */
PaError OboeEngine::openStream(OboeStream *i_oboeStream, Direction i_direction, int32_t i_sampleRate,
                               Usage i_androidOutputUsage, InputPreset i_androidInputPreset) {
    PaError error = paNoError;
    Result result;

    if(!(i_oboeStream->isBlocking)){
        i_oboeStream->oboeCallback = new OboeCallback();
        i_oboeStream->oboeCallback->setStreamHolder(i_oboeStream);
        i_oboeStream->oboeCallback->resetCallbackCounters();
    }

    if (i_direction == Direction::Input) {
        i_oboeStream->inputBuilder.setChannelCount(i_oboeStream->bufferProcessor.inputChannelCount)
                ->setFormat(PaToOboeFormat(i_oboeStream->inputFormat))
                ->setSampleRate(i_sampleRate)
                ->setDirection(Direction::Input)
                ->setDeviceId(getSelectedDevice(Direction::Input))
                ->setPerformanceMode(g_inputPerfMode)
                ->setInputPreset(i_androidInputPreset)
                ->setFramesPerCallback(i_oboeStream->framesPerHostCallback);

        if (!(i_oboeStream->isBlocking)) {
            m_inputBuilder.setDataCallback(i_oboeStream->oboeCallback)
                    ->setErrorCallback(i_oboeStream->oboeCallback);
        }

        result = i_oboeStream->inputBuilder.openStream(i_oboeStream->inputStream);

        if (result != Result::OK) {
            LOGE("[OboeEngine::OpenStream]\t Oboe couldn't open the input stream: %s",
                 convertToText(result));
            return paUnanticipatedHostError;
        }

        i_oboeStream->inputStream->setBufferSizeInFrames(i_oboeStream->inputStream->getFramesPerBurst() *
                                                         g_numberOfBuffers);
        i_oboeStream->inputBuffers =
                (void **) PaUtil_AllocateZeroInitializedMemory(g_numberOfBuffers * sizeof(int32_t * ));

        for (int i = 0; i < g_numberOfBuffers; ++i) {
            i_oboeStream->inputBuffers[i] = (void *) PaUtil_AllocateZeroInitializedMemory(
                    i_oboeStream->framesPerHostCallback *
                    i_oboeStream->bytesPerFrame *
                    i_oboeStream->bufferProcessor.inputChannelCount);

            if (!i_oboeStream->inputBuffers[i]) {
                for (int j = 0; j < i; ++j)
                    PaUtil_FreeMemory(i_oboeStream->inputBuffers[j]);
                PaUtil_FreeMemory(i_oboeStream->inputBuffers);
                i_oboeStream->inputStream->close();
                error = paInsufficientMemory;
                break;
            }
        }
        i_oboeStream->currentInputBuffer = 0;
    } else {
        i_oboeStream->outputBuilder.setChannelCount(i_oboeStream->bufferProcessor.outputChannelCount)
                ->setFormat(PaToOboeFormat(i_oboeStream->outputFormat))
                ->setSampleRate(i_sampleRate)
                ->setDirection(Direction::Output)
                ->setDeviceId(getSelectedDevice(Direction::Output))
                ->setPerformanceMode(g_outputPerfMode)
                ->setUsage(i_androidOutputUsage)
                ->setFramesPerCallback(i_oboeStream->framesPerHostCallback);

        if (!(i_oboeStream->isBlocking)) {
            i_oboeStream->outputBuilder.setDataCallback(i_oboeStream->oboeCallback)
                    ->setErrorCallback(i_oboeStream->oboeCallback);
        }

        result = i_oboeStream->outputBuilder.openStream(m_outputStream);
        if (result != Result::OK) {
            LOGE("[OboeEngine::OpenStream]\t Oboe couldn't open the output stream: %s",
                 convertToText(result));
            return paUnanticipatedHostError;
        }

        i_oboeStream->outputStream->setBufferSizeInFrames(i_oboeStream->outputStream->getFramesPerBurst() *
                                                          g_numberOfBuffers);
        i_oboeStream->outputBuffers =
                (void **) PaUtil_AllocateZeroInitializedMemory(g_numberOfBuffers * sizeof(int32_t * ));

        for (int i = 0; i < g_numberOfBuffers; ++i) {
            i_oboeStream->outputBuffers[i] = (void *) PaUtil_AllocateZeroInitializedMemory(
                    i_oboeStream->framesPerHostCallback *
                    i_oboeStream->bytesPerFrame *
                    i_oboeStream->bufferProcessor.outputChannelCount);

            if (!i_oboeStream->outputBuffers[i]) {
                for (int j = 0; j < i; ++j)
                    PaUtil_FreeMemory(i_oboeStream->outputBuffers[j]);
                PaUtil_FreeMemory(i_oboeStream->outputBuffers);
                i_oboeStream->outputStream->close();
                error = paInsufficientMemory;
                break;
            }
        }
        i_oboeStream->currentOutputBuffer = 0;
    }

    return error;
}


/**
 * \brief   Starts oboeStream - both input and output audiostreams are checked
 *          and requested to be started.
 * @return  true if the streams we wanted to start are started successfully, false otherwise.
 */
bool OboeEngine::startStream(OboeStream *i_oboeStream) {
    Result outputResult = Result::OK, inputResult = Result::OK;

    if (i_oboeStream->hasInput) {
        inputResult = i_oboeStream->inputStream->requestStart();
        if (inputResult != Result::OK)
            LOGE("[OboeEngine::startStream]\t Oboe couldn't start the input stream: %s",
                 convertToText(inputResult));
    }
    if (i_oboeStream->hasOutput) {
        outputResult = i_oboeStream->outputStream->requestStart();
        if (outputResult != Result::OK)
            LOGE("[OboeEngine::startStream]\t Oboe couldn't start the output stream: %s",
                 convertToText(outputResult));
    }

    return (outputResult == Result::OK && inputResult == Result::OK);
}


/**
 * \brief   Stops oboeStream - both input and output audiostreams are checked
 *          and requested to be stopped.
 * @return  true if the streams we wanted to stop are stopped successfully, false otherwise.
 */
bool OboeEngine::stopStream(OboeStream *i_oboeStream) {
    Result outputResult = Result::OK, inputResult = Result::OK;

    if (i_oboeStream->hasInput) {
        inputResult = i_oboeStream->inputStream->requestStop();
        if (inputResult != Result::OK)
            LOGE("[OboeEngine::stopStream]\t Oboe couldn't stop the input stream: %s",
                 convertToText(inputResult));
    }
    if (i_oboeStream->hasOutput) {
        outputResult = i_oboeStream->outputStream->requestStop();
        if (outputResult != Result::OK)
            LOGE("[OboeEngine::stopStream]\t Oboe couldn't stop the output stream: %s",
                 convertToText(outputResult));
    }

    return (outputResult == Result::OK && inputResult == Result::OK);
}


/**
 * \brief   Called when it's needed to restart the oboeStream's audio stream(s), mainly when the
 *          audio devices change while a stream is started.
 * @return  true if the stream is restarted successfully, false otherwise.
 */
bool OboeEngine::restartStream(OboeStream* i_oboeStream, int i_direction) {
    bool outcome = true;
    Result result;

    switch (i_direction) {
        case 1: //output-only
            //stopping and closing
            result = i_oboeStream->outputStream->stop();
            if (result != Result::OK)
                LOGW("[OboeEngine::restartStream]\t Oboe couldn't stop the output stream: %s",
                     convertToText(result));
            result = i_oboeStream->outputStream->close();
            if (result != Result::OK)
                LOGW("[OboeEngine::restartStream]\t Oboe couldn't close the output stream: %s",
                     convertToText(result));

            //reopening and restarting
            result = i_oboeStream->outputBuilder.openStream(i_oboeStream->outputStream);
            if (result != Result::OK)
                LOGE("[OboeEngine::restartStream]\t Oboe couldn't reopen the output stream: %s",
                     convertToText(result));
            result = i_oboeStream->outputStream->start();
            if (result != Result::OK) {
                LOGE("[OboeEngine::restartStream]\t Oboe couldn't restart the output stream: %s",
                     convertToText(result));
                outcome = false;
            }
            break;

        case 2: //input-only
            //stopping and closing
            result = i_oboeStream->inputStream->stop();
            if (result != Result::OK)
                LOGW("[OboeEngine::restartStream]\t Oboe couldn't stop the input stream: %s",
                     convertToText(result));
            result = i_oboeStream->inputStream->close();
            if (result != Result::OK)
                LOGW("[OboeEngine::restartStream]\t Oboe couldn't close the input stream: %s",
                     convertToText(result));

            //reopening and restarting
            result = inputBuilder.openStream(i_oboeStream->inputStream);
            if (result != Result::OK)
                LOGE("[OboeEngine::restartStream]\t Oboe couldn't reopen the input stream: %s",
                     convertToText(result));
            result = i_oboeStream->inputStream->start();
            if (result != Result::OK) {
                LOGE("[OboeEngine::restartStream]\t Oboe couldn't restart the input stream: %s",
                     convertToText(result));
                outcome = false;
            }
            break;

        default:
            // unspecified direction or both directions: restart both streams
            LOGW("[OboeEngine::restartStream]\t Unspecified direction, restarting both streams");
            outcome = (restartStream(i_oboeStream, 1) && restartStream(i_oboeStream, 2));
            break;
    }

    return outcome;
}


/**
 * \brief   Closes oboeStream - both input and output audiostreams are checked
 *          and closed if active.
 * @return  true if the stream is closed successfully, otherwise returns false.
 */
bool OboeEngine::closeStream(OboeStream *i_oboeStream) {
    Result outputResult = Result::OK, inputResult = Result::OK;
    bool hasOutput = true, hasInput = true;

    if (i_oboeStream == nullptr) {
        LOGW("[OboeEngine::closeStream]\t i_oboeStream is a nullptr. Terminating both oboe streams.");
    } else {
        hasInput = i_oboeStream->hasInput;
        hasOutput = i_oboeStream->hasOutput;
    }

    if (hasOutput) {
        outputResult = i_oboeStream->outputStream->close();
        if (outputResult == Result::ErrorClosed) {
            outputResult = Result::OK;
            LOGW("[OboeEngine::closeStream]\t Tried to close output stream, but was already closed.");
        }
    }
    if (hasInput) {
        inputResult = i_oboeStream->inputStream->close();
        if (inputResult == Result::ErrorClosed) {
            inputResult = Result::OK;
            LOGW("[OboeEngine::closeStream]\t Tried to close input stream, but was already closed.");
        }
    }

    return (outputResult == Result::OK && inputResult == Result::OK);
}


/**
 * \brief   Stops oboeStream - both input and output audiostreams are checked and forcefully stopped.
 * @return  true if the output stream and the input stream are stopped successfully, false otherwise.
 */
bool OboeEngine::abortStream(OboeStream *i_oboeStream) {
    Result outputResult = Result::OK, inputResult = Result::OK;
    bool hasOutput = true, hasInput = true;

    if (i_oboeStream == nullptr) {
        LOGW("[OboeEngine::closeStream]\t i_oboeStream is a nullptr. Aborting both oboe streams.");
    } else {
        hasInput = i_oboeStream->hasInput;
        hasOutput = i_oboeStream->hasOutput;
    }

    if (hasInput) {
        inputResult = i_oboeStream->inputStream->stop();
        if (inputResult != Result::OK)
            LOGE("[OboeEngine::abortStream]\t Couldn't force the input stream to stop: %s",
                 convertToText(inputResult));
        inputResult = i_oboeStream->inputStream->close();
        if (i_oboeStream->inputResult != Result::OK)
            LOGE("[OboeEngine::abortStream]\t Couldn't force the input stream to close: %s",
                 convertToText(inputResult));
    }
    if (hasOutput) {
        outputResult = i_oboeStream->outputStream->stop();
        if (i_oboeStream->outputResult != Result::OK)
            LOGE("[OboeEngine::abortStream]\t Couldn't force the output stream to stop: %s",
                 convertToText(outputResult));
        outputResult = i_oboeStream->outputStream->close();
        if (outputResult != Result::OK)
            LOGE("[OboeEngine::abortStream]\t Couldn't force the output stream to close: %s",
                 convertToText(outputResult));
    }

    return (outputResult == Result::OK && inputResult == Result::OK);
}


/**
 * \brief   Writes frames on the output stream of oboeStream. Used by blocking streams.
 * @param   buffer The buffer that we want to write on the output stream;
 * @param   framesToWrite The number of frames that we want to write.
 * @return  true if the buffer is written correctly, false if the write function returns an error
 *          different from ErrorDisconnected. In case of ErrorDisconnected, the function returns
 *          true if the stream is successfully restarted, and false otherwise.
 */
bool OboeEngine::writeStream(OboeStream *i_oboeStream, const void *i_buffer, int32_t i_framesToWrite) {
    bool outcome = true;

    ResultWithValue <int32_t> result = i_oboeStream->outputStream->write(i_buffer, i_framesToWrite, TIMEOUT_NS);

    // If the stream is interrupted because the device suddenly changes, restart the stream.
    if (result.error() == Result::ErrorDisconnected) {
        if (restartStream(i_oboeStream, 1))
            return true;
    }

    if (!result) {
        LOGE("[OboeEngine::writeStream]\t Error writing stream: %s", convertToText(result.error()));
        outcome = false;
    }
    return outcome;
}


/**
 * \brief   Reads frames from the input stream of oboeStream. Used by blocking streams.
 * @param   buffer The buffer that we want to read from the input stream;
 * @param   framesToWrite The number of frames that we want to read.
 * @return  true if the buffer is read correctly, false if the read function returns an error
 *          different from ErrorDisconnected. In case of ErrorDisconnected, the function returns
 *          true if the stream is successfully restarted, and false otherwise.
 */
bool OboeEngine::readStream(OboeStream *i_oboeStream, void *i_buffer, int32_t i_framesToRead) {
    bool outcome = true;

    ResultWithValue <int32_t> result = i_oboeStream->inputStream->read(i_buffer, i_framesToRead, TIMEOUT_NS);

    // If the stream is interrupted because the device suddenly changes, restart the stream.
    if (result.error() == Result::ErrorDisconnected) {
        if (restartStream(i_oboeStream, 2))
            return true;
    }

    if (!result) {
        LOGE("[OboeEngine::readStream]\t Error reading stream: %s", convertToText(result.error()));
        outcome = false;
    }
    return outcome;
}


/**
 * \brief   Allocates the memory of oboeStream.
 * @return  the address of the oboeStream.
 */
OboeStream *OboeEngine::allocateOboeStream() {
    OboeStream *oboeStream = (OboeStream *) PaUtil_AllocateZeroInitializedMemory(sizeof(OboeStream));
    oboeStream->setEngineAddress(this);
    return oboeStream;
}


/**
 * \brief   Converts PaSampleFormat values into Oboe::AudioFormat values.
 * @param   paFormat the PaSampleFormat we want to convert.
 * @return  the converted AudioFormat.
 */
AudioFormat OboeEngine::PaToOboeFormat(PaSampleFormat i_paFormat) {
    AudioFormat oboeFormat;
    switch (i_paFormat) {
        case paFloat32:
            oboeFormat = AudioFormat::Float;
            LOGI("[OboeEngine::PaToOboeFormat]\t REQUESTED OBOE FORMAT: FLOAT");
            break;
        case paInt16:
            oboeFormat = AudioFormat::I16;
            LOGI("[OboeEngine::PaToOboeFormat]\t REQUESTED OBOE FORMAT: I16");
            break;
        case paInt32:
            oboeFormat = AudioFormat::I32;
            LOGI("[OboeEngine::PaToOboeFormat]\t REQUESTED OBOE FORMAT: I32");
            break;
        case paInt24:
            oboeFormat = AudioFormat::I24;
            LOGI("[OboeEngine::PaToOboeFormat]\t REQUESTED OBOE FORMAT: I24");
            break;
        default:
            oboeFormat = AudioFormat::Unspecified;
            LOGW("[OboeEngine::PaToOboeFormat]\t Setting AudioFormat to Unspecified, Oboe does not support the requested format.");
            break;
    }
    return oboeFormat;
}


/**
 * \brief   Function used to implement device selection. Device Ids are kUnspecified by default, but
 *          can be set to something different via JNI using the function PaOboe_SetSelectedDevice.
 * @param   direction the Oboe::Direction for which we want to know the device Id.
 * @return  the device Id of the appropriate direction.
 */
int32_t OboeEngine::getSelectedDevice(Direction i_direction) {
    if (i_direction == Direction::Input)
        return g_inputDeviceId;
    else
        return g_outputDeviceId;
}

/*----------------------------- OboeCallback functions implementations -----------------------------*/
/**
 * \brief   Oboe's callback routine.
 */
DataCallbackResult
OboeCallback::onAudioReady(AudioStream *i_audioStream, void *i_audioData, int32_t i_numFrames) {

    clock_gettime(CLOCK_REALTIME, &m_timeSpec);
    m_timeInfo.currentTime = (PaTime)(m_timeSpec.tv_sec + (m_timeSpec.tv_nsec / 1000000000.0));
    m_timeInfo.outputBufferDacTime = (PaTime)(m_oboeStreamHolder->framesPerHostCallback
                                              /
                                              m_oboeStreamHolder->streamRepresentation.streamInfo.sampleRate
                                              + m_timeInfo.currentTime);
    m_timeInfo.inputBufferAdcTime = (PaTime)(m_oboeStreamHolder->framesPerHostCallback
                                             /
                                             m_oboeStreamHolder->streamRepresentation.streamInfo.sampleRate
                                             + m_timeInfo.currentTime);

    /* check if StopStream or AbortStream was called */
    if (m_oboeStreamHolder->doStop) {
        m_oboeStreamHolder->callbackResult = paComplete;
    } else if (m_oboeStreamHolder->doAbort) {
        m_oboeStreamHolder->callbackResult = paAbort;
    }

    PaUtil_BeginCpuLoadMeasurement(&m_oboeStreamHolder->cpuLoadMeasurer);
    PaUtil_BeginBufferProcessing(&m_oboeStreamHolder->bufferProcessor,
                                 &m_timeInfo, m_oboeStreamHolder->cbFlags);

    if (m_oboeStreamHolder->hasOutput) {
        m_oboeStreamHolder->outputBuffers[m_oboeStreamHolder->currentOutputBuffer] = i_audioData;
        PaUtil_SetOutputFrameCount(&m_oboeStreamHolder->bufferProcessor, i_numFrames);
        PaUtil_SetInterleavedOutputChannels(&m_oboeStreamHolder->bufferProcessor, 0,
                                            (void *) ((PaInt16 **) m_oboeStreamHolder->outputBuffers)[
                                                    m_oboeStreamHolder->currentOutputBuffer],
                                            0);
    }
    if (m_oboeStreamHolder->hasInput) {
        i_audioData = m_oboeStreamHolder->inputBuffers[m_oboeStreamHolder->currentInputBuffer];
        PaUtil_SetInputFrameCount(&m_oboeStreamHolder->bufferProcessor, 0);
        PaUtil_SetInterleavedInputChannels(&m_oboeStreamHolder->bufferProcessor, 0,
                                           (void *) ((PaInt16 **) m_oboeStreamHolder->inputBuffers)[
                                                   m_oboeStreamHolder->currentInputBuffer],
                                           0);
    }

    /* continue processing user buffers if cbresult is paContinue or if cbresult is  paComplete and userBuffers aren't empty yet  */
    if (m_oboeStreamHolder->callbackResult == paContinue
        || (m_oboeStreamHolder->callbackResult == paComplete
            && !PaUtil_IsBufferProcessorOutputEmpty(&m_oboeStreamHolder->bufferProcessor))) {
        m_framesProcessed = PaUtil_EndBufferProcessing(&m_oboeStreamHolder->bufferProcessor,
                                                       &m_oboeStreamHolder->callbackResult);
    }

    /* enqueue a buffer only when there are frames to be processed,
     * this will be 0 when paComplete + empty buffers or paAbort
     */
    if (m_framesProcessed > 0) {
        if (m_oboeStreamHolder->hasOutput) {
            m_oboeStreamHolder->currentOutputBuffer =
                    (m_oboeStreamHolder->currentOutputBuffer + 1) % g_numberOfBuffers;
        }
        if (m_oboeStreamHolder->hasInput) {
            m_oboeStreamHolder->currentInputBuffer = (m_oboeStreamHolder->currentInputBuffer + 1) % g_numberOfBuffers;
        }
    }

    PaUtil_EndCpuLoadMeasurement(&m_oboeStreamHolder->cpuLoadMeasurer, m_framesProcessed);

    /* StopStream was called */
    if (m_framesProcessed == 0 && m_oboeStreamHolder->doStop) {
        m_oboeStreamHolder->oboeCallbackResult = DataCallbackResult::Stop;
    }

        /* if AbortStream or StopStream weren't called, stop from the cb */
    else if (m_framesProcessed == 0 && !(m_oboeStreamHolder->doAbort || m_oboeStreamHolder->doStop)) {
        m_oboeStreamHolder->isActive = false;
        m_oboeStreamHolder->isStopped = true;
        if (m_oboeStreamHolder->streamRepresentation.streamFinishedCallback != nullptr)
            m_oboeStreamHolder->streamRepresentation.streamFinishedCallback(
                    m_oboeStreamHolder->streamRepresentation.userData);
        m_oboeStreamHolder->oboeCallbackResult = DataCallbackResult::Stop; //TODO: Resume this test (onAudioReady)
    }

    return m_oboeStreamHolder->oboeCallbackResult;
}


/**
 * \brief   If the data callback ends without returning DataCallbackResult::Stop, this routine tells
 *          what error occurred.
 */
void OboeCallback::onErrorAfterClose(AudioStream *i_audioStream, Result i_error) {
    if (i_error == oboe::Result::ErrorDisconnected) {
        OboeEngine* oboeEngine = m_oboeStreamHolder->getEngineAddress();
        LOGW("[OboeCallback::onErrorAfterClose]\t ErrorDisconnected - Restarting stream(s)");
        if (!oboeEngine->restartStream(m_oboeStreamHolder, 0))
            LOGE("[OboeCallback::onErrorAfterClose]\t Couldn't restart stream(s)");
    } else
        LOGE("[OboeCallback::onErrorAfterClose]\t Error was %s", oboe::convertToText(i_error));
}


/**
 * \brief   Resets callback counters (called at the start of each iteration of onAudioReady
 */
void OboeCallback::resetCallbackCounters() {
    m_framesProcessed = 0;
    m_timeInfo = {0, 0, 0};
}


/*----------------------------- PaSkeleton functions implementations -----------------------------*/

/**
 * \brief   Checks if the requested sample rate is supported by the output device using
 *          OboeEngine::tryStream.
 *          This function is used by PaOboe_Initialize, IsFormatSupported, and OpenStream.
 * @param   oboeHostApi points towards a OboeHostApiRepresentation (see struct defined at the top of this file);
 * @param   sampleRate is the sample rate we want to check.
 * @return  PaNoError regardless of the outcome of the check, but warns in the Logs if the sample
 *          rate was changed by Oboe.
 */
PaError IsOutputSampleRateSupported(PaOboeHostApiRepresentation *i_oboeHostApi, double i_sampleRate) {
    if (!(i_oboeHostApi->oboeEngine->tryStream(Direction::Output,
                                               i_sampleRate,
                                               kUnspecified)))
        LOGW("[PaOboe - IsOutputSampleRateSupported]\t Sample Rate was changed by Oboe.");

    /*  Since Oboe manages the sample rate in a smart way, we can avoid blocking the process if the
        sample rate we requested wasn't supported.  */
    return paNoError;
}


/**
 * \brief   Checks if the requested sample rate is supported by the input device using
 *          OboeEngine::tryStream.
 *          This function is used by PaOboe_Initialize, IsFormatSupported, and OpenStream.
 * @param   oboeHostApi points towards a OboeHostApiRepresentation (see struct defined at the top of
 *              this file);
 * @param   sampleRate is the sample rate we want to check.
 * @return  PaNoError regardless of the outcome of the check, but warns in the Logs if the sample
 *          rate was changed by Oboe.
 */
PaError IsInputSampleRateSupported(PaOboeHostApiRepresentation *i_oboeHostApi, double i_sampleRate) {
    if (!(i_oboeHostApi->oboeEngine->tryStream(Direction::Input,
                                               i_sampleRate,
                                               kUnspecified)))
        LOGW("[PaOboe - IsInputSampleRateSupported]\t Sample Rate was changed by Oboe.");

    /*  Since Oboe manages the sample rate in a smart way, we can avoid blocking the process if the
        sample rate we requested wasn't supported.  */
    return paNoError;
}


/**
 * \brief   Checks if the requested channel count is supported by the output device using
 *          OboeEngine::tryStream. Used by PaOboe_Initialize.
 * @param   oboeHostApi points towards a OboeHostApiRepresentation (see struct defined at the top of
 *              this file);
 * @param   numOfChannels the number of channels we want to check.
 * @return  PaNoError regardless of the outcome of the check, but warns in the Logs if the channel
 *          count was changed by Oboe.
 */
static PaError IsOutputChannelCountSupported(PaOboeHostApiRepresentation *i_oboeHostApi, int32_t i_numOfChannels) {
    if (i_numOfChannels > 2 || i_numOfChannels == 0) {
        LOGE("[PaOboe - IsOutputChannelCountSupported]\t Invalid channel count.");
        return paInvalidChannelCount;
    }

    if (!(i_oboeHostApi->oboeEngine->tryStream(Direction::Output,
                                               kUnspecified,
                                               i_numOfChannels)))
        LOGW("[PaOboe - IsOutputChannelCountSupported]\t Channel Count was changed by Oboe. The device might not support stereo audio.");

    /*  Since Oboe manages the channel count in a smart way, we can avoid blocking the process if
        the sample rate we requested wasn't supported.  */
    return paNoError;
}


/**
 * \brief   Checks if the requested channel count is supported by the input device using
 *          OboeEngine::tryStream. Used by PaOboe_Initialize.
 * @param   oboeHostApi points towards a OboeHostApiRepresentation (see struct defined at the top of
 *              this file);
 * @param   numOfChannels the number of channels we want to check.
 * @return  PaNoError regardless of the outcome of the check, but warns in the Logs if the channel
 *          count was changed by Oboe.
 */
static PaError IsInputChannelCountSupported(PaOboeHostApiRepresentation *i_oboeHostApi, int32_t i_numOfChannels) {
    if (i_numOfChannels > 2 || i_numOfChannels == 0) {
        LOGE("[PaOboe - IsInputChannelCountSupported]\t Invalid channel count.");
        return paInvalidChannelCount;
    }

    if (!(i_oboeHostApi->oboeEngine->tryStream(Direction::Input,
                                               kUnspecified,
                                               i_numOfChannels)))
        LOGW("[PaOboe - IsInputChannelCountSupported]\t Channel Count was changed by Oboe. The device might not support stereo audio.");

    /*  Since Oboe manages the channel count in a smart way, we can avoid blocking the process if
        the sample rate we requested wasn't supported.  */
    return paNoError;
}


/**
 * \brief   Initializes common parameters and the OboeEngine, and allocates the memory necessary to
 *          start the audio streams.
 * @param   hostApi points towards a *HostApiRepresentation, which is a structure representing the
 *              interface to a host API (see struct in "pa_hostapi.h");
 * @param   hostApiIndex is a PaHostApiIndex, the type used to enumerate the host APIs at runtime.
 * @return  paNoError if no errors occur, or paInsufficientMemory if memory allocation fails;
 */
PaError PaOboe_Initialize(PaUtilHostApiRepresentation **i_hostApi, PaHostApiIndex i_hostApiIndex) {
    PaError result = paNoError;
    int deviceCount;
    PaOboeHostApiRepresentation *oboeHostApi;
    PaDeviceInfo *deviceInfoArray;
    char *deviceName;

    oboeHostApi = (PaOboeHostApiRepresentation *) PaUtil_AllocateZeroInitializedMemory(
            sizeof(PaOboeHostApiRepresentation));
    if (!oboeHostApi) {
        result = paInsufficientMemory;
        goto error;
    }

    oboeHostApi->oboeEngine = new OboeEngine();

    oboeHostApi->allocations = PaUtil_CreateAllocationGroup();
    if (!oboeHostApi->allocations) {
        result = paInsufficientMemory;
        goto error;
    }

    *i_hostApi = &oboeHostApi->inheritedHostApiRep;
    // Initialization of infos.
    (*i_hostApi)->info.structVersion = 1;
    (*i_hostApi)->info.type = paInDevelopment;
    (*i_hostApi)->info.name = "android Oboe";
    (*i_hostApi)->info.defaultOutputDevice = 0;
    (*i_hostApi)->info.defaultInputDevice = 0;
    (*i_hostApi)->info.deviceCount = 0;

    deviceCount = 1;
    (*i_hostApi)->deviceInfos = (PaDeviceInfo **) PaUtil_GroupAllocateZeroInitializedMemory(
            oboeHostApi->allocations, sizeof(PaDeviceInfo * ) * deviceCount);

    if (!(*i_hostApi)->deviceInfos) {
        result = paInsufficientMemory;
        goto error;
    }

    /* allocate all device info structs in a contiguous block */
    deviceInfoArray = (PaDeviceInfo *) PaUtil_GroupAllocateZeroInitializedMemory(
            oboeHostApi->allocations, sizeof(PaDeviceInfo) * deviceCount);
    if (!deviceInfoArray) {
        result = paInsufficientMemory;
        goto error;
    }

    for (int i = 0; i < deviceCount; ++i) {
        PaDeviceInfo *deviceInfo = &deviceInfoArray[i];
        deviceInfo->structVersion = 2;
        deviceInfo->hostApi = hostApiIndex;

        /* OboeEngine will handle manual device selection through the use of PaOboe_SetSelectedDevice.
           Portaudio doesn't need to know about this, so we just use a default device. */
        deviceInfo->name = "default";

        /* Try channels in order of preference - Stereo > Mono. */
        const int32_t channelsToTry[] = {2, 1};
        const int32_t channelsToTryLength = 2;

        deviceInfo->maxOutputChannels = 0;
        deviceInfo->maxInputChannels = 0;

        for (i = 0; i < channelsToTryLength; ++i) {
            if (IsOutputChannelCountSupported(oboeHostApi, channelsToTry[i]) == paNoError) {
                deviceInfo->maxOutputChannels = channelsToTry[i];
                break;
            }
        }
        for (i = 0; i < channelsToTryLength; ++i) {
            if (IsInputChannelCountSupported(oboeHostApi, channelsToTry[i]) == paNoError) {
                deviceInfo->maxInputChannels = channelsToTry[i];
                break;
            }
        }

        /* check sample rates in order of preference */
        const int32_t sampleRates[] = {48000, 44100, 32000, 24000, 16000};
        const int32_t numberOfSampleRates = 5;

        deviceInfo->defaultSampleRate = sampleRates[0];

        for (i = 0; i < numberOfSampleRates; ++i) {
            if (IsOutputSampleRateSupported(oboeHostApi, sampleRates[i]) == paNoError &&
                    IsInputSampleRateSupported(oboeHostApi, sampleRates[i]) == paNoError) {
                deviceInfo->defaultSampleRate = sampleRates[i];
                break;
            }
        }
        if (deviceInfo->defaultSampleRate == 0)
            goto error;

        /* If the user has set g_nativeBufferSize by querying the optimal buffer size via java,
           use the user-defined value since that will offer the lowest possible latency. */

        if (g_nativeBufferSize != 0) {
            deviceInfo->defaultLowInputLatency =
                    (double) g_nativeBufferSize / deviceInfo->defaultSampleRate;
            deviceInfo->defaultLowOutputLatency =
                    (double) g_nativeBufferSize / deviceInfo->defaultSampleRate;
            deviceInfo->defaultHighInputLatency =
                    (double) g_nativeBufferSize * 4 / deviceInfo->defaultSampleRate;
            deviceInfo->defaultHighOutputLatency =
                    (double) g_nativeBufferSize * 4 / deviceInfo->defaultSampleRate;
        } else {
            deviceInfo->defaultLowInputLatency =
                    (double) GetApproximateLowBufferSize() / deviceInfo->defaultSampleRate;
            deviceInfo->defaultLowOutputLatency =
                    (double) GetApproximateLowBufferSize() / deviceInfo->defaultSampleRate;
            deviceInfo->defaultHighInputLatency =
                    (double) GetApproximateLowBufferSize() * 4 / deviceInfo->defaultSampleRate;
            deviceInfo->defaultHighOutputLatency =
                    (double) GetApproximateLowBufferSize() * 4 / deviceInfo->defaultSampleRate;
        }

        (*i_hostApi)->deviceInfos[i] = deviceInfo;
        ++(*i_hostApi)->info.deviceCount;
    }

    (*i_hostApi)->Terminate = Terminate;
    (*i_hostApi)->OpenStream = OpenStream;
    (*i_hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface(&oboeHostApi->callbackStreamInterface,
                                     CloseStream, StartStream, StopStream,
                                     AbortStream, IsStreamStopped,
                                     IsStreamActive, GetStreamTime,
                                     GetStreamCpuLoad, PaUtil_DummyRead,
                                     PaUtil_DummyWrite,
                                     PaUtil_DummyGetReadAvailable,
                                     PaUtil_DummyGetWriteAvailable);

    PaUtil_InitializeStreamInterface(&oboeHostApi->blockingStreamInterface,
                                     CloseStream, StartStream, StopStream,
                                     AbortStream, IsStreamStopped,
                                     IsStreamActive, GetStreamTime,
                                     PaUtil_DummyGetCpuLoad, ReadStream,
                                     WriteStream, GetStreamReadAvailable,
                                     GetStreamWriteAvailable);

    if (result == paNoError)
        LOGV("[PaOboe - Initialize]\t Oboe host API successfully initialized");
    else
        LOGE("[PaOboe - Initialize]\t An unusual error occurred. Error code: %d", result);
    return result;

    error:
    if (oboeHostApi) {
        if (oboeHostApi->allocations) {
            PaUtil_FreeAllAllocations(oboeHostApi->allocations);
            PaUtil_DestroyAllocationGroup(oboeHostApi->allocations);
        }

        PaUtil_FreeMemory(boeHostApi);
    }
    LOGE("[PaOboe - Initialize]\t Initialization failed. Error code: %d", result);
    return result;
}


/**
 * \brief   Interrupts the stream and frees the memory that was allocated to sustain the stream.
 * @param   hostApi points towards a *HostApiRepresentation, which is a structure representing the
 *              interface to a host API (see struct in "pa_hostapi.h").
 */
static void Terminate(struct PaUtilHostApiRepresentation *i_hostApi) {
    auto *oboeHostApi = (PaOboeHostApiRepresentation *) i_hostApi;

    if (!(oboeHostApi->oboeEngine->closeStream(nullptr)))
        LOGW("[PaOboe - Terminate]\t Couldn't close the streams correctly - see OboeEngine::CloseStream logs.");

    if (oboeHostApi->oboeEngine != nullptr)
        delete oboeHostApi->oboeEngine;

    if (oboeHostApi->allocations) {
        PaUtil_FreeAllAllocations(oboeHostApi->allocations);
        PaUtil_DestroyAllocationGroup(oboeHostApi->allocations);
    }

    PaUtil_FreeMemory(oboeHostApi);
}


/**
 * \brief   Checks if the initialized values are supported by the selected device(s).
 * @param   hostApi points towards a *HostApiRepresentation, which is a structure representing the
 *              interface to a host API (see struct in "pa_hostapi.h");
 * @param   inputParameters points towards the parameters given to the input stream;
 * @param   outputParameters points towards the parameters given to the output stream;
 * @param   sampleRate is the value of the sample rate we want to check if it's supported.
 * @return  paNoError (== paFormatIsSupported) if no errors occur, otherwise returns an appropriate
 *          PaError message.
 */
static PaError IsFormatSupported(struct PaUtilHostApiRepresentation *i_hostApi,
                                 const PaStreamParameters *i_inputParameters,
                                 const PaStreamParameters *i_outputParameters,
                                 double i_sampleRate) {
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    auto *oboeHostApi = (PaOboeHostApiRepresentation *) i_hostApi;

    if (i_inputParameters) {
        inputChannelCount = i_inputParameters->channelCount;
        inputSampleFormat = i_inputParameters->sampleFormat;

        /* all standard sample formats are supported by the buffer adapter,
            this implementation doesn't support any custom sample formats */
        if (inputSampleFormat & paCustomFormat) {
            return paSampleFormatNotSupported;
        }

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */
        if (i_inputParameters->device == paUseHostApiSpecificDeviceSpecification) {
            return paInvalidDevice;
        }

        /* check that input device can support inputChannelCount */
        if (inputChannelCount >
                i_hostApi->deviceInfos[i_inputParameters->device]->maxInputChannels) {
            return paInvalidChannelCount;
        }

        /* validate inputStreamInfo */
        if (i_inputParameters->hostApiSpecificStreamInfo) {
            // Only has an effect on ANDROID_API>=28.
            InputPreset androidRecordingPreset =
                    ((PaOboeStreamInfo *) i_inputParameters->hostApiSpecificStreamInfo)->androidInputPreset;
            if (androidRecordingPreset != InputPreset::Generic &&
                androidRecordingPreset != InputPreset::Camcorder &&
                androidRecordingPreset != InputPreset::VoiceRecognition &&
                androidRecordingPreset != InputPreset::VoiceCommunication
                androidRecordingPreset != InputPreset::VoicePerformance) {
                return paIncompatibleHostApiSpecificStreamInfo;
            }
        }
    } else {
        inputChannelCount = 0;
    }

    if (i_outputParameters) {
        outputChannelCount = i_outputParameters->channelCount;
        outputSampleFormat = i_outputParameters->sampleFormat;

        /* all standard sample formats are supported by the buffer adapter,
            this implementation doesn't support any custom sample formats */
        if (outputSampleFormat & paCustomFormat) {
            return paSampleFormatNotSupported;
        }

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */
        if (i_outputParameters->device == paUseHostApiSpecificDeviceSpecification) {
            return paInvalidDevice;
        }

        /* check that output device can support outputChannelCount */
        if (outputChannelCount > i_hostApi->deviceInfos[i_outputParameters->device]->maxOutputChannels) {
            return paInvalidChannelCount;
        }

        /* validate outputStreamInfo */
        if (i_outputParameters->hostApiSpecificStreamInfo) {
            // Only has an effect on ANDROID_API>=28.
            Usage androidOutputUsage =
                    ((PaOboeStreamInfo *) i_outputParameters->hostApiSpecificStreamInfo)->androidOutputUsage;
            if (androidOutputUsage != Usage::Media &&
                androidOutputUsage != Usage::Notification &&
                androidOutputUsage != Usage::NotificationEvent &&
                androidOutputUsage != Usage::NotificationRingtone &&
                androidOutputUsage != Usage::VoiceCommunication &&
                androidOutputUsage != Usage::VoiceCommunicationSignalling &&
                androidOutputUsage != Usage::Alarm &&
                androidOutputUsage != Usage::Game) {
                return paIncompatibleHostApiSpecificStreamInfo;
            }
        }
    } else {
        outputChannelCount = 0;
    }

    if (outputChannelCount > 0) {
        if (IsOutputSampleRateSupported(oboeHostApi, i_sampleRate) != paNoError) {
            return paInvalidSampleRate;
        }
    }
    if (inputChannelCount > 0) {
        if (IsInputSampleRateSupported(oboeHostApi, i_sampleRate) != paNoError) {
            return paInvalidSampleRate;
        }
    }

    return paFormatIsSupported;
}


/**
 * \brief   Calls OboeEngine::openStream to open the outputStream and a Generic input preset.
 * @param   oboeStream is the OboeStream we want to initialize in the output direction.
 * @param   oboeHostApi points towards a OboeHostApiRepresentation (see struct defined at the top of
 *              this file);
 * @param   androidOutputUsage is an attribute that expresses why we are opening the output stream.
 *              This information can be used by certain platforms to make more refined volume or
 *              routing decisions. It only has an effect on Android API 28+.
 * @param   sampleRate is the sample rate we want for the audio stream we want to initialize. This is used to allocate
 *              the correct amount of memory.
 * @return  the value returned by OboeEngine::openStream.
 */
static PaError InitializeOutputStream(OboeStream i_oboeStream, PaOboeHostApiRepresentation *i_oboeHostApi,
                                      Usage i_androidOutputUsage, double i_sampleRate) {

    return i_oboeHostApi->oboeEngine->openStream(i_oboeStream,
                                                 Direction::Output,
                                                 sampleRate,
                                                 androidOutputUsage,
                                                 Generic); //Input preset won't be used, so we put the default value.
}


/**
 * \brief   Calls OboeEngine::openStream to open the outputStream and a Generic input preset.
 * @param   oboeStream is the OboeStream we want to initialize in the input direction.
 * @param   oboeHostApi points towards a OboeHostApiRepresentation (see struct defined at the top of
 *              this file);
 * @param   androidInputPreset is an attribute that defines the audio source. This information
 *              defines both a default physical source of audio signal, and a recording configuration.
 *              It only has an effect on Android API 28+.
 * @param   sampleRate is the sample rate we want for the audio stream we want to initialize. This is used to allocate
 *              the correct amount of memory.
 * @return  the value returned by OboeEngine::openStream.
 */
static PaError InitializeInputStream(OboeStream i_oboeStream, PaOboeHostApiRepresentation *i_oboeHostApi,
                                     InputPreset i_androidInputPreset, double i_sampleRate) {

    return i_oboeHostApi->oboeEngine->openStream(i_oboeStream,
                                                 Direction::Input,
                                                 i_sampleRate,
                                                 Usage::Media,   //Usage won't be used, so we put the default value.
                                                 i_androidInputPreset);
}


/**
 * \brief   Opens the portaudio audio stream - while initializing our OboeStream.
 * @param   hostApi points towards a *HostApiRepresentation, which is a structure representing the
 *              interface to a host API (see struct in "pa_hostapi.h");
 * @param   paStream points to a pointer to a PaStream, which is an audio stream structure used and built
 *              by portaudio, which will hold the information of our OboeStream;
 * @param   inputParameters points towards the parameters given to the input stream;
 * @param   outputParameters points towards the parameters given to the output stream;
 * @param   sampleRate the sample rate we want for our stream;
 * @param   framesPerBuffer the number of frames per buffer we want for our stream;
 * @param   streamFlags the flags used to control the behavior of a stream;
 * @param   streamCallback points to a callback function that allows a non-blocking stream to
 *              receive or transmit data;
 * @param   userData stores the user data, and is passed to some PaUtil functions without further
 *              manipulation or checks.
 * @return  paNoError if no errors occur, or other error codes accordingly with what goes wrong.
*/
static PaError OpenStream(struct PaUtilHostApiRepresentation *i_hostApi,
                          PaStream **i_paStream,
                          const PaStreamParameters *i_inputParameters,
                          const PaStreamParameters *i_outputParameters,
                          double i_sampleRate,
                          unsigned long i_framesPerBuffer,
                          PaStreamFlags i_streamFlags,
                          PaStreamCallback *i_streamCallback,
                          void *i_userData) {
    PaError error = paNoError;
    auto oboeHostApi = (PaOboeHostApiRepresentation *) i_hostApi;
    unsigned long framesPerHostBuffer; /* these may not be equivalent for all implementations */
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    PaSampleFormat hostInputSampleFormat, hostOutputSampleFormat;

    Usage androidOutputUsage = Usage::VoiceCommunication;
    InputPreset androidInputPreset = InputPreset::Generic;

    OboeStream *oboeStream = oboeHostApi->oboeEngine->allocateOboeStream();

    if (!oboeStream) {
        error = paInsufficientMemory;
        goto error;
    }

    LOGI("[PaOboe - OpenStream]\t OpenStream called.");

    if (i_inputParameters) {
        inputChannelCount = i_inputParameters->channelCount;
        inputSampleFormat = i_inputParameters->sampleFormat;

        /* Oboe supports alternate device specification with API>=28, but here we reject the use of
            paUseHostApiSpecificDeviceSpecification and stick with the default. Devices can be set via
            PaOboe_SetSelectedDevice. */
        if (i_inputParameters->device == paUseHostApiSpecificDeviceSpecification)
            return paInvalidDevice;

        /* check that input device can support inputChannelCount */
        if (inputChannelCount > i_hostApi->deviceInfos[i_inputParameters->device]->maxInputChannels)
            return paInvalidChannelCount;

        /* validate inputStreamInfo */
        if (i_inputParameters->hostApiSpecificStreamInfo) {
            // Only has an effect on ANDROID_API>=28.
            androidInputPreset =
                    ((PaOboeStreamInfo *) i_outputParameters->hostApiSpecificStreamInfo)->androidInputPreset;
            if (androidInputPreset != InputPreset::Generic &&
                androidInputPreset != InputPreset::Camcorder &&
                androidInputPreset != InputPreset::VoiceRecognition &&
                androidInputPreset != InputPreset::VoiceCommunication
                androidInputPreset != InputPreset::VoicePerformance)
                return paIncompatibleHostApiSpecificStreamInfo;
        }
        /* FIXME: Replace "paFloat32" with whatever format you prefer -
         *  PaUtil_SelectClosestAvailableFormat is a bit faulty when working with multiple options */
        hostInputSampleFormat = PaUtil_SelectClosestAvailableFormat(
                paFloat32, inputSampleFormat);
        oboeStream->inputFormat = hostInputSampleFormat;
    } else {
        inputChannelCount = 0;
        inputSampleFormat = hostInputSampleFormat = paFloat32; /* Surpress 'uninitialised var' warnings. */
        oboeStream->inputFormat = hostInputSampleFormat;
    }

    if (i_outputParameters) {
        outputChannelCount = i_outputParameters->channelCount;
        outputSampleFormat = i_outputParameters->sampleFormat;

        /* Oboe supports alternate device specification with API>=28, but here we reject the use of
            paUseHostApiSpecificDeviceSpecification and stick with the default. Devices can be set via
            PaOboe_SetSelectedDevice. */
        if (i_outputParameters->device == paUseHostApiSpecificDeviceSpecification)
            return paInvalidDevice;

        /* check that output device can support outputChannelCount */
        if (outputChannelCount >
            i_hostApi->deviceInfos[i_outputParameters->device]->maxOutputChannels)
            return paInvalidChannelCount;

        /* validate outputStreamInfo */
        if (i_outputParameters->hostApiSpecificStreamInfo) {
            androidOutputUsage =
                    ((PaOboeStreamInfo *) i_outputParameters->hostApiSpecificStreamInfo)->androidOutputUsage;
            if (androidOutputUsage != Usage::Media &&
                androidOutputUsage != Usage::Notification &&
                androidOutputUsage != Usage::NotificationEvent &&
                androidOutputUsage != Usage::NotificationRingtone &&
                androidOutputUsage != Usage::VoiceCommunication &&
                androidOutputUsage != Usage::VoiceCommunicationSignalling &&
                androidOutputUsage != Usage::Alarm &&
                androidOutputUsage != Usage::Game)
                return paIncompatibleHostApiSpecificStreamInfo;
        }
        /* FIXME: Replace "paFloat32" with whatever format you prefer -
                  PaUtil_SelectClosestAvailableFormat is a bit faulty when working with multiple options
         */
        hostOutputSampleFormat = PaUtil_SelectClosestAvailableFormat(
                paFloat32, m_outputSampleFormat);
        oboeStream->outputFormat = hostOutputSampleFormat;
    } else {
        outputChannelCount = 0;
        outputSampleFormat = hostOutputSampleFormat = paFloat32;
        oboeStream->outputFormat = hostOutputSampleFormat;
    }

    /* validate platform specific flags */
    if ((i_streamFlags & paPlatformSpecificFlags) != 0)
        return paInvalidFlag; /* unexpected platform specific flag */

    if (i_framesPerBuffer == paFramesPerBufferUnspecified) {
        if (i_outputParameters) {
            framesPerHostBuffer =
                    (unsigned long) (i_outputParameters->suggestedLatency * i_sampleRate);
        } else {
            framesPerHostBuffer =
                    (unsigned long) (i_inputParameters->suggestedLatency * i_sampleRate);
        }
    } else {
        framesPerHostBuffer = i_framesPerBuffer;
    }

    if (i_streamCallback) {
        PaUtil_InitializeStreamRepresentation(&(oboeStream->streamRepresentation),
                                              &oboeHostApi->callbackStreamInterface,
                                              i_streamCallback, i_userData);
    } else {
        PaUtil_InitializeStreamRepresentation(&(oboeStream->streamRepresentation),
                                              &oboeHostApi->blockingStreamInterface,
                                              i_streamCallback, i_userData);
    }

    PaUtil_InitializeCpuLoadMeasurer(&(oboeStream->cpuLoadMeasurer), i_sampleRate);

    error = PaUtil_InitializeBufferProcessor(&(oboeStream->bufferProcessor),
                                             inputChannelCount,
                                             inputSampleFormat,
                                             hostInputSampleFormat,
                                             outputChannelCount,
                                             outputSampleFormat,
                                             hostOutputSampleFormat,
                                             i_sampleRate, i_streamFlags,
                                             i_framesPerBuffer,
                                             framesPerHostBuffer,
                                             paUtilFixedHostBufferSize,
                                             i_streamCallback, i_userData);
    if (error != paNoError)
        goto error;

    oboeStream->streamRepresentation.streamInfo.sampleRate = i_sampleRate;
    oboeStream->isBlocking = (i_streamCallback == nullptr);
    oboeStream->framesPerHostCallback = framesPerHostBuffer;
    oboeStream->bytesPerFrame = sizeof(int16_t);
    oboeStream->cbFlags = 0;
    oboeStream->isStopped = true;
    oboeStream->isActive = false;

    if (!(oboeStream->isBlocking)) {}
//        PaUnixThreading_Initialize(); TODO: see if threading works with this version of PortAudio

    if (inputChannelCount > 0) {
        oboeStream->hasInput = true;
        oboeStream->streamRepresentation.streamInfo.inputLatency =
                ((PaTime) PaUtil_GetBufferProcessorInputLatencyFrames(
                        &(oboeStream->bufferProcessor)) +
                 oboeStream->framesPerHostCallback) / i_sampleRate;
        ENSURE(InitializeInputStream(oboeStream, oboeHostApi,
                                     androidInputPreset, i_sampleRate),
               "Initializing inputstream failed")
    } else { oboeStream->hasInput = false; }

    if (outputChannelCount > 0) {
        oboeStream->hasOutput = true;
        oboeStream->streamRepresentation.streamInfo.outputLatency =
                ((PaTime) PaUtil_GetBufferProcessorOutputLatencyFrames(
                        &oboeStream->bufferProcessor)
                 + oboeStream->framesPerHostCallback) / i_sampleRate;
        ENSURE(InitializeOutputStream(oboeStream, oboeHostApi,
                                      androidOutputUsage, i_sampleRate),
               "Initializing outputstream failed");
    } else { oboeStream->hasOutput = false; }

    *i_paStream = (PaStream *) oboeStream;
    return error;

    error:
    if (oboeStream)
        PaUtil_FreeMemory(oboeStream);

    LOGE("[PaOboe - OpenStream]\t Error opening stream(s). Error code: %d", error);

    return error;
}


/**
 * \brief   Calls OboeEngine::closeStream, and then frees the memory that was allocated to sustain
 *          the stream(s). When CloseStream() is called, the multi-api layer ensures that the stream
 *          has already been stopped or aborted.
 * @param   paStream points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  paNoError, but warns in the logs if OboeEngine::closeStream failed.
 */
static PaError CloseStream(PaStream *i_paStream) {
    auto *oboeStream = (OboeStream *) i_paStream;
    auto *oboeEngine = oboeStream->getEngineAddress();

    if (!(oboeEngine->closeStream(oboeStream)))
        LOGW("[PaOboe - CloseStream]\t Some errors have occurred in closing oboe streams - see OboeEngine::CloseStream logs.");

    PaUtil_TerminateBufferProcessor(&oboeStream->bufferProcessor);
    PaUtil_TerminateStreamRepresentation(&oboeStream->streamRepresentation);

    for (int i = 0; i < g_numberOfBuffers; ++i) {
        if (oboeStream->hasOutput)
            PaUtil_FreeMemory(oboeStream->outputBuffers[i]);
        if (oboeStream->hasInput)
            PaUtil_FreeMemory(oboeStream->inputBuffers[i]);
    }

    if (oboeStream->hasOutput)
        PaUtil_FreeMemory(oboeStream->outputBuffers);
    if (oboeStream->hasInput)
        PaUtil_FreeMemory(oboeStream->inputBuffers);

    PaUtil_FreeMemory(oboeStream);
    return paNoError;
}


/**
 * \brief   Allocates the memory of the buffers necessary to start a stream, both for output and
 *          input, then calls OboeEngine::startStream.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  paNoError if no errors occur, paUnanticipatedHostError if OboeEngine::startStream fails.
 */
static PaError StartStream(PaStream *i_paStream) {
    auto *oboeStream = (OboeStream *) i_paStream;
    auto *oboeEngine = oboeStream->getEngineAddress();

    PaUtil_ResetBufferProcessor(&oboeStream->bufferProcessor);

    //Checking if the stream(s) are already active.
    //TODO: check if it's working as expected (extensive testing needed, no problem spotted with situational tests)
    if (oboeStream->isActive) {
        LOGW("[PaOboe - StartStream]\t Stream was already active, stopping...");
        StopStream(i_paStream);
        LOGW("[PaOboe - StartStream]\t Restarting...");
        StartStream(i_paStream);
    }

    oboeStream->currentOutputBuffer = 0;
    oboeStream->currentInputBuffer = 0;

    /* Initialize buffers */
    for (int i = 0; i < g_numberOfBuffers; ++i) {
        if (oboeStream->hasOutput) {
            memset(oboeStream->outputBuffers[oboeStream->currentOutputBuffer], 0,
                   oboeStream->framesPerHostCallback * oboeStream->bytesPerFrame *
                        oboeStream->bufferProcessor.outputChannelCount);
            oboeStream->currentOutputBuffer = (oboeStream->currentOutputBuffer + 1) % g_numberOfBuffers;
        }
        if (oboeStream->hasInput) {
            memset(oboeStream->inputBuffers[oboeStream->currentInputBuffer], 0,
                   oboeStream->framesPerHostCallback * oboeStream->bytesPerFrame *
                        oboeStream->bufferProcessor.inputChannelCount);
            oboeStream->currentInputBuffer = (oboeStream->currentInputBuffer + 1) % g_numberOfBuffers;
        }
    }

    if (!oboeStream->isBlocking) {
        oboeStream->callbackResult = paContinue;
        oboeStream->oboeCallbackResult = DataCallbackResult::Continue;
    }

    oboeStream->isStopped = false;
    oboeStream->isActive = true;
    oboeStream->doStop = false;
    oboeStream->doAbort = false;

    if (!(oboeEngine->startStream(oboeStream)))
        return paUnanticipatedHostError;
    else
        return paNoError;
}


/**
 * \brief   Ends the stream callback, if the stream is not blocking, and calls
 *          OboeEngine::stopStream.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  paNoError if no errors occur, paUnanticipatedHostError if OboeStream::stopStream fails.
 */
static PaError StopStream(PaStream *i_paStream) {
    PaError error = paNoError;
    auto *oboeStream = (OboeStream *) i_paStream;
    auto *oboeEngine = oboeStream->getEngineAddress();

    if (oboeStream->isStopped) {
        LOGW("[PaOboe - StopStream]\t Stream was already stopped.");
    } else {
        if (!(oboeStream->isBlocking)) {
            oboeStream->doStop = true;
        }
        if (!(oboeEngine->stopStream(oboeStream))) {
            LOGE("[PaOboe - StopStream]\t Couldn't stop the stream(s) correctly - see OboeEngine::StopStream logs.");
            error = paUnanticipatedHostError;
        }

        oboeStream->isActive = false;
        oboeStream->isStopped = true;
        if (oboeStream->streamRepresentation.streamFinishedCallback != nullptr)
            oboeStream->streamRepresentation.streamFinishedCallback(
                    oboeStream->streamRepresentation.userData);
    }

    return error;
}


/**
 * \brief   Aborts the stream callback, if the stream is not blocking, and calls
 *          OboeEngine::abortStream.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  paNoError if no errors occur, paUnanticipatedHostError if OboeStream::abortStream fails.
 */
static PaError AbortStream(PaStream *i_paStream) {
    PaError error = paNoError;
    auto *oboeStream = (OboeStream *) i_paStream;
    auto *oboeEngine = oboeStream->getEngineAddress();
    LOGI("[PaOboe - AbortStream]\t Aborting stream.");

    if (!oboeStream->isBlocking) {
        oboeStream->doAbort = true;
    }

    /* stop immediately so enqueue has no effect */
    if (!(oboeEngine->abortStream(oboeStream))) {
        LOGE("[PaOboe - AbortStream]\t Couldn't abort the stream - see OboeEngine::abortStream logs.");
        error = paUnanticipatedHostError;
    }

    oboeStream->isActive = false;
    oboeStream->isStopped = true;
    if (oboeStream->streamRepresentation.streamFinishedCallback != nullptr)
        oboeStream->streamRepresentation.streamFinishedCallback(
                oboeStream->streamRepresentation.userData);

    return error;
}


/**
 * \brief   Copies an input stream buffer by buffer, and calls OboeEngine::readStream.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream;
 * @param   buffer is the address of the first sample of the buffer;
 * @param   frames is the total number of frames to read.
 * @return  paInternalError if OboeEngine::readStream fails, paNoError otherwise.
 */
static PaError ReadStream(PaStream *i_paStream, void *i_buffer, unsigned long i_frames) {
    auto *oboeStream = (OboeStream *) i_paStream;
    auto *oboeEngine = oboeStream->getEngineAddress();
    void *userBuffer = i_buffer;
    unsigned framesToRead;
    PaError error = paNoError;

    while (i_frames > 0) {
        framesToRead = PA_MIN(oboeStream->framesPerHostCallback, i_frames);

        if (!(oboeEngine->readStream(oboeStream, userBuffer, framesToRead *
                                                 oboeStream->bufferProcessor.inputChannelCount)))
            error = paInternalError;

        oboeStream->currentInputBuffer = (oboeStream->currentInputBuffer + 1) % g_numberOfBuffers;
        i_frames -= framesToRead;
    }

    return error;
}


/**
 * \brief   Copies an output stream buffer by buffer, and calls OboeEngine::writeStream.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream;
 * @param   buffer is the address of the first sample of the buffer;
 * @param   frames is the total number of frames to write.
 * @return  paInternalError if OboeEngine::writeStream fails, paNoError otherwise.
 */
static PaError WriteStream(PaStream *i_paStream, const void *i_buffer, unsigned long i_frames) {
    auto *oboeStream = (OboeStream *) i_paStream;
    auto *oboeEngine = oboeStream->getEngineAddress();
    const void *userBuffer = i_buffer;
    unsigned framesToWrite;
    PaError error = paNoError;

    while (i_frames > 0) {
        framesToWrite = PA_MIN(stream->framesPerHostCallback, i_frames);

        if (!(oboeEngine->writeStream(oboeStream, userBuffer, framesToWrite *
                                                  oboeStream->bufferProcessor.outputChannelCount)))
            error = paInternalError;

        oboeStream->currentOutputBuffer = (oboeStream->currentOutputBuffer + 1) % g_numberOfBuffers;
        i_frames -= framesToWrite;
    }

    return error;
}


/*-------------------------------- PaSkeleton Secondary Functions --------------------------------*/

/**
 * \brief   Function needed by portaudio to understand how many frames can be read without waiting.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  the minimum number of frames that can be read without waiting.
 */
static signed long GetStreamReadAvailable(PaStream *i_paStream) {
    auto *oboeStream = (OboeStream *) i_paStream;
    return oboeStream->framesPerHostCallback * (g_numberOfBuffers - oboeStream->currentInputBuffer);
}


/**
 * \brief   Function needed by portaudio to understand how many frames can be written without waiting.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  the minimum number of frames that can be written without waiting.
 */
static signed long GetStreamWriteAvailable(PaStream *i_paStream) {
    auto *oboeStream = (OboeStream *) i_paStream;
    return oboeStream->framesPerHostCallback * (g_numberOfBuffers - oboeStream->currentOutputBuffer);
}


/**
 * \brief   Function needed by portaudio to understand if the stream is stopped.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  one (1) when the stream is stopped, or zero (0) when the stream is running.
 */
static PaError IsStreamStopped(PaStream *i_paStream) {
    auto *oboeStream = (OboeStream *) i_paStream;
    return oboeStream->isStopped;
}


/**
 * \brief   Function needed by portaudio to understand if the stream is active.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  one (1) when the stream is active (ie playing or recording audio), or zero (0) otherwise.
 */
static PaError IsStreamActive(PaStream *i_paStream) {
    auto *oboeStream = (OboeStream *) i_paStream;
    return oboeStream->isActive;
}


/**
 * \brief   Function needed by portaudio to get the stream time in seconds.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  The stream's current time in seconds, or 0 if an error occurred.
 */
static PaTime GetStreamTime(PaStream *i_paStream) {
    return PaUtil_GetTime();
}


/**
 * \brief   Function needed by portaudio to retrieve CPU usage information for the specified stream.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  A floating point value, typically between 0.0 and 1.0, where 1.0 indicates that the
 *          stream callback is consuming the maximum number of CPU cycles possible to maintain
 *          real-time operation. A value of 0.5 would imply that PortAudio and the stream callback
 *          was consuming roughly 50% of the available CPU time. The return value may exceed 1.0.
 *          A value of 0.0 will always be returned for a blocking read/write stream, or if an error
 *          occurs.
 */
static double GetStreamCpuLoad(PaStream *i_paStream) {
    auto *oboeStream = (OboeStream *) i_paStream;
    return PaUtil_GetCpuLoad(&oboeStream->cpuLoadMeasurer);
}


/*----------------------------------- PaOboe Utility Functions -----------------------------------*/

/**
 * \brief   In case that no buffer size was specifically set via PaOboe_setNativeBufferSize, this
 *          function is called to get a sensible value for the buffer size.
 * @return  256 for Android API Level <= 23, 192 otherwise.
 */
static unsigned long GetApproximateLowBufferSize() {
    if (__ANDROID_API__ <= 23)
        return 256;
    else
        return 192;
}


/*----------------------------- Implementation of PaOboe.h functions -----------------------------*/

void PaOboe_SetSelectedDevice(Direction i_direction, int32_t i_deviceID) {
    LOGI("[PaOboe - SetSelectedDevice] Selecting device...");
    if (i_direction == Direction::Input)
        g_inputDeviceId = i_deviceID;
    else
        g_outputDeviceId = i_deviceID;
}


void PaOboe_SetPerformanceMode(oboe::Direction i_direction, oboe::PerformanceMode i_performanceMode) {
    if (i_direction == Direction::Input) {
        g_inputPerfMode = i_performanceMode;
    } else {
        g_outputPerfMode = i_performanceMode;
    }
}


void PaOboe_SetNativeBufferSize(unsigned long i_bufferSize) {
    g_nativeBufferSize = i_bufferSize;
}


void PaOboe_SetNumberOfBuffers(unsigned i_numberOfBuffers) {
    g_numberOfBuffers = i_numberOfBuffers;
}
