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
#pragma ide diagnostic ignored "cppcoreguidelines-narrowing-conversions"

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

//TODO: PaOboe_Patch: PaUtil_AllocateZeroInitializedMemory replaces PaUtil_AllocateMemory (same with GoupAllocations)

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
static unsigned long nativeBufferSize = 0;
static unsigned numberOfBuffers = 2;

using namespace oboe;

int32_t inputDeviceId = kUnspecified;
int32_t outputDeviceId = kUnspecified;

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

    // Buffers are managed by the callback function in Oboe.
    void **outputBuffers;
    int currentOutputBuffer;
    void **inputBuffers;
    int currentInputBuffer;

    long engineAddress;
    unsigned long framesPerHostCallback;
    unsigned bytesPerFrame;
} OboeStream;


/**
 * Stream engine of the host API - Oboe. We allocate only one instance of the engine, and
 * we call its functions when we want to operate directly on Oboe. More infos on each functions are
 * provided right before their implementations.
 */
class OboeEngine : public AudioStreamCallback {
public:
    OboeEngine();

    //Stream-managing functions
    bool tryStream(Direction direction, int32_t sampleRate, int32_t channelCount);
    PaError openStream(Direction direction, int32_t sampleRate,
                       Usage outputUsage, InputPreset inputPreset);
    bool startStream();
    bool stopStream();
    bool restartStream(int direction);
    bool closeStream();
    bool abortStream();

    //Callback function for non-blocking streams and some callback utils
    DataCallbackResult onAudioReady(AudioStream *audioStream, void *audioData,
                                    int32_t numFrames) override;
    void onErrorAfterClose(AudioStream *audioStream, oboe::Result error) override;
    void resetCallbackCounters();

    //Blocking read/write functions
    bool writeStream(const void *buffer, int32_t framesToWrite);
    bool readStream(void *buffer, int32_t framesToRead);

    //Engine utils
    OboeStream* initializeOboeStream();
    void setEngineAddress(long address);

private:
    //The only instance of OboeStream that will be used
    OboeStream *oboeStream;

    //The only instances of output and input streams that will be used, and their builders
    std::shared_ptr<AudioStream> outputStream;
    AudioStreamBuilder outputBuilder;
    std::shared_ptr<AudioStream> inputStream;
    AudioStreamBuilder inputBuilder;

    //callback utils
    unsigned long framesProcessed{};
    PaStreamCallbackTimeInfo timeInfo{};
    struct timespec timeSpec{};

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
 * \brief   Initializes an instance of the engine.
 */
OboeEngine::OboeEngine() {
    oboeStream = nullptr;
}


/**
 * \brief   Tries to open a stream with the direction @direction, sample rate @sampleRate and/or
 *          channel count @channelCount. It then checks if the stream was in fact opened with the
 *          desired settings, and then closes the stream. It's used to see if the requested
 *          parameters are supported by the devices that are going to be used.
 * @param   direction the Direction of the stream;
 * @param   sampleRate the sample rate we want to try;
 * @param   channelCount the channel count we want to try;
 * @return  true if the requested sample rate / channel count is supported by the device, false if
 *          they aren't, or if tryStream couldn't open a stream.
 */
bool OboeEngine::tryStream(Direction direction, int32_t sampleRate, int32_t channelCount) {
    AudioStreamBuilder m_builder;
    Result m_result;
    bool m_outcome = false;

    m_builder.setDeviceId(getSelectedDevice(direction))
            // Arbitrary format usually broadly supported. Later, we'll open streams with correct formats.
            ->setFormat(AudioFormat::I16)
            ->setDirection(direction)
            ->setSampleRate(sampleRate)
            ->setChannelCount(channelCount);
    if (direction == Direction::Input) {
        m_result = m_builder.openStream(inputStream);
    } else {
        m_result = m_builder.openStream(outputStream);
    }

    if (m_result != Result::OK) {
        LOGE("[OboeEngine::TryStream]\t Couldn't open the stream in TryStream. Error: %s",
             convertToText(m_result));
        return m_outcome;
    }

    if (sampleRate != kUnspecified) {
        m_outcome = (sampleRate == m_builder.getSampleRate());
        if(!m_outcome) {
            LOGW("[OboeEngine::TryStream]\t Tried sampleRate = %d, built sampleRate = %d",
                 sampleRate, m_builder.getSampleRate());
        }
    } else if (channelCount != kUnspecified) {
        m_outcome = (channelCount == m_builder.getChannelCount());
        if(!m_outcome) {
            LOGW("[OboeEngine::TryStream]\t Tried channelCount = %d, built channelCount = %d",
                 channelCount, m_builder.getChannelCount());
        }
    } else {
        LOGE("[OboeEngine::TryStream]\t Logical failure. This message should NEVER occur.");
        m_outcome = false;
    }

    if (direction == Direction::Input)
        inputStream->close();
    else
        outputStream->close();

    return m_outcome;
}


/**
 * \brief   Opens an audio stream of oboeStream with a specific direction, sample rate and,
 *          depending on the direction of the stream, sets its usage (if
 *          direction == Ditrction::Output) or its preset (if direction == Direction::Input).
 *          Moreover, this function checks if the stream is blocking, and sets its callback
 *          function if not.
 * @param   direction The Oboe::Direction of the stream we want to open;
 * @param   sampleRate The sample rate of the stream we want to open;
 * @param   androidOutputUsage The Oboe::Usage of the output stream we want to open
 *              (only matters with Android Api level >= 28);
 * @param   androidInputPreset The Preset of the input stream we want to open
 *              (only matters with Android Api level >= 28).
 * @return  paNoError if everything goes as expected, paUnanticipatedHostError if Oboe fails to open
 *          a stream, and paInsufficientMemory if the memory allocation of the buffers fails.
 */
PaError OboeEngine::openStream(Direction direction, int32_t sampleRate,
                               Usage androidOutputUsage, InputPreset androidInputPreset) {
    PaError m_error = paNoError;
    Result m_result;

    if (direction == Direction::Input) {
        inputBuilder.setChannelCount(oboeStream->bufferProcessor.inputChannelCount)
                ->setFormat(PaToOboeFormat(oboeStream->inputFormat))
                ->setSampleRate(sampleRate)
                ->setDirection(Direction::Input)
                ->setDeviceId(getSelectedDevice(Direction::Input))
                ->setInputPreset(androidInputPreset)
                ->setFramesPerCallback(oboeStream->framesPerHostCallback);

        if (!(oboeStream->isBlocking)) {
            resetCallbackCounters();
            inputBuilder.setDataCallback(this)
                    ->setPerformanceMode(PerformanceMode::LowLatency);
        }

        m_result = inputBuilder.openStream(inputStream);

        if (m_result != Result::OK) {
            LOGE("[OboeEngine::OpenStream]\t Oboe couldn't open the input stream: %s",
                 convertToText(m_result));
            m_error = paUnanticipatedHostError;
            return m_error;
        }

        inputStream->setBufferSizeInFrames(inputStream->getFramesPerBurst() * numberOfBuffers);
        oboeStream->inputBuffers =
                (void **) PaUtil_AllocateZeroInitializedMemory(numberOfBuffers * sizeof(int32_t *));

        for (int i = 0; i < numberOfBuffers; ++i) {
            oboeStream->inputBuffers[i] = (void *) PaUtil_AllocateZeroInitializedMemory(
                    oboeStream->framesPerHostCallback *
                    oboeStream->bytesPerFrame *
                    oboeStream->bufferProcessor.inputChannelCount);

            if (!oboeStream->inputBuffers[i]) {
                for (int j = 0; j < i; ++j)
                    PaUtil_FreeMemory(oboeStream->inputBuffers[j]);
                PaUtil_FreeMemory(oboeStream->inputBuffers);
                inputStream->close();
                m_error = paInsufficientMemory;
                break;
            }
        }
        oboeStream->currentInputBuffer = 0;
    } else {
        outputBuilder.setChannelCount(oboeStream->bufferProcessor.outputChannelCount)
                ->setFormat(PaToOboeFormat(oboeStream->outputFormat))
                ->setSampleRate(sampleRate)
                ->setDirection(Direction::Output)
                ->setDeviceId(getSelectedDevice(Direction::Output))
                ->setUsage(androidOutputUsage)
                ->setFramesPerCallback(oboeStream->framesPerHostCallback);

        if (!(oboeStream->isBlocking)) {
            resetCallbackCounters();
            outputBuilder.setDataCallback(this)
                    ->setPerformanceMode(PerformanceMode::LowLatency);
        }

        m_result = outputBuilder.openStream(outputStream);
        if (m_result != Result::OK) {
            LOGE("[OboeEngine::OpenStream]\t Oboe couldn't open the output stream: %s",
                 convertToText(m_result));
            m_error = paUnanticipatedHostError;
            return m_error;
        }

        outputStream->setBufferSizeInFrames(outputStream->getFramesPerBurst() * numberOfBuffers);
        oboeStream->outputBuffers =
                (void **) PaUtil_AllocateZeroInitializedMemory(numberOfBuffers * sizeof(int32_t *));

        for (int i = 0; i < numberOfBuffers; ++i) {
            oboeStream->outputBuffers[i] = (void *) PaUtil_AllocateZeroInitializedMemory(
                    oboeStream->framesPerHostCallback *
                    oboeStream->bytesPerFrame *
                    oboeStream->bufferProcessor.outputChannelCount);

            if (!oboeStream->outputBuffers[i]) {
                for (int j = 0; j < i; ++j)
                    PaUtil_FreeMemory(oboeStream->outputBuffers[j]);
                PaUtil_FreeMemory(oboeStream->outputBuffers);
                outputStream->close();
                m_error = paInsufficientMemory;
                break;
            }
        }
        oboeStream->currentOutputBuffer = 0;
    }

    return m_error;
}


/**
 * \brief   Starts oboeStream - both input and output audiostreams are checked
 *          and requested to be started.
 * @return  true if the streams we wanted to start are started successfully, false otherwise.
 */
bool OboeEngine::startStream() {
    Result m_outputResult = Result::OK, m_inputResult = Result::OK;

    if (oboeStream->hasInput) {
        m_inputResult = inputStream->requestStart();
        if (m_inputResult != Result::OK)
            LOGE("[OboeEngine::startStream]\t Oboe couldn't start the input stream: %s",
                 convertToText(m_inputResult));
    }
    if (oboeStream->hasOutput) {
        m_outputResult = outputStream->requestStart();
        if (m_outputResult != Result::OK)
            LOGE("[OboeEngine::startStream]\t Oboe couldn't start the output stream: %s",
                 convertToText(m_outputResult));
    }

    return (m_outputResult == Result::OK && m_inputResult == Result::OK);
}


/**
 * \brief   Stops oboeStream - both input and output audiostreams are checked
 *          and requested to be stopped.
 * @return  true if the streams we wanted to stop are stopped successfully, false otherwise.
 */
bool OboeEngine::stopStream() {
    Result m_outputResult = Result::OK, m_inputResult = Result::OK;

    if (oboeStream->hasInput) {
        m_inputResult = inputStream->requestStop();
        if (m_inputResult != Result::OK)
            LOGE("[OboeEngine::stopStream]\t Oboe couldn't stop the input stream: %s",
                 convertToText(m_inputResult));
    }
    if (oboeStream->hasOutput) {
        m_outputResult = outputStream->requestStop();
        if (m_outputResult != Result::OK)
            LOGE("[OboeEngine::stopStream]\t Oboe couldn't stop the output stream: %s",
                 convertToText(m_outputResult));
    }

    return (m_outputResult == Result::OK && m_inputResult == Result::OK);
}


/**
 * \brief   Called when it's needed to restart the oboeStream's audio stream(s), mainly when the
 *          audio devices change while a stream is started.
 * @return  true if the stream is restarted successfully, false otherwise.
 */
bool OboeEngine::restartStream(int direction) {
    bool m_outcome = true;
    Result m_result;

    //TODO: Test if KCTI crashes when ErrorDisconnected occurs
    switch (direction) {
        case 1: //output-only
            //stopping and closing
            m_result = outputStream->stop();
            if (m_result != Result::OK)
                LOGW("[OboeEngine::restartStream]\t Oboe couldn't stop the output stream: %s",
                     convertToText(m_result));
            m_result = outputStream->close();
            if (m_result != Result::OK)
                LOGW("[OboeEngine::restartStream]\t Oboe couldn't close the output stream: %s",
                     convertToText(m_result));

            //reopening and restarting
            m_result = outputBuilder.openStream(outputStream);
            if (m_result != Result::OK)
                LOGE("[OboeEngine::restartStream]\t Oboe couldn't reopen the output stream: %s",
                     convertToText(m_result));
            m_result = outputStream->start();
            if (m_result != Result::OK) {
                LOGE("[OboeEngine::restartStream]\t Oboe couldn't restart the output stream: %s",
                     convertToText(m_result));
                m_outcome = false;
            }
            break;

        case 2: //input-only
            //stopping and closing
            m_result = inputStream->stop();
            if (m_result != Result::OK)
                LOGW("[OboeEngine::restartStream]\t Oboe couldn't stop the input stream: %s",
                     convertToText(m_result));
            m_result = inputStream->close();
            if (m_result != Result::OK)
                LOGW("[OboeEngine::restartStream]\t Oboe couldn't close the input stream: %s",
                     convertToText(m_result));

            //reopening and restarting
            m_result = inputBuilder.openStream(inputStream);
            if (m_result != Result::OK)
                LOGE("[OboeEngine::restartStream]\t Oboe couldn't reopen the input stream: %s",
                     convertToText(m_result));
            m_result = inputStream->start();
            if (m_result != Result::OK) {
                LOGE("[OboeEngine::restartStream]\t Oboe couldn't restart the input stream: %s",
                     convertToText(m_result));
                m_outcome = false;
            }
            break;

        default:
            // unspecified direction or both directions, abort streams
            LOGW("[OboeEngine::restartStream]\t Unspecified direction, restarting both streams");
            m_outcome = (restartStream(1) && restartStream(2));
            break;
    }

    return m_outcome;
}


/**
 * \brief   Closes oboeStream - both input and output audiostreams are checked
 *          and closed if active.
 * @return  true if the stream is closed successfully, otherwise returns false.
 */
bool OboeEngine::closeStream() {
    Result m_outputResult = Result::OK, m_inputResult = Result::OK;

    if(oboeStream == nullptr){
        LOGE("[OboeEngine::closeStream]\t Tried to close a NULL stream. Exiting closeStream.");
        return false;
    }

    if (oboeStream->hasOutput) {
        m_outputResult = outputStream->close();
        if (m_outputResult == Result::ErrorClosed) {
            m_outputResult = Result::OK;
            LOGW("[OboeEngine::closeStream]\t Tried to close output stream, but was already closed.");
        }
    }
    if (oboeStream->hasInput) {
        m_inputResult = inputStream->close();
        if (m_inputResult == Result::ErrorClosed) {
            m_inputResult = Result::OK;
            LOGW("[OboeEngine::closeStream]\t Tried to close input stream, but was already closed.");
        }
    }

    return (m_outputResult == Result::OK && m_inputResult == Result::OK);
}


/**
 * \brief   Stops oboeStream - both input and output audiostreams are checked and forcefully stopped.
 * @return  true if the output stream and the input stream are stopped successfully, false otherwise.
 */
bool OboeEngine::abortStream() {
    Result m_outputResult = Result::OK, m_inputResult = Result::OK;

    if(oboeStream == nullptr){
        LOGE("[OboeEngine::abortStream]\t Tried to abort a NULL stream. Exiting abortStream.");
        return false;
    }

    if (oboeStream->hasInput) {
        m_inputResult = inputStream->stop();
        if (m_inputResult != Result::OK)
            LOGE("[OboeEngine::abortStream]\t Couldn't force the input stream to stop: %s",
                 convertToText(m_inputResult));
        m_inputResult = inputStream->close();
        if (m_inputResult != Result::OK)
            LOGE("[OboeEngine::abortStream]\t Couldn't force the input stream to close: %s",
                 convertToText(m_inputResult));
    }
    if (oboeStream->hasOutput) {
        m_outputResult = outputStream->stop();
        if (m_outputResult != Result::OK)
            LOGE("[OboeEngine::abortStream]\t Couldn't force the output stream to stop: %s",
                 convertToText(m_outputResult));
        m_outputResult = outputStream->close();
        if (m_outputResult != Result::OK)
            LOGE("[OboeEngine::abortStream]\t Couldn't force the output stream to close: %s",
                 convertToText(m_outputResult));
    }

    return (m_outputResult == Result::OK && m_inputResult == Result::OK);
}


/**
 * \brief   Oboe's callback routine. FIXME: implement onErrorAfterClose correctly
 */
DataCallbackResult
OboeEngine::onAudioReady(AudioStream *audioStream, void *audioData, int32_t numFrames) {

    clock_gettime(CLOCK_REALTIME, &timeSpec);
    timeInfo.currentTime = (PaTime) (timeSpec.tv_sec + (timeSpec.tv_nsec / 1000000000.0));
    timeInfo.outputBufferDacTime = (PaTime) (oboeStream->framesPerHostCallback
                                             /
                                             oboeStream->streamRepresentation.streamInfo.sampleRate
                                             + timeInfo.currentTime);
    timeInfo.inputBufferAdcTime = (PaTime) (oboeStream->framesPerHostCallback
                                            /
                                            oboeStream->streamRepresentation.streamInfo.sampleRate
                                            + timeInfo.currentTime);

    /* check if StopStream or AbortStream was called */
    if (oboeStream->doStop) {
        oboeStream->callbackResult = paComplete;
    } else if (oboeStream->doAbort) {
        oboeStream->callbackResult = paAbort;
    }

    PaUtil_BeginCpuLoadMeasurement(&oboeStream->cpuLoadMeasurer);
    PaUtil_BeginBufferProcessing(&oboeStream->bufferProcessor,
                                 &timeInfo, oboeStream->cbFlags);

    if (oboeStream->hasOutput) {
        oboeStream->outputBuffers[oboeStream->currentOutputBuffer] = audioData;
        PaUtil_SetOutputFrameCount(&oboeStream->bufferProcessor, numFrames);
        PaUtil_SetInterleavedOutputChannels(&oboeStream->bufferProcessor, 0,
                                            (void *) ((PaInt16 **) oboeStream->outputBuffers)[oboeStream->currentOutputBuffer],
                                            0);
    }
    if (oboeStream->hasInput) {
        audioData = oboeStream->inputBuffers[oboeStream->currentInputBuffer];
        PaUtil_SetInputFrameCount(&oboeStream->bufferProcessor, 0);
        PaUtil_SetInterleavedInputChannels(&oboeStream->bufferProcessor, 0,
                                           (void *) ((PaInt16 **) oboeStream->inputBuffers)[oboeStream->currentInputBuffer],
                                           0);
    }

    /* continue processing user buffers if cbresult is pacontinue or if cbresult is  pacomplete and userbuffers aren't empty yet  */
    if (oboeStream->callbackResult == paContinue
        || (oboeStream->callbackResult == paComplete
            && !PaUtil_IsBufferProcessorOutputEmpty(&oboeStream->bufferProcessor))) {
        framesProcessed = PaUtil_EndBufferProcessing(&oboeStream->bufferProcessor,
                                                     &oboeStream->callbackResult);
    }

    /* enqueue a buffer only when there are frames to be processed,
     * this will be 0 when paComplete + empty buffers or paAbort
     */
    if (framesProcessed > 0) {
        if (oboeStream->hasOutput) {
            oboeStream->currentOutputBuffer =
                    (oboeStream->currentOutputBuffer + 1) % numberOfBuffers;
        }
        if (oboeStream->hasInput) {
            oboeStream->currentInputBuffer = (oboeStream->currentInputBuffer + 1) % numberOfBuffers;
        }
    }

    PaUtil_EndCpuLoadMeasurement(&oboeStream->cpuLoadMeasurer, framesProcessed);

    /* StopStream was called */
    if (framesProcessed == 0 && oboeStream->doStop) {
        oboeStream->oboeCallbackResult = DataCallbackResult::Stop;
    }

        /* if AbortStream or StopStream weren't called, stop from the cb */
    else if (framesProcessed == 0 && !(oboeStream->doAbort || oboeStream->doStop)) {
        oboeStream->isActive = false;
        oboeStream->isStopped = true;
        if (oboeStream->streamRepresentation.streamFinishedCallback != nullptr)
            oboeStream->streamRepresentation.streamFinishedCallback(
                    oboeStream->streamRepresentation.userData);
        //oboeStream->oboeCallbackResult = DataCallbackResult::Stop; TODO: Resume this test (onAudioReady)
    }

    return oboeStream->oboeCallbackResult;
}


/**
 * \brief   If the data callback ends without returning DataCallbackResult::Stop, this routine tells
 *          what error occurred.
 */
void OboeEngine::onErrorAfterClose(AudioStream *audioStream, Result error) {
    if (error == oboe::Result::ErrorDisconnected) {
        LOGW("[OboeEngine::onErrorAfterClose]\t ErrorDisconnected - Restarting stream(s)");
        if (!restartStream(0))
            LOGE("[OboeEngine::onErrorAfterClose]\t Couldn't restart stream(s)");
    }
    else
        LOGE("[OboeEngine::onErrorAfterClose]\t Error was %s", oboe::convertToText(error));
}


/**
 * \brief   Resets callback counters (called at the start of each iteration of onAudioReady
 */
void OboeEngine::resetCallbackCounters() {
    framesProcessed = 0;
    timeInfo = {0, 0, 0};
}


/**
 * \brief   Writes frames on the output stream of oboeStream. Used by blocking streams.
 * @param   buffer The buffer that we want to write on the output stream;
 * @param   framesToWrite The number of frames that we want to write.
 * @return  true if the buffer is written correctly, false if the write function returns an error
 *          different from ErrorDisconnected. In case of ErrorDisconnected, the function returns
 *          true if the stream is successfully restarted, and false otherwise.
 */
bool OboeEngine::writeStream(const void *buffer, int32_t framesToWrite) {
    bool m_outcome = true;

    ResultWithValue<int32_t> m_result = outputStream->write(buffer, framesToWrite, TIMEOUT_NS);

    // If the stream is interrupted because the device suddenly changes, restart the stream.
    if (m_result.error() == Result::ErrorDisconnected) {
        if (OboeEngine::restartStream(1))
            return true;
    }

    if (!m_result) {
        LOGE("[OboeEngine::writeStream]\t Error writing stream: %s", convertToText(m_result.error()));
        m_outcome = false;
    }
    return m_outcome;
}


/**
 * \brief   Reads frames from the input stream of oboeStream. Used by blocking streams.
 * @param   buffer The buffer that we want to read from the input stream;
 * @param   framesToWrite The number of frames that we want to read.
 * @return  true if the buffer is read correctly, false if the read function returns an error
 *          different from ErrorDisconnected. In case of ErrorDisconnected, the function returns
 *          true if the stream is successfully restarted, and false otherwise.
 */
bool OboeEngine::readStream(void *buffer, int32_t framesToRead) {
    bool m_outcome = true;

    ResultWithValue<int32_t> m_result = inputStream->read(buffer, framesToRead, TIMEOUT_NS);

    // If the stream is interrupted because the device suddenly changes, restart the stream.
    if (m_result.error() == Result::ErrorDisconnected) {
        if (OboeEngine::restartStream(2))
            return true;
    }

    if (!m_result) {
        LOGE("[OboeEngine::readStream]\t Error reading stream: %s", convertToText(m_result.error()));
        m_outcome = false;
    }
    return m_outcome;
}


/**
 * \brief   Allocates the memory of oboeStream.
 * @return  the address of the oboeStream.
 */
OboeStream* OboeEngine::initializeOboeStream() {
    oboeStream = (OboeStream *) PaUtil_AllocateZeroInitializedMemory(sizeof(OboeStream));
    return oboeStream;
}


/**
 * \brief   Sets the engineAddress parameter of oboeStream, useful for recalling the engine whenever
 *          it's needed.
 * @param   address the address of the only instance of OboeEngine.
 */
void OboeEngine::setEngineAddress(long address) {
    oboeStream->engineAddress = address;
}

/**
 * \brief   Converts PaSampleFormat values into Oboe::AudioFormat values.
 * @param   paFormat the PaSampleFormat we want to convert.
 * @return  the converted AudioFormat.
 */
AudioFormat OboeEngine::PaToOboeFormat(PaSampleFormat paFormat) {
    AudioFormat m_oboeFormat;
    switch (paFormat) {
        case paFloat32:
            m_oboeFormat = AudioFormat::Float;
            LOGI("[OboeEngine::PaToOboeFormat]\t REQUESTED OBOE FORMAT: FLOAT");
            break;
        case paInt16:
            m_oboeFormat = AudioFormat::I16;
            LOGI("[OboeEngine::PaToOboeFormat]\t REQUESTED OBOE FORMAT: I16");
            break;
        case paInt32:
            m_oboeFormat = AudioFormat::I32;
            LOGI("[OboeEngine::PaToOboeFormat]\t REQUESTED OBOE FORMAT: I32");
            break;
        case paInt24:
            m_oboeFormat = AudioFormat::I24;
            LOGI("[OboeEngine::PaToOboeFormat]\t REQUESTED OBOE FORMAT: I24");
            break;
        default:
            m_oboeFormat = AudioFormat::Unspecified;
            LOGW("[OboeEngine::PaToOboeFormat]\t Setting AudioFormat to Unspecified, because Oboe does not support the requested format.");
            break;
    }
    return m_oboeFormat;
}


/**
 * \brief   Function used to implement device selection. Device Ids are kUnspecified by default, but
 *          can be set to something different via JNI using the function PaOboe_SetSelectedDevice.
 * @param   direction the Oboe::Direction for which we want to know the device Id.
 * @return  the device Id of the appropriate direction.
 */
int32_t OboeEngine::getSelectedDevice(Direction direction) {
    if (direction == Direction::Input)
        return inputDeviceId;
    else
        return outputDeviceId;
}



/*----------------------------- PaSkeleton functions implementations -----------------------------*/

/**
 * \brief   Checks if the requested sample rate is supported by the output device using
 *          OboeEngine::tryStream.
 *          This function is used by PaOboe_Initialize, IsFormatSupported, and OpenStream.
 * @param   oboeHostApi points towards a OboeHostApiRepresentation (see struct defined at the top of
 *              this file);
 * @param   sampleRate is the sample rate we want to check.
 * @return  PaNoError regardless of the outcome of the check, but warns in the Logs if the sample
 *          rate was changed by Oboe.
 */
PaError IsOutputSampleRateSupported(PaOboeHostApiRepresentation *oboeHostApi, double sampleRate) {
    if (!(oboeHostApi->oboeEngine->tryStream(Direction::Output,
                                             sampleRate,
                                             kUnspecified)))
        LOGW("[PaOboe - IsOutputSampleRateSupported]\t Sample Rate was changed by Oboe. The device might not support high frequencies.");

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
PaError IsInputSampleRateSupported(PaOboeHostApiRepresentation *oboeHostApi, double sampleRate) {
    if (!(oboeHostApi->oboeEngine->tryStream(Direction::Input,
                                             sampleRate,
                                             kUnspecified)))
        LOGW("[PaOboe - IsInputSampleRateSupported]\t Sample Rate was changed by Oboe. The device might not support high frequencies.");

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
static PaError IsOutputChannelCountSupported(
        PaOboeHostApiRepresentation *oboeHostApi,
        int32_t numOfChannels) {
    if (numOfChannels > 2 || numOfChannels == 0) {
        LOGE("[PaOboe - IsOutputChannelCountSupported]\t Invalid channel count.");
        return paInvalidChannelCount;
    }

    if (!(oboeHostApi->oboeEngine->tryStream(Direction::Output,
                                             kUnspecified,
                                             numOfChannels)))
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
static PaError IsInputChannelCountSupported(
        PaOboeHostApiRepresentation *oboeHostApi,
        int32_t numOfChannels) {
    if (numOfChannels > 2 || numOfChannels == 0) {
        LOGE("[PaOboe - IsInputChannelCountSupported]\t Invalid channel count.");
        return paInvalidChannelCount;
    }

    if (!(oboeHostApi->oboeEngine->tryStream(Direction::Input,
                                             kUnspecified,
                                             numOfChannels)))
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
PaError PaOboe_Initialize(PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex) {
    PaError m_result = paNoError;
    int m_deviceCount;
    PaOboeHostApiRepresentation *m_oboeHostApi;
    PaDeviceInfo *m_deviceInfoArray;
    char *m_deviceName;

    m_oboeHostApi = (PaOboeHostApiRepresentation *) PaUtil_AllocateZeroInitializedMemory(
            sizeof(PaOboeHostApiRepresentation));
    if (!m_oboeHostApi) {
        m_result = paInsufficientMemory;
        goto error;
    }

    m_oboeHostApi->oboeEngine = new OboeEngine();

    m_oboeHostApi->allocations = PaUtil_CreateAllocationGroup();
    if (!m_oboeHostApi->allocations) {
        m_result = paInsufficientMemory;
        goto error;
    }

    *hostApi = &m_oboeHostApi->inheritedHostApiRep;
    // Initialization of infos.
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paInDevelopment;
    (*hostApi)->info.name = "android Oboe";
    (*hostApi)->info.defaultOutputDevice = 0;
    (*hostApi)->info.defaultInputDevice = 0;
    (*hostApi)->info.deviceCount = 0;


    m_deviceCount = 1;
    (*hostApi)->deviceInfos = (PaDeviceInfo **) PaUtil_GroupAllocateZeroInitializedMemory(
            m_oboeHostApi->allocations, sizeof(PaDeviceInfo *) * m_deviceCount);

    if (!(*hostApi)->deviceInfos) {
        m_result = paInsufficientMemory;
        goto error;
    }

    /* allocate all device info structs in a contiguous block */
    m_deviceInfoArray = (PaDeviceInfo *) PaUtil_GroupAllocateZeroInitializedMemory(
            m_oboeHostApi->allocations, sizeof(PaDeviceInfo) * m_deviceCount);
    if (!m_deviceInfoArray) {
        m_result = paInsufficientMemory;
        goto error;
    }

    for (int i = 0; i < m_deviceCount; ++i) {
        PaDeviceInfo *m_deviceInfo = &m_deviceInfoArray[i];
        m_deviceInfo->structVersion = 2;
        m_deviceInfo->hostApi = hostApiIndex;

        /* OboeEngine will handle manual device selection through the use of
           PaOboe_SetSelectedDevice via a JNI interface that can be implemented.
           Portaudio doesn't need to know about this, so we just use a default device. */

        m_deviceInfo->name = "default";

        /* Try channels in order of preference - Stereo > Mono. */
        const int32_t m_channelsToTry[] = {2, 1};
        const int32_t m_channelsToTryLength = 2;

        m_deviceInfo->maxOutputChannels = 0;
        m_deviceInfo->maxInputChannels = 0;

        for (i = 0; i < m_channelsToTryLength; ++i) {
            if (IsOutputChannelCountSupported(m_oboeHostApi, m_channelsToTry[i]) == paNoError) {
                m_deviceInfo->maxOutputChannels = m_channelsToTry[i];
                break;
            }
        }
        for (i = 0; i < m_channelsToTryLength; ++i) {
            if (IsInputChannelCountSupported(m_oboeHostApi, m_channelsToTry[i]) == paNoError) {
                m_deviceInfo->maxInputChannels = m_channelsToTry[i];
                break;
            }
        }

        /* check sample rates in order of preference */
        const int32_t m_sampleRates[] = {48000, 44100, 32000, 24000, 16000};
        const int32_t m_numberOfSampleRates = 5;

        m_deviceInfo->defaultSampleRate = m_sampleRates[0];

        for (i = 0; i < m_numberOfSampleRates; ++i) {
            if (IsOutputSampleRateSupported(
                    m_oboeHostApi, m_sampleRates[i]) == paNoError &&
                IsInputSampleRateSupported(
                        m_oboeHostApi, m_sampleRates[i]) == paNoError) {
                m_deviceInfo->defaultSampleRate = m_sampleRates[i];
                break;
            }
        }
        if (m_deviceInfo->defaultSampleRate == 0)
            goto error;

        /* If the user has set nativeBufferSize by querying the optimal buffer size via java,
           use the user-defined value since that will offer the lowest possible latency. */

        if (nativeBufferSize != 0) {
            m_deviceInfo->defaultLowInputLatency =
                    (double) nativeBufferSize / m_deviceInfo->defaultSampleRate;
            m_deviceInfo->defaultLowOutputLatency =
                    (double) nativeBufferSize / m_deviceInfo->defaultSampleRate;
            m_deviceInfo->defaultHighInputLatency =
                    (double) nativeBufferSize * 4 / m_deviceInfo->defaultSampleRate;
            m_deviceInfo->defaultHighOutputLatency =
                    (double) nativeBufferSize * 4 / m_deviceInfo->defaultSampleRate;
        } else {
            m_deviceInfo->defaultLowInputLatency =
                    (double) GetApproximateLowBufferSize() / m_deviceInfo->defaultSampleRate;
            m_deviceInfo->defaultLowOutputLatency =
                    (double) GetApproximateLowBufferSize() / m_deviceInfo->defaultSampleRate;
            m_deviceInfo->defaultHighInputLatency =
                    (double) GetApproximateLowBufferSize() * 4 / m_deviceInfo->defaultSampleRate;
            m_deviceInfo->defaultHighOutputLatency =
                    (double) GetApproximateLowBufferSize() * 4 / m_deviceInfo->defaultSampleRate;
        }

        (*hostApi)->deviceInfos[i] = m_deviceInfo;
        ++(*hostApi)->info.deviceCount;
    }

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface(&m_oboeHostApi->callbackStreamInterface,
                                     CloseStream, StartStream, StopStream,
                                     AbortStream, IsStreamStopped,
                                     IsStreamActive, GetStreamTime,
                                     GetStreamCpuLoad, PaUtil_DummyRead,
                                     PaUtil_DummyWrite,
                                     PaUtil_DummyGetReadAvailable,
                                     PaUtil_DummyGetWriteAvailable);

    PaUtil_InitializeStreamInterface(&m_oboeHostApi->blockingStreamInterface,
                                     CloseStream, StartStream, StopStream,
                                     AbortStream, IsStreamStopped,
                                     IsStreamActive, GetStreamTime,
                                     PaUtil_DummyGetCpuLoad, ReadStream,
                                     WriteStream, GetStreamReadAvailable,
                                     GetStreamWriteAvailable);

    if (m_result == paNoError)
        LOGV("[PaOboe - Initialize]\t Oboe host API successfully initialized");
    else
        LOGE("[PaOboe - Initialize]\t An unusual error occurred. Error code: %d", m_result);
    return m_result;

    error:
    if (m_oboeHostApi) {
        if (m_oboeHostApi->allocations) {
            PaUtil_FreeAllAllocations(m_oboeHostApi->allocations);
            PaUtil_DestroyAllocationGroup(m_oboeHostApi->allocations);
        }

        PaUtil_FreeMemory(m_oboeHostApi);
    }
    LOGE("[PaOboe - Initialize]\t Initialization failed. Error code: %d", m_result);
    return m_result;
}


/**
 * \brief   Interrupts the stream and frees the memory that was allocated to sustain the stream.
 * @param   hostApi points towards a *HostApiRepresentation, which is a structure representing the
 *              interface to a host API (see struct in "pa_hostapi.h").
 */
static void Terminate(struct PaUtilHostApiRepresentation *hostApi) {
    auto *m_oboeHostApi = (PaOboeHostApiRepresentation *) hostApi;

    if (!(m_oboeHostApi->oboeEngine->closeStream()))
        LOGW("[PaOboe - Terminate]\t Couldn't close the streams correctly - see OboeEngine::CloseStream logs.");

    if(m_oboeHostApi->oboeEngine != nullptr)
        delete m_oboeHostApi->oboeEngine;

    if (m_oboeHostApi->allocations) {
        PaUtil_FreeAllAllocations(m_oboeHostApi->allocations);
        PaUtil_DestroyAllocationGroup(m_oboeHostApi->allocations);
    }

    PaUtil_FreeMemory(m_oboeHostApi);
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
static PaError IsFormatSupported(struct PaUtilHostApiRepresentation *hostApi,
                                 const PaStreamParameters *inputParameters,
                                 const PaStreamParameters *outputParameters,
                                 double sampleRate) {
    PaError m_outcome;
    int m_inputChannelCount, m_outputChannelCount;
    PaSampleFormat m_inputSampleFormat, m_outputSampleFormat;
    auto *m_oboeHostApi = (PaOboeHostApiRepresentation *) hostApi;

    if (inputParameters) {
        m_inputChannelCount = inputParameters->channelCount;
        m_inputSampleFormat = inputParameters->sampleFormat;

        /* all standard sample formats are supported by the buffer adapter,
            this implementation doesn't support any custom sample formats */
        if (m_inputSampleFormat & paCustomFormat) {
            m_outcome = paSampleFormatNotSupported;
            return m_outcome;
        }

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */
        if (inputParameters->device == paUseHostApiSpecificDeviceSpecification) {
            m_outcome = paInvalidDevice;
            return m_outcome;
        }

        /* check that input device can support inputChannelCount */
        if (m_inputChannelCount >
            hostApi->deviceInfos[inputParameters->device]->maxInputChannels) {
            m_outcome = paInvalidChannelCount;
            return m_outcome;
        }

        /* validate inputStreamInfo */
        if (inputParameters->hostApiSpecificStreamInfo) {
            // Only has an effect on ANDROID_API>=28.
            InputPreset m_androidRecordingPreset =
                    ((PaOboeStreamInfo *) outputParameters->hostApiSpecificStreamInfo)->androidInputPreset;
            if (m_androidRecordingPreset != InputPreset::Generic &&
                m_androidRecordingPreset != InputPreset::Camcorder &&
                m_androidRecordingPreset != InputPreset::VoiceRecognition &&
                m_androidRecordingPreset != InputPreset::VoiceCommunication
                // Should I add compatibility with VoicePerformance?
                    ) {
                m_outcome = paIncompatibleHostApiSpecificStreamInfo;
                return m_outcome;
            }
        }
    } else {
        m_inputChannelCount = 0;
    }

    if (outputParameters) {
        m_outputChannelCount = outputParameters->channelCount;
        m_outputSampleFormat = outputParameters->sampleFormat;

        /* all standard sample formats are supported by the buffer adapter,
            this implementation doesn't support any custom sample formats */
        if (m_outputSampleFormat & paCustomFormat) {
            m_outcome = paSampleFormatNotSupported;
            return m_outcome;
        }

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */
        if (outputParameters->device == paUseHostApiSpecificDeviceSpecification) {
            m_outcome = paInvalidDevice;
            return m_outcome;
        }

        /* check that output device can support outputChannelCount */
        if (m_outputChannelCount >
            hostApi->deviceInfos[outputParameters->device]->maxOutputChannels) {
            m_outcome = paInvalidChannelCount;
            return m_outcome;
        }

        /* validate outputStreamInfo */
        if (outputParameters->hostApiSpecificStreamInfo) {
            // Only has an effect on ANDROID_API>=28.
            Usage m_androidOutputUsage =
                    ((PaOboeStreamInfo *) outputParameters->hostApiSpecificStreamInfo)->androidOutputUsage;
            if (m_androidOutputUsage != Usage::Media &&
                m_androidOutputUsage != Usage::Notification &&
                m_androidOutputUsage != Usage::NotificationEvent &&
                m_androidOutputUsage != Usage::NotificationRingtone &&
                m_androidOutputUsage != Usage::VoiceCommunication &&
                m_androidOutputUsage != Usage::VoiceCommunicationSignalling &&
                m_androidOutputUsage != Usage::Alarm
                // See if more are needed.
                    ) {
                m_outcome = paIncompatibleHostApiSpecificStreamInfo;
                return m_outcome;
            }
        }
    } else {
        m_outputChannelCount = 0;
    }

    if (m_outputChannelCount > 0) {
        if (IsOutputSampleRateSupported(m_oboeHostApi, sampleRate) != paNoError) {
            m_outcome = paInvalidSampleRate;
            return m_outcome;
        }
    }
    if (m_inputChannelCount > 0) {
        if (IsInputSampleRateSupported(m_oboeHostApi, sampleRate) != paNoError) {
            m_outcome = paInvalidSampleRate;
            return m_outcome;
        }
    }

    return paFormatIsSupported;
}


/**
 * \brief   Calls OboeEngine::openStream to open the outputStream and a Generic input preset.
 * @param   oboeHostApi points towards a OboeHostApiRepresentation (see struct defined at the top of
 *              this file);
 * @param   androidOutputUsage is an attribute that expresses why we are opening the output stream.
 *              This information can be used by certain platforms to make more refined volume or
 *              routing decisions. It only has an effect on Android API 28+.
 * @param   sampleRate is the sample rate we want for the audio stream we want to initialize. This is used to allocate
 *              the correct amount of memory.
 * @return  the value returned by OboeEngine::openStream.
 */
static PaError InitializeOutputStream(PaOboeHostApiRepresentation *oboeHostApi,
                                      Usage androidOutputUsage, double sampleRate) {

    return oboeHostApi->oboeEngine->openStream(Direction::Output,
                                               sampleRate,
                                               androidOutputUsage,
                                               Generic //Won't be used, so we put the default value.
    );
}


/**
 * \brief   Calls OboeEngine::openStream to open the outputStream and a Generic input preset.
 * @param   oboeHostApi points towards a OboeHostApiRepresentation (see struct defined at the top of
 *              this file);
 * @param   androidInputPreset is an attribute that defines the audio source. This information
 *              defines both a default physical source of audio signal, and a recording configuration.
 *              It only has an effect on Android API 28+.
 * @param   sampleRate is the sample rate we want for the audio stream we want to initialize. This is used to allocate
 *              the correct amount of memory.
 * @return  the value returned by OboeEngine::openStream.
 */
static PaError InitializeInputStream(PaOboeHostApiRepresentation *oboeHostApi,
                                     InputPreset androidInputPreset, double sampleRate) {

    return oboeHostApi->oboeEngine->openStream(Direction::Input,
                                               sampleRate,
                                               Usage::Media,   //Won't be used, so we put the default value.
                                               androidInputPreset
    );
}


/**
 * \brief   Opens the portaudio audio stream - while initializing our OboeStream.
 * @param   hostApi points towards a *HostApiRepresentation, which is a structure representing the
 *              interface to a host API (see struct in "pa_hostapi.h");
 * @param   s points to a pointer to a PaStream, which is an audio stream structure used and built
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
static PaError OpenStream(struct PaUtilHostApiRepresentation *hostApi,
                          PaStream **s,
                          const PaStreamParameters *inputParameters,
                          const PaStreamParameters *outputParameters,
                          double sampleRate,
                          unsigned long framesPerBuffer,
                          PaStreamFlags streamFlags,
                          PaStreamCallback *streamCallback,
                          void *userData) {
    PaError m_error = paNoError;
    auto m_oboeHostApi = (PaOboeHostApiRepresentation *) hostApi;
    unsigned long m_framesPerHostBuffer; /* these may not be equivalent for all implementations */
    int m_inputChannelCount, m_outputChannelCount;
    PaSampleFormat m_inputSampleFormat, m_outputSampleFormat;
    PaSampleFormat m_hostInputSampleFormat, m_hostOutputSampleFormat;

    Usage m_androidOutputUsage = Usage::VoiceCommunication;
    InputPreset m_androidInputPreset = InputPreset::Generic;

    OboeStream *m_oboeStream = m_oboeHostApi->oboeEngine->initializeOboeStream();

    if (!m_oboeStream) {
        m_error = paInsufficientMemory;
        goto error;
    }

    LOGI("[PaOboe - OpenStream]\t OpenStream called.");

    if (inputParameters) {
        m_inputChannelCount = inputParameters->channelCount;
        m_inputSampleFormat = inputParameters->sampleFormat;

        /* Oboe supports alternate device specification with API>=28, but for now we reject the use of
            paUseHostApiSpecificDeviceSpecification and stick with the default.*/
        if (inputParameters->device == paUseHostApiSpecificDeviceSpecification)
            return paInvalidDevice;

        /* check that input device can support inputChannelCount */
        if (m_inputChannelCount > hostApi->deviceInfos[inputParameters->device]->maxInputChannels)
            return paInvalidChannelCount;

        /* validate inputStreamInfo */
        if (inputParameters->hostApiSpecificStreamInfo) {
            // Only has an effect on ANDROID_API>=28.
            m_androidInputPreset =
                    ((PaOboeStreamInfo *) outputParameters->hostApiSpecificStreamInfo)->androidInputPreset;
            if (m_androidInputPreset != InputPreset::Generic &&
                m_androidInputPreset != InputPreset::Camcorder &&
                m_androidInputPreset != InputPreset::VoiceRecognition &&
                m_androidInputPreset != InputPreset::VoiceCommunication
                // Should I add compatibility with VoicePerformance?
                    )
                return paIncompatibleHostApiSpecificStreamInfo;
        }
    /* FIXME: Replace "paInt16" with whatever format you prefer -
     *  PaUtil_SelectClosestAvailableFormat is a bit faulty when working with multiple options */
        m_hostInputSampleFormat = PaUtil_SelectClosestAvailableFormat(
                paInt16, m_inputSampleFormat);
        m_oboeStream->inputFormat = m_hostInputSampleFormat;
    } else {
        m_inputChannelCount = 0;
        m_inputSampleFormat = m_hostInputSampleFormat = paInt16; /* Surpress 'uninitialised var' warnings. */
        m_oboeStream->inputFormat = m_hostInputSampleFormat;
    }

    if (outputParameters) {
        m_outputChannelCount = outputParameters->channelCount;
        m_outputSampleFormat = outputParameters->sampleFormat;

        /* Oboe supports alternate device specification with API>=28, but for now we reject the use of
            paUseHostApiSpecificDeviceSpecification and stick with the default.*/
        if (outputParameters->device == paUseHostApiSpecificDeviceSpecification)
            return paInvalidDevice;

        /* check that output device can support outputChannelCount */
        if (m_outputChannelCount >
            hostApi->deviceInfos[outputParameters->device]->maxOutputChannels)
            return paInvalidChannelCount;

        /* validate outputStreamInfo */
        if (outputParameters->hostApiSpecificStreamInfo) {
            m_androidOutputUsage =
                    ((PaOboeStreamInfo *) outputParameters->hostApiSpecificStreamInfo)->androidOutputUsage;
            if (m_androidOutputUsage != Usage::Media &&
                m_androidOutputUsage != Usage::Notification &&
                m_androidOutputUsage != Usage::NotificationEvent &&
                m_androidOutputUsage != Usage::NotificationRingtone &&
                m_androidOutputUsage != Usage::VoiceCommunication &&
                m_androidOutputUsage != Usage::VoiceCommunicationSignalling &&
                m_androidOutputUsage != Usage::Alarm
                // See if more are needed.
                    )
                return paIncompatibleHostApiSpecificStreamInfo;
        }
    /* FIXME: Replace "paInt16" with whatever format you prefer -
              PaUtil_SelectClosestAvailableFormat is a bit faulty when working with multiple options
     */
        m_hostOutputSampleFormat = PaUtil_SelectClosestAvailableFormat(
                paInt16, m_outputSampleFormat);
        m_oboeStream->outputFormat = m_hostOutputSampleFormat;
    } else {
        m_outputChannelCount = 0;
        m_outputSampleFormat = m_hostOutputSampleFormat = paInt16;
        m_oboeStream->outputFormat = m_hostOutputSampleFormat;
    }

    /* validate platform specific flags */
    if ((streamFlags & paPlatformSpecificFlags) != 0)
        return paInvalidFlag; /* unexpected platform specific flag */

    if (framesPerBuffer == paFramesPerBufferUnspecified) {
        if (outputParameters) {
            m_framesPerHostBuffer =
                    (unsigned long) (outputParameters->suggestedLatency * sampleRate);
        } else {
            m_framesPerHostBuffer =
                    (unsigned long) (inputParameters->suggestedLatency * sampleRate);
        }
    } else {
        m_framesPerHostBuffer = framesPerBuffer;
    }

    m_oboeHostApi->oboeEngine->setEngineAddress(
            reinterpret_cast<long>(m_oboeHostApi->oboeEngine));

    if (streamCallback) {
        PaUtil_InitializeStreamRepresentation(&(m_oboeStream->streamRepresentation),
                                              &m_oboeHostApi->callbackStreamInterface,
                                              streamCallback, userData);
    } else {
        PaUtil_InitializeStreamRepresentation(&(m_oboeStream->streamRepresentation),
                                              &m_oboeHostApi->blockingStreamInterface,
                                              streamCallback, userData);
    }

    PaUtil_InitializeCpuLoadMeasurer(&(m_oboeStream->cpuLoadMeasurer), sampleRate);

    m_error = PaUtil_InitializeBufferProcessor(&(m_oboeStream->bufferProcessor),
                                               m_inputChannelCount,
                                               m_inputSampleFormat,
                                               m_hostInputSampleFormat,
                                               m_outputChannelCount,
                                               m_outputSampleFormat,
                                               m_hostOutputSampleFormat,
                                               sampleRate, streamFlags,
                                               framesPerBuffer,
                                               m_framesPerHostBuffer,
                                               paUtilFixedHostBufferSize,
                                               streamCallback, userData);
    if (m_error != paNoError)
        goto error;

    m_oboeStream->streamRepresentation.streamInfo.sampleRate = sampleRate;
    m_oboeStream->isBlocking = (streamCallback == nullptr);
    m_oboeStream->framesPerHostCallback = m_framesPerHostBuffer;
    m_oboeStream->bytesPerFrame = sizeof(int16_t);
    m_oboeStream->cbFlags = 0;
    m_oboeStream->isStopped = true;
    m_oboeStream->isActive = false;

    if (!(m_oboeStream->isBlocking)) {}
//        PaUnixThreading_Initialize(); TODO: see if threading works with this version of PortAudio

    if (m_inputChannelCount > 0) {
        m_oboeStream->hasInput = true;
        m_oboeStream->streamRepresentation.streamInfo.inputLatency =
                ((PaTime) PaUtil_GetBufferProcessorInputLatencyFrames(
                        &(m_oboeStream->bufferProcessor)) +
                 m_oboeStream->framesPerHostCallback) / sampleRate;
        ENSURE(InitializeInputStream(m_oboeHostApi,
                                     m_androidInputPreset, sampleRate),
               "Initializing inputstream failed")
    } else { m_oboeStream->hasInput = false; }

    if (m_outputChannelCount > 0) {
        m_oboeStream->hasOutput = true;
        m_oboeStream->streamRepresentation.streamInfo.outputLatency =
                ((PaTime) PaUtil_GetBufferProcessorOutputLatencyFrames(
                        &m_oboeStream->bufferProcessor)
                 + m_oboeStream->framesPerHostCallback) / sampleRate;
        ENSURE(InitializeOutputStream(m_oboeHostApi,
                                      m_androidOutputUsage, sampleRate),
               "Initializing outputstream failed");
    } else { m_oboeStream->hasOutput = false; }

    *s = (PaStream *) m_oboeStream;
    return m_error;

    error:
    if (m_oboeStream)
        PaUtil_FreeMemory(m_oboeStream);

    LOGE("[PaOboe - OpenStream]\t Error opening stream(s). Error code: %d", m_error);

    return m_error;
}


/**
 * \brief   Calls OboeEngine::closeStream, and then frees the memory that was allocated to sustain
 *          the stream(s). When CloseStream() is called, the multi-api layer ensures that the stream
 *          has already been stopped or aborted.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  paNoError, but warns in the logs if OboeEngine::closeStream failed.
 */
static PaError CloseStream(PaStream *s) {
    auto *m_stream = (OboeStream *) s;
    auto *m_oboeEngine = reinterpret_cast<OboeEngine *>(m_stream->engineAddress);

    if (!(m_oboeEngine->closeStream()))
        LOGW("[PaOboe - CloseStream]\t Couldn't close the stream(s) correctly - see OboeEngine::CloseStream logs.");

    PaUtil_TerminateBufferProcessor(&m_stream->bufferProcessor);
    PaUtil_TerminateStreamRepresentation(&m_stream->streamRepresentation);

    for (int i = 0; i < numberOfBuffers; ++i) {
        if (m_stream->hasOutput)
            PaUtil_FreeMemory(m_stream->outputBuffers[i]);
        if (m_stream->hasInput)
            PaUtil_FreeMemory(m_stream->inputBuffers[i]);
    }

    if (m_stream->hasOutput)
        PaUtil_FreeMemory(m_stream->outputBuffers);
    if (m_stream->hasInput)
        PaUtil_FreeMemory(m_stream->inputBuffers);

    PaUtil_FreeMemory(m_stream);
    return paNoError;
}


/**
 * \brief   Allocates the memory of the buffers necessary to start a stream, both for output and
 *          input, then calls OboeEngine::startStream.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  paNoError if no errors occur, paUnanticipatedHostError if OboeEngine::startStream fails.
 */
static PaError StartStream(PaStream *s) {
    auto *m_stream = (OboeStream *) s;
    auto *m_oboeEngine = reinterpret_cast<OboeEngine *>(m_stream->engineAddress);

    PaUtil_ResetBufferProcessor(&m_stream->bufferProcessor);

    //Checking if the stream(s) are already active. TODO: check if it's working as expected (extensive testing needed, no problem spotted with situational tests)
    if (m_stream->isActive) {
        LOGW("[PaOboe - StartStream]\t Stream was already active, stopping...");
        StopStream(s);
        LOGW("[PaOboe - StartStream]\t Restarting...");
        StartStream(s);
    }

    m_stream->currentOutputBuffer = 0;
    m_stream->currentInputBuffer = 0;

    /* Initialize buffers */
    for (int i = 0; i < numberOfBuffers; ++i) {
        if (m_stream->hasOutput) {
            memset(m_stream->outputBuffers[m_stream->currentOutputBuffer], 0,
                   m_stream->framesPerHostCallback * m_stream->bytesPerFrame *
                   m_stream->bufferProcessor.outputChannelCount
            );
            m_stream->currentOutputBuffer = (m_stream->currentOutputBuffer + 1) % numberOfBuffers;
        }
        if (m_stream->hasInput) {
            memset(m_stream->inputBuffers[m_stream->currentInputBuffer], 0,
                   m_stream->framesPerHostCallback * m_stream->bytesPerFrame *
                   m_stream->bufferProcessor.inputChannelCount
            );
            m_stream->currentInputBuffer = (m_stream->currentInputBuffer + 1) % numberOfBuffers;
        }
    }

    /* Start the processing thread.*/
    if (!m_stream->isBlocking) {
        m_stream->callbackResult = paContinue;
        m_stream->oboeCallbackResult = DataCallbackResult::Continue;
    }

    m_stream->isStopped = false;
    m_stream->isActive = true;
    m_stream->doStop = false;
    m_stream->doAbort = false;

    if (!(m_oboeEngine->startStream()))
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
static PaError StopStream(PaStream *s) {
    PaError m_error = paNoError;
    auto *m_stream = (OboeStream *) s;
    auto *m_oboeEngine = reinterpret_cast<OboeEngine *>(m_stream->engineAddress);

    if (m_stream->isStopped) {
        LOGW("[PaOboe - StopStream]\t Stream was already stopped.");
    } else {
        if (!(m_stream->isBlocking)) {
            m_stream->doStop = true;
        }
        if (!(m_oboeEngine->stopStream())) {
            LOGE("[PaOboe - StopStream]\t Couldn't stop the stream(s) correctly - see OboeEngine::StopStream logs.");
            m_error = paUnanticipatedHostError;
        }

        m_stream->isActive = false;
        m_stream->isStopped = true;
        if (m_stream->streamRepresentation.streamFinishedCallback != nullptr)
            m_stream->streamRepresentation.streamFinishedCallback(
                    m_stream->streamRepresentation.userData);
    }

    return m_error;
}


/**
 * \brief   Aborts the stream callback, if the stream is not blocking, and calls
 *          OboeEngine::abortStream.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  paNoError if no errors occur, paUnanticipatedHostError if OboeStream::abortStream fails.
 */
static PaError AbortStream(PaStream *s) {
    PaError m_error = paNoError;
    auto *m_stream = (OboeStream *) s;
    auto *m_oboeEngine = reinterpret_cast<OboeEngine *>(m_stream->engineAddress);
    LOGI("[PaOboe - AbortStream]\t Aborting stream.");

    if (!m_stream->isBlocking) {
        m_stream->doAbort = true;
    }

    /* stop immediately so enqueue has no effect */
    if (!(m_oboeEngine->abortStream())) {
        LOGE("[PaOboe - AbortStream]\t Couldn't abort the stream - see OboeEngine::abortStream logs.");
        m_error = paUnanticipatedHostError;
    }

    m_stream->isActive = false;
    m_stream->isStopped = true;
    if (m_stream->streamRepresentation.streamFinishedCallback != nullptr)
        m_stream->streamRepresentation.streamFinishedCallback(
                m_stream->streamRepresentation.userData);

    return m_error;
}


/**
 * \brief   Copies an input stream buffer by buffer, and calls OboeEngine::readStream.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream;
 * @param   buffer is the address of the first sample of the buffer;
 * @param   frames is the total number of frames to read.
 * @return  paInternalError if OboeEngine::readStream fails, paNoError otherwise.
 */
static PaError ReadStream(PaStream *s, void *buffer, unsigned long frames) {
    auto *m_stream = (OboeStream *) s;
    auto *m_oboeEngine = reinterpret_cast<OboeEngine *>(m_stream->engineAddress);
    void *m_userBuffer = buffer;
    unsigned m_framesToRead;
    PaError m_error = paNoError;

    while (frames > 0) {
        m_framesToRead = PA_MIN(m_stream->framesPerHostCallback, frames);

        if (!(m_oboeEngine->readStream(m_userBuffer,
                                       m_framesToRead *
                                       m_stream->bufferProcessor.inputChannelCount)))
            m_error = paInternalError;

        m_stream->currentInputBuffer = (m_stream->currentInputBuffer + 1) % numberOfBuffers;
        frames -= m_framesToRead;
    }

    return m_error;
}


/**
 * \brief   Copies an output stream buffer by buffer, and calls OboeEngine::writeStream.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream;
 * @param   buffer is the address of the first sample of the buffer;
 * @param   frames is the total number of frames to write.
 * @return  paInternalError if OboeEngine::writeStream fails, paNoError otherwise.
 */
static PaError WriteStream(PaStream *s, const void *buffer, unsigned long frames) {
    auto *m_stream = (OboeStream *) s;
    auto *m_oboeEngine = reinterpret_cast<OboeEngine *>(m_stream->engineAddress);
    const void *m_userBuffer = buffer;
    unsigned m_framesToWrite;
    PaError m_error = paNoError;

    while (frames > 0) {
        m_framesToWrite = PA_MIN(m_stream->framesPerHostCallback, frames);

        if (!(m_oboeEngine->writeStream(m_userBuffer,
                                        m_framesToWrite *
                                        m_stream->bufferProcessor.outputChannelCount)))
            m_error = paInternalError;

        m_stream->currentOutputBuffer = (m_stream->currentOutputBuffer + 1) % numberOfBuffers;
        frames -= m_framesToWrite;
    }

    return m_error;
}


/*-------------------------------- PaSkeleton Secondary Functions --------------------------------*/

/**
 * \brief   Function needed by portaudio to understand how many frames can be read without waiting.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  the minimum number of frames that can be read without waiting.
 */
static signed long GetStreamReadAvailable(PaStream *s) {
    auto *m_stream = (OboeStream *) s;
    return m_stream->framesPerHostCallback * (numberOfBuffers - m_stream->currentInputBuffer);
}


/**
 * \brief   Function needed by portaudio to understand how many frames can be written without waiting.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  the minimum number of frames that can be written without waiting.
 */
static signed long GetStreamWriteAvailable(PaStream *s) {
    auto *m_stream = (OboeStream *) s;
    return m_stream->framesPerHostCallback * (numberOfBuffers - m_stream->currentOutputBuffer);
}


/**
 * \brief   Function needed by portaudio to understand if the stream is stopped.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  one (1) when the stream is stopped, or zero (0) when the stream is running.
 */
static PaError IsStreamStopped(PaStream *s) {
    auto *m_stream = (OboeStream *) s;
    return m_stream->isStopped;
}


/**
 * \brief   Function needed by portaudio to understand if the stream is active.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  one (1) when the stream is active (ie playing or recording audio), or zero (0) otherwise.
 */
static PaError IsStreamActive(PaStream *s) {
    auto *m_stream = (OboeStream *) s;
    return m_stream->isActive;
}


/**
 * \brief   Function needed by portaudio to get the stream time in seconds.
 * @param   s points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our OboeStream.
 * @return  The stream's current time in seconds, or 0 if an error occurred.
 */
static PaTime GetStreamTime(PaStream *s) {
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
static double GetStreamCpuLoad(PaStream *s) {
    auto *m_stream = (OboeStream *) s;
    return PaUtil_GetCpuLoad(&m_stream->cpuLoadMeasurer);
}


/*----------------------------------- PaOboe Utility Functions -----------------------------------*/

/**
 * \brief   In case that no buffer size was specifically set via PaOboe_setNativeBufferSize, this
 *          function is called to get a sensible value for the buffer size.
 * @return  256 for Android API Level <= 23, 192 otherwise.
 */
static unsigned long GetApproximateLowBufferSize() {
/* FIXME: This function should return the following commented values, but was changed in order to improve
         compatibility with KCTI for android. Please use the commented values in normal conditions. */

//    if (__ANDROID_API__ <= 23)
//        return 256;
//    else
//        return 192;

    return 1024;
}


/*----------------------------- Implementation of PaOboe.h functions -----------------------------*/

void PaOboe_SetSelectedDevice(Direction direction, int32_t deviceID) {
    LOGI("[PaOboe - SetSelectedDevice] Selecting device...");
    if (direction == Direction::Input)
        inputDeviceId = deviceID;
    else
        outputDeviceId = deviceID;
}


void PaOboe_SetNativeBufferSize(unsigned long bufferSize) {
    nativeBufferSize = bufferSize;
}


void PaOboe_SetNumberOfBuffers(unsigned buffers) {
    numberOfBuffers = buffers;
}
