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

#include <mutex>
#include <pthread.h>
#include <ctime>
#include <cstring>
#include <cstdint>
#include <string.h>
#include <vector>
#include "oboe/Oboe.h"
#include "portaudio.h"

#include <android/log.h>
#include <android/api-level.h>

#include "pa_oboe.h"

//FIXME: if your project needs a specific PaFormat, modify this value
#define paOboeDefaultFormat paFloat32

#define MODULE_NAME "PaOboe"

#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, MODULE_NAME, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, MODULE_NAME, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, MODULE_NAME, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,MODULE_NAME, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR,MODULE_NAME, __VA_ARGS__)
#define LOGF(...) __android_log_print(ANDROID_LOG_FATAL,MODULE_NAME, __VA_ARGS__)

#define ENSURE(expr, errorText)                                             \
    do                                                                      \
    {                                                                       \
        PaError err;                                                      \
        if (UNLIKELY((err = (expr)) < paNoError))                         \
        {                                                                   \
            PaUtil_DebugPrint(("Expression '" #expr "' failed in '" __FILE__ "', line: " PA_STRINGIZE( \
                                                                __LINE__ ) "\n")); \
            PaUtil_SetLastHostErrorInfo(paOboe, err, errorText); \
            error = err;                                               \
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

// Commonly used parameters initialized.
static constexpr int kDelayBeforeCloseMillis = 100; // Copied from AudioStream::kMaxDelayBeforeCloseMillis;

using namespace oboe;

class OboeEngine;
class OboeMediator;
struct PaOboeHostApiRepresentation;

/**
 * Stream structure, useful to store relevant information. It's needed by Portaudio.
 */
typedef struct PaOboeStream {
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

    bool isBlocking;
    bool isStopped;
    bool isActive;
    bool doStop;
    bool doAbort;

    inline bool hasOutput() const { return outputBuffers; }
    inline bool hasInput() const { return inputBuffers; }

    int callbackResult;
    DataCallbackResult oboeCallbackResult;
    PaStreamCallbackFlags cbFlags;

    PaSampleFormat inputFormat;
    PaSampleFormat outputFormat;

    // Buffers are managed by the callback function in Oboe.
    void **outputBuffers;
    int currentOutputBuffer;
    unsigned numOutputBuffers;
    void **inputBuffers;
    int currentInputBuffer;
    unsigned numInputBuffers;

    unsigned long framesPerHostCallback;
    unsigned bytesPerFrame;

    std::shared_ptr<OboeMediator> oboeMediator;
} PaOboeStream;

class OboeMediator: public AudioStreamCallback {
public:
    OboeMediator(PaOboeStream* paOboeStream): mOboeCallbackStream(paOboeStream) {}
    ~OboeMediator();

    //Callback function for non-blocking streams
    DataCallbackResult onAudioReady(AudioStream *oboeStream, void *audioData,
                                    int32_t numFrames) override;
    //Callback utils
    void onErrorAfterClose(AudioStream *oboeStream, oboe::Result error) override;
    void resetCallbackCounters();
    void setOutputCallback(std::shared_ptr<OboeMediator> instance) { mOutputBuilder.setDataCallback(instance)->setErrorCallback(instance); }
    void setInputCallback(std::shared_ptr<OboeMediator> instance) { mInputBuilder.setDataCallback(instance)->setErrorCallback(instance); }

    //The only instances of output and input streams that will be used, and their builders
    std::shared_ptr <AudioStream> mOutputStream;
    AudioStreamBuilder mOutputBuilder;
    std::shared_ptr <AudioStream> mInputStream;
    AudioStreamBuilder mInputBuilder;

private:
    //callback utils
    PaOboeStream *mOboeCallbackStream;
    unsigned long mFramesProcessed{};
    PaStreamCallbackTimeInfo mTimeInfo{};
    struct timespec mTimeSpec{};
};


/**
 * Stream engine of the host API - Oboe. We use a signleton instance of the engine for all PaOboeStream, and
 * we call its functions when we want to operate directly on Oboe. More information on each function is
 * provided right before its implementation.
 */
class OboeEngine {
public:
    static OboeEngine &getInstance() {
        static OboeEngine instance;
        return instance;
    }

    //Stream-managing functions
    PaError openStream(PaOboeStream *paOboeStream, Direction direction, PaDeviceIndex paDeviceId, int32_t sampleRate,
                       Usage androidOutputUsage, InputPreset androidInputPreset, PerformanceMode performanceMode,
                       SharingMode sharingMode, const char* packageName, ContentType contentType, SampleRateConversionQuality sampleRateConversionQuality);

    bool startStream(PaOboeStream *paOboeStream);

    bool stopStream(PaOboeStream *paOboeStream);

    bool restartStream(PaOboeStream *paOboeStream, int direction);

    bool closeStream(PaOboeStream *paOboeStream);

    bool abortStream(PaOboeStream *paOboeStream);

    //Blocking read/write functions
    bool writeStream(PaOboeStream *paOboeStream, const void *buffer, int32_t framesToWrite);

    bool readStream(PaOboeStream *paOboeStream, void *buffer, int32_t framesToRead);

    //Engine utils
    void constructPaOboeStream(PaOboeStream* paOboeStream);

    PaErrorCode setNumberOfBuffers(unsigned numberOfBuffers);
    unsigned getNumberOfBuffers() const { return mNumberOfBuffers; }
    PaErrorCode setNativeBufferSize(unsigned long bufferSize);
    PaErrorCode registerDevice(const char* name, int32_t id, PaOboe_Direction direction, int channelCount, int sampleRate);
    PaErrorCode initializeDeviceList(PaUtilHostApiRepresentation *hostApi, PaHostApiIndex hostApiIndex, PaOboeHostApiRepresentation *oboeHostApi);

private:
    OboeEngine() = default;

    unsigned long getLowBufferSize() const;
    struct RegisteredDevice {
        char* name;
        int32_t id;
        PaOboe_Direction direction;
        int channelCount;
        int sampleRate;
    };

    unsigned long mNativeBufferSize{0};
    unsigned mNumberOfBuffers{2};
    std::vector<RegisteredDevice> mRegisteredDevices = {};
    std::shared_ptr<OboeMediator> mTerminableMediator;
    bool mHasInitialised{false};

    std::mutex mMutex;

    //Conversion utils
    static AudioFormat PaToOboeFormat(PaSampleFormat paFormat);
};


/**
 * Structure used by Portaudio to interface with the HostApi - in this case, Oboe.
 */
typedef struct PaOboeHostApiRepresentation {
    PaUtilHostApiRepresentation inheritedHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilStreamInterface blockingStreamInterface;

    PaUtilAllocationGroup *allocations;

} PaOboeHostApiRepresentation;


/*----------------------------- OboeEngine functions implementation -----------------------------*/


/**
 * \brief   Opens an audio stream with a specific direction, sample rate and,
 *          depending on the direction of the stream, sets its usage (if
 *          direction == Direction::Output) or its preset (if direction == Direction::Input).
 *          Moreover, this function checks if the stream is blocking, and sets its callback
 *          function if not.
 * @param   paOboeStream The stream we want to open
 * @param   direction The Oboe::Direction of the stream we want to open;
 * @param   paDeviceId The sample rate of the stream we want to open;
 * @param   sampleRate The sample rate of the stream we want to open;
 * @param   androidOutputUsage The Oboe::Usage of the output stream we want to open
 *              (only matters with Android Api level >= 28);
 * @param   androidInputPreset The Preset of the input stream we want to open
 *              (only matters with Android Api level >= 28).
 * @param   performanceMode The Oboe performance mode.
 * @param   sharingMode The Oboe sharing mode.
 * @param   packageName The optional package name to use in oboe.
 * @param   contentType The Oboe content type.
 * @param   sampleRateConversionQuality The Oboe sample rate conversion quality.
 * @return  paNoError if everything goes as expected, paUnanticipatedHostError if Oboe fails to open
 *          a stream, and paInsufficientMemory if the memory allocation of the buffers fails.
 */
PaError OboeEngine::openStream(PaOboeStream *paOboeStream, Direction direction, PaDeviceIndex paDeviceId, int32_t sampleRate,
                               Usage androidOutputUsage, InputPreset androidInputPreset, PerformanceMode performanceMode,
                               SharingMode sharingMode, const char* packageName, ContentType contentType, SampleRateConversionQuality sampleRateConversionQuality) {
    PaError error = paNoError;
    Result result;

    if (paDeviceId >= mRegisteredDevices.size() || paDeviceId < 0){
        LOGE("[OboeEngine::openStream]\t Device ID out of bount! %d given, but only %d devices known.", paDeviceId, mRegisteredDevices.size());
        return paDeviceUnavailable;
    }
    int32_t deviceId = mRegisteredDevices[paDeviceId].id;
    if (static_cast<Direction>(mRegisteredDevices[paDeviceId].direction) != direction){
        LOGE("[OboeEngine::openStream]\t Device ID %d has incompatible direction.", paDeviceId);
        return paDeviceUnavailable;
    }

    if (paOboeStream == nullptr) {
        LOGE("[OboeEngine::openStream]\t paOboeStream is a nullptr.");
        return paInternalError;
    }

    auto& mediator = paOboeStream->oboeMediator;

    if(!(paOboeStream->isBlocking)){
        mediator->resetCallbackCounters();
    }

    if (direction == Direction::Input) {
        LOGV("[PaOboe - OpenStream]\t Open input stream on device %d with %d channels.", deviceId, paOboeStream->bufferProcessor.inputChannelCount);
        mediator->mInputBuilder.setChannelCount(paOboeStream->bufferProcessor.inputChannelCount)
                ->setFormat(PaToOboeFormat(paOboeStream->inputFormat))
                ->setSampleRate(sampleRate)
                ->setDirection(Direction::Input)
                ->setDeviceId(deviceId)
                ->setSampleRateConversionQuality(sampleRateConversionQuality)
                ->setPerformanceMode(performanceMode)
                ->setInputPreset(androidInputPreset)
                ->setFramesPerCallback(paOboeStream->framesPerHostCallback);

        if (!(paOboeStream->isBlocking)) {
            mediator->setInputCallback(mediator);
        }

        result = mediator->mInputBuilder.openStream(mediator->mInputStream);

        if (result != Result::OK || !mediator->mInputStream) {
            LOGE("[OboeEngine::OpenStream]\t Oboe couldn't open the input stream: %s",
                 convertToText(result));
            return paUnanticipatedHostError;
        }

        mediator->mInputStream->setDelayBeforeCloseMillis(kDelayBeforeCloseMillis);
        mediator->mInputStream->setPerformanceHintEnabled(performanceMode == PerformanceMode::LowLatency);
        mediator->mInputStream->setBufferSizeInFrames(mediator->mInputStream->getFramesPerBurst() *
                                                               getNumberOfBuffers());
        paOboeStream->numInputBuffers = getNumberOfBuffers();
        paOboeStream->inputBuffers =
                (void **) PaUtil_AllocateZeroInitializedMemory(paOboeStream->numInputBuffers * sizeof(int32_t * ));

        for (int i = 0; i < paOboeStream->numInputBuffers; ++i) {
            paOboeStream->inputBuffers[i] = (void *) PaUtil_AllocateZeroInitializedMemory(
                    paOboeStream->framesPerHostCallback *
                            paOboeStream->bytesPerFrame *
                            paOboeStream->bufferProcessor.inputChannelCount);

            if (!paOboeStream->inputBuffers[i]) {
                for (int j = 0; j < i; ++j)
                    PaUtil_FreeMemory(paOboeStream->inputBuffers[j]);
                PaUtil_FreeMemory(paOboeStream->inputBuffers);
                mediator->mInputStream->close();
                error = paInsufficientMemory;
                break;
            }
        }
        paOboeStream->currentInputBuffer = 0;
    } else {
        LOGV("[PaOboe - OpenStream]\t Open output stream on device %d with %d channels.", deviceId, paOboeStream->bufferProcessor.outputChannelCount);
        mediator->mOutputBuilder.setChannelCount(paOboeStream->bufferProcessor.outputChannelCount)
                ->setFormat(PaToOboeFormat(paOboeStream->outputFormat))
                ->setSampleRate(sampleRate)
                ->setDirection(Direction::Output)
                ->setDeviceId(deviceId)
                ->setSharingMode(sharingMode)
                ->setPackageName(packageName)
                ->setContentType(contentType)
                ->setPerformanceMode(performanceMode)
                ->setUsage(androidOutputUsage)
                ->setFramesPerCallback(paOboeStream->framesPerHostCallback);

        if (!paOboeStream->isBlocking) {
            mediator->setOutputCallback(mediator);
        }

        // We could also add AAudioExtensions library to allow the use of mmap and improve low latency performance
        // AAudioExtensions::getInstance().setMMapEnabled(false);

        result = mediator->mOutputBuilder.openStream(mediator->mOutputStream);
        if (result != Result::OK) {
            LOGE("[OboeEngine::OpenStream]\t Oboe couldn't open the output stream: %s",
                 convertToText(result));
            return paUnanticipatedHostError;
        }

        mediator->mOutputStream->setDelayBeforeCloseMillis(kDelayBeforeCloseMillis);
        mediator->mOutputStream->setPerformanceHintEnabled(performanceMode == PerformanceMode::LowLatency);
        mediator->mOutputStream->setBufferSizeInFrames(mediator->mOutputStream->getFramesPerBurst() * getNumberOfBuffers());
        paOboeStream->numOutputBuffers = getNumberOfBuffers();
        paOboeStream->outputBuffers =
            (void **) PaUtil_AllocateZeroInitializedMemory(paOboeStream->numOutputBuffers * sizeof(int32_t * ));

        for (int i = 0; i < paOboeStream->numOutputBuffers; ++i) {
            paOboeStream->outputBuffers[i] = (void *) PaUtil_AllocateZeroInitializedMemory(
                    paOboeStream->framesPerHostCallback *
                            paOboeStream->bytesPerFrame *
                            paOboeStream->bufferProcessor.outputChannelCount);

            if (!paOboeStream->outputBuffers[i]) {
                for (int j = 0; j < i; ++j)
                    PaUtil_FreeMemory(paOboeStream->outputBuffers[j]);
                PaUtil_FreeMemory(paOboeStream->outputBuffers);
                mediator->mOutputStream->close();
                error = paInsufficientMemory;
                break;
            }
        }
        paOboeStream->currentOutputBuffer = 0;
    }

    return error;
}


/**
 * \brief   Starts paOboeStream - both input and output AudioStreams of the paOboeStream are checked
 *          and requested to be started.
 * @param   paOboeStream The stream we want to start.
 * @return  true if the streams we wanted to start are started successfully, false otherwise.
 */
bool OboeEngine::startStream(PaOboeStream *paOboeStream) {
    Result outputResult = Result::OK, inputResult = Result::OK;
    auto& mediator = paOboeStream->oboeMediator;

    if (paOboeStream->hasInput()) {
        inputResult = mediator->mInputStream->requestStart();
        if (inputResult != Result::OK)
            LOGE("[OboeEngine::startStream]\t Oboe couldn't start the input stream: %s",
                 convertToText(inputResult));
    }
    if (paOboeStream->hasOutput()) {
        outputResult = mediator->mOutputStream->requestStart();
        if (outputResult != Result::OK)
            LOGE("[OboeEngine::startStream]\t Oboe couldn't start the output stream: %s",
                 convertToText(outputResult));
    }

    return (outputResult == Result::OK && inputResult == Result::OK);
}


/**
 * \brief   Stops paOboeStream - both input and output AudioStreams of the PaOboeStream are checked
 *          and requested to be stopped.
 * @param   paOboeStream The stream we want to stop.
 * @return  true if the streams we wanted to stop are stopped successfully, false otherwise.
 */
bool OboeEngine::stopStream(PaOboeStream *paOboeStream) {
    Result outputResult = Result::OK, inputResult = Result::OK;
    auto& mediator = paOboeStream->oboeMediator;

    if (paOboeStream->hasInput()) {
        inputResult = mediator->mInputStream->requestStop();
        if (inputResult != Result::OK)
            LOGE("[OboeEngine::stopStream]\t Oboe couldn't stop the input stream: %s",
                 convertToText(inputResult));
    }
    if (paOboeStream->hasOutput()) {
        outputResult = mediator->mOutputStream->requestStop();
        if (outputResult != Result::OK)
            LOGE("[OboeEngine::stopStream]\t Oboe couldn't stop the output stream: %s",
                 convertToText(outputResult));
    }

    return (outputResult == Result::OK && inputResult == Result::OK);
}


/**
 * \brief   Called when it's needed to restart the PaOboeStream's audio stream(s) when the audio device(s) change
 *          while a stream is started. Oboe will stop and close said streams in that case,
 *          so this function just reopens and restarts them.
 * @param   paOboeStream The stream we want to restart.
 * @param   direction The direction(s) of the stream that have to be restarted (1 for output, 2 for input, 3 for both).
 * @return  true if the stream is restarted successfully, false otherwise.
 */
bool OboeEngine::restartStream(PaOboeStream* paOboeStream, int direction) {
    bool outcome = true;
    Result result;
    auto& mediator = paOboeStream->oboeMediator;

    switch (direction) {
        case 1: //output-only
            result = mediator->mOutputBuilder.openStream(mediator->mOutputStream);
            if (result != Result::OK)
                LOGE("[OboeEngine::restartStream]\t Oboe couldn't reopen the output stream: %s",
                     convertToText(result));
            result = mediator->mOutputStream->start();
            if (result != Result::OK) {
                LOGE("[OboeEngine::restartStream]\t Oboe couldn't restart the output stream: %s",
                     convertToText(result));
                outcome = false;
            }
            break;

        case 2: //input-only
            result = mediator->mInputBuilder.openStream(mediator->mInputStream);
            if (result != Result::OK)
                LOGE("[OboeEngine::restartStream]\t Oboe couldn't reopen the input stream: %s",
                     convertToText(result));
            result = mediator->mInputStream->start();
            if (result != Result::OK) {
                LOGE("[OboeEngine::restartStream]\t Oboe couldn't restart the input stream: %s",
                     convertToText(result));
                outcome = false;
            }
            break;

        default:
            // unspecified direction or both directions: restart both streams
            LOGW("[OboeEngine::restartStream]\t Unspecified direction, restarting both streams");
            outcome = (restartStream(paOboeStream, 1) && restartStream(paOboeStream, 2));
            break;
    }

    return outcome;
}


/**
 * \brief   Closes paOboeStream - both input and output AudioStreams of the PaOboeStream are checked
 *          and closed if active.
 * @param   paOboeStream The stream we want to close.
 * @return  true if the stream is closed successfully, otherwise returns false.
 */
bool OboeEngine::closeStream(PaOboeStream *paOboeStream) {
    Result outputResult = Result::OK, inputResult = Result::OK;

    if (paOboeStream == nullptr) {
        LOGE("[OboeEngine::closeStream]\t paOboeStream is a nullptr.");
        return false;
    }

    auto& mediator = paOboeStream->oboeMediator;

    if (paOboeStream->hasOutput()) {
        outputResult = mediator->mOutputStream->close();
        if (outputResult == Result::ErrorClosed) {
            outputResult = Result::OK;
            LOGW("[OboeEngine::closeStream]\t Tried to close output stream, but was already closed.");
        }
    }
    if (paOboeStream->hasInput()) {
        inputResult = mediator->mInputStream->close();
        if (inputResult == Result::ErrorClosed) {
            inputResult = Result::OK;
            LOGW("[OboeEngine::closeStream]\t Tried to close input stream, but was already closed.");
        }
    }

    return (outputResult == Result::OK && inputResult == Result::OK);
}


/**
 * \brief   Stops paOboeStream - both input and output AudioStreams of the PaOboeStream are checked and forcefully stopped.
 * @param   paOboeStream The stream we want to abort.
 * @return  true if the output stream and the input stream are stopped successfully, false otherwise.
 */
bool OboeEngine::abortStream(PaOboeStream *paOboeStream) {
    Result outputResult = Result::OK, inputResult = Result::OK;

    if (paOboeStream == nullptr) {
        LOGE("[OboeEngine::abortStream]\t paOboeStream is a nullptr.");
        return false;
    }

    auto& mediator = paOboeStream->oboeMediator;

    if (paOboeStream->hasInput()) {
        inputResult = mediator->mInputStream->stop();
        if (inputResult != Result::OK)
            LOGE("[OboeEngine::abortStream]\t Couldn't force the input stream to stop: %s",
                 convertToText(inputResult));
        inputResult = mediator->mInputStream->close();
        if (inputResult != Result::OK)
            LOGE("[OboeEngine::abortStream]\t Couldn't force the input stream to close: %s",
                 convertToText(inputResult));
    }
    if (paOboeStream->hasOutput()) {
        outputResult = mediator->mOutputStream->stop();
        if (outputResult != Result::OK)
            LOGE("[OboeEngine::abortStream]\t Couldn't force the output stream to stop: %s",
                 convertToText(outputResult));
        outputResult = mediator->mOutputStream->close();
        if (outputResult != Result::OK)
            LOGE("[OboeEngine::abortStream]\t Couldn't force the output stream to close: %s",
                 convertToText(outputResult));
    }

    return (outputResult == Result::OK && inputResult == Result::OK);
}


/**
 * \brief   Writes frames on the output stream of paOboeStream. Used by blocking streams.
 * @param   paOboeStream The stream we want to write onto.
 * @param   buffer The buffer that we want to write on the output stream;
 * @param   framesToWrite The number of frames that we want to write.
 * @return  true if the buffer is written correctly, false if the write function returns an error
 *          different from ErrorDisconnected. In case of ErrorDisconnected, the function returns
 *          true if the stream is successfully restarted, and false otherwise.
 */
bool OboeEngine::writeStream(PaOboeStream *paOboeStream, const void *buffer, int32_t framesToWrite) {
    bool outcome = true;
    auto& mediator = paOboeStream->oboeMediator;

    ResultWithValue <int32_t> result = mediator->mOutputStream->write(buffer, framesToWrite, TIMEOUT_NS);

    // If the stream is interrupted because the device suddenly changes, restart the stream.
    if (result.error() == Result::ErrorDisconnected) {
        if (restartStream(paOboeStream, 1))
            return true;
    }

    if (!result) {
        LOGE("[OboeEngine::writeStream]\t Error writing stream: %s", convertToText(result.error()));
        outcome = false;
    }
    return outcome;
}


/**
 * \brief   Reads frames from the input stream of paOboeStream. Used by blocking streams.
 * @param   paOboeStream The stream we want to read from.
 * @param   buffer The buffer that we want to read from the input stream;
 * @param   framesToWrite The number of frames that we want to read.
 * @return  true if the buffer is read correctly, false if the read function returns an error
 *          different from ErrorDisconnected. In case of ErrorDisconnected, the function returns
 *          true if the stream is successfully restarted, and false otherwise.
 */
bool OboeEngine::readStream(PaOboeStream *paOboeStream, void *buffer, int32_t framesToRead) {
    bool outcome = true;
    auto& mediator = paOboeStream->oboeMediator;

    ResultWithValue <int32_t> result = mediator->mInputStream->read(buffer, framesToRead, TIMEOUT_NS);

    // If the stream is interrupted because the device suddenly changes, restart the stream.
    if (result.error() == Result::ErrorDisconnected) {
        if (restartStream(paOboeStream, 2))
            return true;
    }

    if (!result) {
        LOGE("[OboeEngine::readStream]\t Error reading stream: %s", convertToText(result.error()));
        outcome = false;
    }
    return outcome;
}


/**
 * \brief   Allocates the memory of a PaOboeStream, and sets its EngineAddress to this.
 * @return  the address of the paOboeStream.
 */
void OboeEngine::constructPaOboeStream(PaOboeStream* paOboeStream) {
    paOboeStream->oboeMediator = std::make_shared<OboeMediator>(paOboeStream);
    mTerminableMediator = paOboeStream->oboeMediator;
}

/**
 * \brief   Set the number of buffers to use - must be set before PA init.
 * @return  Error if PA has already be init'ed.
 */
PaErrorCode OboeEngine::setNumberOfBuffers(unsigned numberOfBuffers) {
    const std::lock_guard<std::mutex> lock(mMutex);
    if (mHasInitialised){
        return paCanNotInitializeRecursively;
    }
    mNumberOfBuffers = numberOfBuffers;
    return paNoError;
}

/**
 * \brief   Set the number of buffers to use - must be set before PA init.
 * @return  Error if PA has already be init'ed.
 */
PaErrorCode OboeEngine::setNativeBufferSize(unsigned long bufferSize) {
    const std::lock_guard<std::mutex> lock(mMutex);
    if (mHasInitialised){
        return paCanNotInitializeRecursively;
    }
    mNativeBufferSize = bufferSize;
    return paNoError;
}


/**
 * \brief   Get a sensible value for the buffer size.
 * @return  256 for Android API Level <= 23, 192 otherwise or in case that a buffer size was specifically set via PaOboe_setNativeBufferSize, use this value.
 */
unsigned long OboeEngine::getLowBufferSize() const {
    /* If the user has set paOboe_nativeBufferSize by querying the optimal buffer size via java,
           use the user-defined value since that will offer the lowest possible latency. */
    if (mNativeBufferSize){
        return mNativeBufferSize;
    }

    if (__ANDROID_API__ <= 23)
        return 256;
    else
        return 192;
}

/**
 * \brief   Register devices, prior to PA init.
 * @return  Ok or error if no more device can be registered due to an error.
 */
PaErrorCode OboeEngine::registerDevice(const char* name, int32_t id, PaOboe_Direction direction, int channelCount, int sampleRate) {
    const std::lock_guard<std::mutex> lock(mMutex);
    if (mHasInitialised){
        return paCanNotInitializeRecursively;
    }
    mRegisteredDevices.emplace_back(RegisteredDevice{
        (char*)malloc(sizeof(char)*strlen(name)),
        id,
        direction,
        channelCount,
        sampleRate,
    });
    strcpy(mRegisteredDevices.back().name, name);
    return paNoError;
}

/**
 * \brief   Register devices, prior to PA init.
 * @return  Ok or error if no more device can be registered due to an error.
 */
PaErrorCode OboeEngine::initializeDeviceList(PaUtilHostApiRepresentation *hostApi, PaHostApiIndex hostApiIndex, PaOboeHostApiRepresentation *oboeHostApi) {
    const std::lock_guard<std::mutex> lock(mMutex);
    mHasInitialised = true;

    int deviceCount = mRegisteredDevices.size();

    hostApi->deviceInfos = (PaDeviceInfo **) PaUtil_GroupAllocateZeroInitializedMemory(
            oboeHostApi->allocations, sizeof(PaDeviceInfo * ) * deviceCount);

    if (!hostApi->deviceInfos) {
        return paInsufficientMemory;
    }

    /* allocate all device info structs in a contiguous block */
    PaDeviceInfo *deviceInfoArray = (PaDeviceInfo *) PaUtil_GroupAllocateZeroInitializedMemory(
            oboeHostApi->allocations, sizeof(PaDeviceInfo) * deviceCount);
    if (!deviceInfoArray) {
        return paInsufficientMemory;
    }

    for (int i = 0; i < deviceCount; ++i) {
        const auto& registeredDevice = mRegisteredDevices[i];
        PaDeviceInfo *deviceInfo = &deviceInfoArray[i];
        deviceInfo->structVersion = 2;
        deviceInfo->hostApi = hostApiIndex;
        deviceInfo->name = registeredDevice.name;

        deviceInfo->maxOutputChannels = registeredDevice.direction == PaOboe_Direction::Output ? registeredDevice.channelCount : 0;
        deviceInfo->maxInputChannels = registeredDevice.direction == PaOboe_Direction::Input ? registeredDevice.channelCount : 0;
        deviceInfo->defaultSampleRate = registeredDevice.sampleRate;

        deviceInfo->defaultLowInputLatency =
                (double) getLowBufferSize() / deviceInfo->defaultSampleRate;
        deviceInfo->defaultLowOutputLatency =
                (double) getLowBufferSize() / deviceInfo->defaultSampleRate;
        deviceInfo->defaultHighInputLatency =
                (double) getLowBufferSize() * 4 / deviceInfo->defaultSampleRate;
        deviceInfo->defaultHighOutputLatency =
                (double) getLowBufferSize() * 4 / deviceInfo->defaultSampleRate;

        hostApi->deviceInfos[i] = deviceInfo;
        ++hostApi->info.deviceCount;
    }
    return paNoError;
}

/**
 * \brief   Converts PaSampleFormat values into Oboe::AudioFormat values.
 * @param   paFormat the PaSampleFormat we want to convert.
 * @return  the converted AudioFormat.
 */
AudioFormat OboeEngine::PaToOboeFormat(PaSampleFormat paFormat) {
    AudioFormat oboeFormat;
    switch (paFormat) {
        case paFloat32:
            oboeFormat = AudioFormat::Float;
            LOGV("[OboeEngine::PaToOboeFormat]\t REQUESTED OBOE FORMAT: FLOAT");
            break;
        case paInt16:
            oboeFormat = AudioFormat::I16;
            LOGV("[OboeEngine::PaToOboeFormat]\t REQUESTED OBOE FORMAT: I16");
            break;
        case paInt32:
            oboeFormat = AudioFormat::I32;
            LOGV("[OboeEngine::PaToOboeFormat]\t REQUESTED OBOE FORMAT: I32");
            break;
        case paInt24:
            oboeFormat = AudioFormat::I24;
            LOGV("[OboeEngine::PaToOboeFormat]\t REQUESTED OBOE FORMAT: I24");
            break;
        default:
            oboeFormat = AudioFormat::Unspecified;
            LOGW("[OboeEngine::PaToOboeFormat]\t Setting AudioFormat to Unspecified, Oboe does not support the requested format.");
            break;
    }
    return oboeFormat;
}

/*----------------------------- OboeMediator functions implementations -----------------------------*/

OboeMediator::~OboeMediator(){
    if (!mOboeCallbackStream) return;

    if (mOboeCallbackStream->hasOutput()){
        for (int i = 0; i < mOboeCallbackStream->numOutputBuffers; ++i) {
            PaUtil_FreeMemory(mOboeCallbackStream->outputBuffers[i]);
        }
        PaUtil_FreeMemory(mOboeCallbackStream->outputBuffers);
    }

    if (mOboeCallbackStream->hasInput()){
        for (int i = 0; i < mOboeCallbackStream->numInputBuffers; ++i) {
            PaUtil_FreeMemory(mOboeCallbackStream->inputBuffers[i]);
        }
        PaUtil_FreeMemory(mOboeCallbackStream->inputBuffers);
    }

    PaUtil_FreeMemory(mOboeCallbackStream);
}

/**
 * \brief   Oboe's callback routine.
 */
DataCallbackResult
OboeMediator::onAudioReady(AudioStream *oboeStream, void *audioData, int32_t numFrames) {

    clock_gettime(CLOCK_REALTIME, &mTimeSpec);
    mTimeInfo.currentTime = (PaTime)(mTimeSpec.tv_sec + (mTimeSpec.tv_nsec / 1000000000.0));
    mTimeInfo.outputBufferDacTime = (PaTime)(mOboeCallbackStream->framesPerHostCallback
                                              /
                                              mOboeCallbackStream->streamRepresentation.streamInfo.sampleRate
                                              + mTimeInfo.currentTime);
    mTimeInfo.inputBufferAdcTime = (PaTime)(mOboeCallbackStream->framesPerHostCallback
                                             /
                                             mOboeCallbackStream->streamRepresentation.streamInfo.sampleRate
                                             + mTimeInfo.currentTime);

    /* check if StopStream or AbortStream was called */
    if (mOboeCallbackStream->doStop) {
        mOboeCallbackStream->callbackResult = paComplete;
    } else if (mOboeCallbackStream->doAbort) {
        mOboeCallbackStream->callbackResult = paAbort;
    }

    PaUtil_BeginCpuLoadMeasurement(&mOboeCallbackStream->cpuLoadMeasurer);
    PaUtil_BeginBufferProcessing(&mOboeCallbackStream->bufferProcessor,
                                 &mTimeInfo, mOboeCallbackStream->cbFlags);

    if (mOboeCallbackStream->hasOutput()) {
        mOboeCallbackStream->outputBuffers[mOboeCallbackStream->currentOutputBuffer] = audioData;
        PaUtil_SetOutputFrameCount(&mOboeCallbackStream->bufferProcessor, numFrames);
        PaUtil_SetInterleavedOutputChannels(&mOboeCallbackStream->bufferProcessor, 0,
                                            (void *) ((PaInt16 **) mOboeCallbackStream->outputBuffers)[
                                                    mOboeCallbackStream->currentOutputBuffer],
                                            0);
    }
    if (mOboeCallbackStream->hasInput()) {
        audioData = mOboeCallbackStream->inputBuffers[mOboeCallbackStream->currentInputBuffer];
        PaUtil_SetInputFrameCount(&mOboeCallbackStream->bufferProcessor, 0);
        PaUtil_SetInterleavedInputChannels(&mOboeCallbackStream->bufferProcessor, 0,
                                           (void *) ((PaInt16 **) mOboeCallbackStream->inputBuffers)[
                                                    mOboeCallbackStream->currentInputBuffer],
                                           0);
    }

    /* continue processing user buffers if callback result is paContinue or
     * if it is paComplete and userBuffers aren't empty yet  */
    if (mOboeCallbackStream->callbackResult == paContinue
        || (mOboeCallbackStream->callbackResult == paComplete
            && !PaUtil_IsBufferProcessorOutputEmpty(&mOboeCallbackStream->bufferProcessor))) {
        mFramesProcessed = PaUtil_EndBufferProcessing(&mOboeCallbackStream->bufferProcessor,
                                                       &mOboeCallbackStream->callbackResult);
    }

    /* enqueue a buffer only when there are frames to be processed,
     * this will be 0 when paComplete + empty buffers or paAbort
     */
    if (mFramesProcessed > 0) {
        if (mOboeCallbackStream->hasOutput()) {
            mOboeCallbackStream->currentOutputBuffer =
                    (mOboeCallbackStream->currentOutputBuffer + 1) % mOboeCallbackStream->numOutputBuffers;
        }
        if (mOboeCallbackStream->hasInput()) {
            mOboeCallbackStream->currentInputBuffer = (mOboeCallbackStream->currentInputBuffer + 1) % mOboeCallbackStream->numInputBuffers;
        }
    }

    PaUtil_EndCpuLoadMeasurement(&mOboeCallbackStream->cpuLoadMeasurer, mFramesProcessed);

    /* StopStream was called */
    if (mFramesProcessed == 0 && mOboeCallbackStream->doStop) {
        mOboeCallbackStream->oboeCallbackResult = DataCallbackResult::Stop;
    }

        /* if AbortStream or StopStream weren't called, stop from the cb */
    else if (mFramesProcessed == 0 && !(mOboeCallbackStream->doAbort || mOboeCallbackStream->doStop)) {
        mOboeCallbackStream->isActive = false;
        mOboeCallbackStream->isStopped = true;
        if (mOboeCallbackStream->streamRepresentation.streamFinishedCallback != nullptr)
            mOboeCallbackStream->streamRepresentation.streamFinishedCallback(
                    mOboeCallbackStream->streamRepresentation.userData);
        mOboeCallbackStream->oboeCallbackResult = DataCallbackResult::Stop; //TODO: Resume this test (onAudioReady)
    }

    return mOboeCallbackStream->oboeCallbackResult;
}


/**
 * \brief   If the data callback ends without returning DataCallbackResult::Stop, this routine tells
 *          what error occurred, and tries to restart the stream if the error was ErrorDisconnected.
 */
void OboeMediator::onErrorAfterClose(AudioStream *oboeStream, Result error) {
    if (error == oboe::Result::ErrorDisconnected) {
        LOGW("[OboeMediator::onErrorAfterClose]\t ErrorDisconnected - Restarting stream(s)");
        int i = 0;
        if(mOboeCallbackStream->hasOutput())
            i++;
        if(mOboeCallbackStream->hasInput())
            i+=2;
        if (!OboeEngine::getInstance().restartStream(mOboeCallbackStream, i))
            LOGE("[OboeMediator::onErrorAfterClose]\t Couldn't restart stream(s)");
    } else
        LOGE("[OboeMediator::onErrorAfterClose]\t Error was %s", oboe::convertToText(error));
}


/**
 * \brief   Resets callback counters (called at the start of each iteration of onAudioReady).
 */
void OboeMediator::resetCallbackCounters() {
    mFramesProcessed = 0;
    mTimeInfo = {0, 0, 0};
}


/*----------------------------- PaSkeleton functions implementations -----------------------------*/

/**
 * \brief   Initializes common parameters and the OboeEngine, and allocates the memory necessary to
 *          start the audio streams.
 * @param   hostApi points towards a *HostApiRepresentation, which is a structure representing the
 *              interface to a host API (see struct in "pa_hostapi.h");
 * @param   hostApiIndex is a PaHostApiIndex, the type used to enumerate the host APIs at runtime.
 * @return  paNoError if no errors occur, or paInsufficientMemory if memory allocation fails;
 */
PaError PaOboe_Initialize(PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex) {
    PaError result = paNoError;
    PaOboeHostApiRepresentation *oboeHostApi;

    oboeHostApi = (PaOboeHostApiRepresentation *) PaUtil_AllocateZeroInitializedMemory(
            sizeof(PaOboeHostApiRepresentation));
    if (!oboeHostApi) {
        result = paInsufficientMemory;
        goto error;
    }

    oboeHostApi->allocations = PaUtil_CreateAllocationGroup();
    if (!oboeHostApi->allocations) {
        result = paInsufficientMemory;
        goto error;
    }

    *hostApi = &oboeHostApi->inheritedHostApiRep;
    // Info initialization.
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paOboe;
    (*hostApi)->info.name = "Android Oboe";
    (*hostApi)->info.defaultOutputDevice = 0;
    (*hostApi)->info.defaultInputDevice = 0;
    (*hostApi)->info.deviceCount = 0;

    result = OboeEngine::getInstance().initializeDeviceList(*hostApi, hostApiIndex, oboeHostApi);
    if (result != paNoError) {
        goto error;
    }

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

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

        PaUtil_FreeMemory(oboeHostApi);
    }
    LOGE("[PaOboe - Initialize]\t Initialization failed. Error code: %d", result);
    return result;
}


/**
 * \brief   Interrupts the stream and frees the memory that was allocated to sustain the stream.
 * @param   hostApi points towards a *HostApiRepresentation, which is a structure representing the
 *              interface to a host API (see struct in "pa_hostapi.h").
 */
static void Terminate(struct PaUtilHostApiRepresentation *hostApi) {
    auto *oboeHostApi = (PaOboeHostApiRepresentation *) hostApi;

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
static PaError IsFormatSupported(struct PaUtilHostApiRepresentation *hostApi,
                                 const PaStreamParameters *inputParameters,
                                 const PaStreamParameters *outputParameters,
                                 double sampleRate) {
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    auto *oboeHostApi = (PaOboeHostApiRepresentation *) hostApi;

    if (inputParameters) {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        /* all standard sample formats are supported by the buffer adapter,
            this implementation doesn't support any custom sample formats */
        if (inputSampleFormat & paCustomFormat) {
            return paSampleFormatNotSupported;
        }

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */
        if (inputParameters->device == paUseHostApiSpecificDeviceSpecification) {
            return paInvalidDevice;
        }

        /* check that input device can support inputChannelCount */
        if (inputChannelCount >
                hostApi->deviceInfos[inputParameters->device]->maxInputChannels) {
            return paInvalidChannelCount;
        }

        /* validate inputStreamInfo */
        if (inputParameters->hostApiSpecificStreamInfo) {
            // Only has an effect on ANDROID_API>=28.
            InputPreset androidInputPreset =
                    static_cast<InputPreset>(
                                ((PaOboeStreamInfo *) inputParameters->hostApiSpecificStreamInfo)->androidInputPreset
                            );
            switch (androidInputPreset) {
                // Supported types
                case InputPreset::Generic:
                case InputPreset::Camcorder:
                case InputPreset::VoiceRecognition:
                case InputPreset::VoiceCommunication:
                case InputPreset::VoicePerformance:
                    break;
                default:
                    LOGW("[PaOboe - IsFormatSupported]\t Request an unsupported input preset %d", androidInputPreset);
                    return paIncompatibleHostApiSpecificStreamInfo;
            }
        }
    } else {
        inputChannelCount = 0;
    }

    if (outputParameters) {
        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;

        /* all standard sample formats are supported by the buffer adapter,
            this implementation doesn't support any custom sample formats */
        if (outputSampleFormat & paCustomFormat) {
            return paSampleFormatNotSupported;
        }

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */
        if (outputParameters->device == paUseHostApiSpecificDeviceSpecification) {
            return paInvalidDevice;
        }

        /* check that output device can support outputChannelCount */
        if (outputChannelCount > hostApi->deviceInfos[outputParameters->device]->maxOutputChannels) {
            return paInvalidChannelCount;
        }

        /* validate outputStreamInfo */
        if (outputParameters->hostApiSpecificStreamInfo) {
            // Only has an effect on ANDROID_API>=28.
            Usage androidOutputUsage =
                    static_cast<Usage>(
                                ((PaOboeStreamInfo *) outputParameters->hostApiSpecificStreamInfo)->androidOutputUsage
                            );
            switch (androidOutputUsage) {
                // Supported types
                case Usage::Media:
                case Usage::Notification:
                case Usage::NotificationEvent:
                case Usage::NotificationRingtone:
                case Usage::VoiceCommunication:
                case Usage::VoiceCommunicationSignalling:
                case Usage::Alarm:
                case Usage::Game:
                    break;
                default:
                    LOGW("[PaOboe - IsFormatSupported]\t Request an unsupported usage %d", androidOutputUsage);
                    return paIncompatibleHostApiSpecificStreamInfo;
            }
        }
    } else {
        outputChannelCount = 0;
    }

    return paFormatIsSupported;
}


/**
 * \brief   Calls OboeEngine::openStream to open the outputStream and a Generic input preset.
 * @param   paOboeStream is the PaOboeStream we want to initialize in the output direction.
 * @param   oboeHostApi points towards a OboeHostApiRepresentation (see struct defined at the top of
 *              this file);
 * @param   androidOutputUsage is an attribute that expresses why we are opening the output stream.
 *              This information can be used by certain platforms to make more refined volume or
 *              routing decisions. It only has an effect on Android API 28+.
 * @param   sampleRate is the sample rate we want for the audio stream we want to initialize. This is used to allocate
 *              the correct amount of memory.
 * @return  the value returned by OboeEngine::openStream.
 */
static PaError InitializeOutputStream(PaOboeStream* paOboeStream, PaOboeHostApiRepresentation *oboeHostApi, PaDeviceIndex deviceId,
                                      Usage androidOutputUsage, double sampleRate, PerformanceMode performanceMode,
                                      SharingMode sharingMode, const char* packageName, ContentType contentType,
                                      SampleRateConversionQuality sampleRateConversionQuality) {

    LOGV("[PaOboe - OpenStream]\t Initialize output stream %d", deviceId);
    return OboeEngine::getInstance().openStream(paOboeStream,
                                                 Direction::Output, deviceId,
                                                 sampleRate,
                                                 androidOutputUsage,
                                                 Generic,  //Input preset won't be used, so we put the default value.
                                                 performanceMode,
                                                 sharingMode,
                                                 packageName,
                                                 contentType,
                                                 sampleRateConversionQuality);
}


/**
 * \brief   Calls OboeEngine::openStream to open the outputStream and a Generic input preset.
 * @param   paOboeStream is the PaOboeStream we want to initialize in the input direction.
 * @param   oboeHostApi points towards a OboeHostApiRepresentation (see struct defined at the top of
 *              this file);
 * @param   androidInputPreset is an attribute that defines the audio source. This information
 *              defines both a default physical source of audio signal, and a recording configuration.
 *              It only has an effect on Android API 28+.
 * @param   sampleRate is the sample rate we want for the audio stream we want to initialize. This is used to allocate
 *              the correct amount of memory.
 * @return  the value returned by OboeEngine::openStream.
 */
static PaError InitializeInputStream(PaOboeStream* paOboeStream, PaOboeHostApiRepresentation *oboeHostApi, int32_t deviceId,
                                     InputPreset androidInputPreset, double sampleRate, PerformanceMode performanceMode,
                                     SharingMode sharingMode, const char* packageName, ContentType contentType,
                                      SampleRateConversionQuality sampleRateConversionQuality) {

    LOGV("[PaOboe - OpenStream]\t Initialize input stream %d", deviceId);
    return OboeEngine::getInstance().openStream(paOboeStream,
                                                 Direction::Input, deviceId,
                                                 sampleRate,
                                                 Usage::Media,   //Usage won't be used, so we put the default value.
                                                 androidInputPreset,
                                                 performanceMode,
                                                 sharingMode,
                                                 packageName,
                                                 contentType,
                                                 sampleRateConversionQuality);
}


/**
 * \brief   Opens the portaudio audio stream - while initializing our PaOboeStream.
 * @param   hostApi points towards a *HostApiRepresentation, which is a structure representing the
 *              interface to a host API (see struct in "pa_hostapi.h");
 * @param   paStream points to a pointer to a PaStream, which is an audio stream structure used and built
 *              by portaudio, which will hold the information of our PaOboeStream;
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
                          PaStream **paStream,
                          const PaStreamParameters *inputParameters,
                          const PaStreamParameters *outputParameters,
                          double sampleRate,
                          unsigned long framesPerBuffer,
                          PaStreamFlags streamFlags,
                          PaStreamCallback *streamCallback,
                          void *userData) {
    PaError error = paNoError;
    auto oboeHostApi = (PaOboeHostApiRepresentation *) hostApi;
    unsigned long framesPerHostBuffer; /* these may not be equivalent for all implementations */
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    PaSampleFormat hostInputSampleFormat, hostOutputSampleFormat;

    //Initialization to generic values, in the event that these hostApiSpecificStreamInfo were not set
    Usage androidOutputUsage = Usage::Media;
    InputPreset androidInputPreset = InputPreset::Generic;
    PerformanceMode performanceMode = PerformanceMode::None;
    SharingMode sharingMode = SharingMode::Shared;
    const char* packageName = "org.portaudio";
    ContentType contentType = ContentType::Speech;
    SampleRateConversionQuality sampleRateConversionQuality = SampleRateConversionQuality::None;

    PaOboeStream* paOboeStream = (PaOboeStream *) PaUtil_AllocateZeroInitializedMemory(sizeof(PaOboeStream));
    OboeEngine::getInstance().constructPaOboeStream(paOboeStream);

    const auto oboeParametersFromPAParameters =
        [&androidOutputUsage, &androidInputPreset, &performanceMode,
            &sharingMode, &packageName, &contentType,
            &sampleRateConversionQuality](PaOboeStreamInfo * streamInfo){
        androidOutputUsage =
                static_cast<Usage>(streamInfo->androidOutputUsage);
        androidInputPreset =
                static_cast<InputPreset>(streamInfo->androidInputPreset);
        performanceMode =
                static_cast<PerformanceMode>(streamInfo->performanceMode);
        sharingMode =
                static_cast<SharingMode>(streamInfo->sharingMode);
        packageName = streamInfo->packageName;
        contentType =
                static_cast<ContentType>(streamInfo->contentType);
        sampleRateConversionQuality =
                static_cast<SampleRateConversionQuality>(streamInfo->sampleRateConversionQuality);
    };

    if (!paOboeStream) {
        error = paInsufficientMemory;
        goto error;
    }

    LOGV("[PaOboe - OpenStream]\t OpenStream called.");

    if (inputParameters) {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        /* Oboe supports alternate device specification with API>=28, but here we reject the use of
            paUseHostApiSpecificDeviceSpecification and stick with the default. Devices can be set via
            PaOboe_SetSelectedDevice. */
        if (inputParameters->device == paUseHostApiSpecificDeviceSpecification)
            return paInvalidDevice;

        /* check that input device can support inputChannelCount */
        if (inputChannelCount > hostApi->deviceInfos[inputParameters->device]->maxInputChannels)
            return paInvalidChannelCount;

        /* validate inputStreamInfo */
        if (inputParameters->hostApiSpecificStreamInfo) {
            oboeParametersFromPAParameters((PaOboeStreamInfo *) inputParameters->hostApiSpecificStreamInfo);
        }
        hostInputSampleFormat = PaUtil_SelectClosestAvailableFormat(
                paOboeDefaultFormat, inputSampleFormat);
        paOboeStream->inputFormat = hostInputSampleFormat;
    } else {
        inputChannelCount = 0;
        inputSampleFormat = hostInputSampleFormat = paOboeDefaultFormat; /* Suppress 'uninitialised var' warnings. */
        paOboeStream->inputFormat = hostInputSampleFormat;
    }

    if (outputParameters) {
        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;

        /* Oboe supports alternate device specification with API>=28, but here we reject the use of
            paUseHostApiSpecificDeviceSpecification and stick with the default. Devices can be set via
            PaOboe_SetSelectedDevice. */
        if (outputParameters->device == paUseHostApiSpecificDeviceSpecification)
            return paInvalidDevice;

        /* check that output device can support outputChannelCount */
        if (outputChannelCount >
            hostApi->deviceInfos[outputParameters->device]->maxOutputChannels)
            return paInvalidChannelCount;

        /* validate outputStreamInfo */
        if (outputParameters->hostApiSpecificStreamInfo) {
            oboeParametersFromPAParameters((PaOboeStreamInfo *) outputParameters->hostApiSpecificStreamInfo);
        }
        hostOutputSampleFormat = PaUtil_SelectClosestAvailableFormat(
                paOboeDefaultFormat, outputSampleFormat);
        paOboeStream->outputFormat = hostOutputSampleFormat;
    } else {
        outputChannelCount = 0;
        outputSampleFormat = hostOutputSampleFormat = paOboeDefaultFormat;
        paOboeStream->outputFormat = hostOutputSampleFormat;
    }

    /* validate platform specific flags */
    if ((streamFlags & paPlatformSpecificFlags) != 0)
        return paInvalidFlag; /* unexpected platform specific flag */

    if (framesPerBuffer == paFramesPerBufferUnspecified) {
        if (outputParameters) {
            framesPerHostBuffer =
                    (unsigned long) (outputParameters->suggestedLatency * sampleRate);
        } else {
            framesPerHostBuffer =
                    (unsigned long) (inputParameters->suggestedLatency * sampleRate);
        }
    } else {
        framesPerHostBuffer = framesPerBuffer;
    }

    PaUtil_InitializeStreamRepresentation(&(paOboeStream->streamRepresentation),
                                            streamCallback ? &oboeHostApi->callbackStreamInterface : &oboeHostApi->blockingStreamInterface,
                                            streamCallback, userData);

    PaUtil_InitializeCpuLoadMeasurer(&(paOboeStream->cpuLoadMeasurer), sampleRate);

    error = PaUtil_InitializeBufferProcessor(&(paOboeStream->bufferProcessor),
                                             inputChannelCount,
                                             inputSampleFormat,
                                             hostInputSampleFormat,
                                             outputChannelCount,
                                             outputSampleFormat,
                                             hostOutputSampleFormat,
                                             sampleRate, streamFlags,
                                             framesPerBuffer,
                                             framesPerHostBuffer,
                                             paUtilFixedHostBufferSize,
                                             streamCallback, userData);
    if (error != paNoError)
        goto error;

    paOboeStream->streamRepresentation.streamInfo.sampleRate = sampleRate;
    paOboeStream->isBlocking = (streamCallback == nullptr);
    paOboeStream->framesPerHostCallback = framesPerHostBuffer;
    paOboeStream->bytesPerFrame = sizeof(paOboeDefaultFormat);
    paOboeStream->cbFlags = 0;
    paOboeStream->isStopped = true;
    paOboeStream->isActive = false;

    if (!(paOboeStream->isBlocking)) {}
//        PaUnixThreading_Initialize(); TODO: see if threading works with this version of PortAudio

    if (inputChannelCount > 0) {
        paOboeStream->streamRepresentation.streamInfo.inputLatency =
                ((PaTime) PaUtil_GetBufferProcessorInputLatencyFrames(
                        &(paOboeStream->bufferProcessor)) +
                        paOboeStream->framesPerHostCallback) / sampleRate;
        ENSURE(InitializeInputStream(paOboeStream, oboeHostApi, inputParameters->device,
                                     androidInputPreset, sampleRate, performanceMode, sharingMode, packageName,
                                     contentType, sampleRateConversionQuality),
               "Initializing input stream failed")
    }

    if (outputChannelCount > 0) {
        paOboeStream->streamRepresentation.streamInfo.outputLatency =
                ((PaTime) PaUtil_GetBufferProcessorOutputLatencyFrames(
                        &paOboeStream->bufferProcessor)
                 + paOboeStream->framesPerHostCallback) / sampleRate;
        ENSURE(InitializeOutputStream(paOboeStream, oboeHostApi, outputParameters->device,
                                      androidOutputUsage, sampleRate, performanceMode, sharingMode, packageName,
                                      contentType, sampleRateConversionQuality),
               "Initializing output stream failed");
    }

    *paStream = (PaStream *) paOboeStream;
    return error;

    error:
    if (paOboeStream)
        PaUtil_FreeMemory(paOboeStream);

    LOGE("[PaOboe - OpenStream]\t Error opening stream(s). Error code: %d", error);

    return error;
}


/**
 * \brief   Calls OboeEngine::closeStream, and then frees the memory that was allocated to sustain
 *          the stream(s). When CloseStream() is called, the multi-api layer ensures that the stream
 *          has already been stopped or aborted.
 * @param   paStream points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our PaOboeStream.
 * @return  paNoError, but warns in the logs if OboeEngine::closeStream failed.
 */
static PaError CloseStream(PaStream *paStream) {
    LOGV("[PaOboe - CloseStream]\t CloseStream called.");
    auto *paOboeStream = (PaOboeStream *) paStream;

    if (!(OboeEngine::getInstance().closeStream(paOboeStream)))
        LOGW("[PaOboe - CloseStream]\t Some errors have occurred in closing oboe streams - see OboeEngine::CloseStream logs.");

    PaUtil_TerminateBufferProcessor(&paOboeStream->bufferProcessor);
    PaUtil_TerminateStreamRepresentation(&paOboeStream->streamRepresentation);

    return paNoError;
}


/**
 * \brief   Allocates the memory of the buffers necessary to start a stream, both for output and
 *          input, then calls OboeEngine::startStream.
 * @param   paStream points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our PaOboeStream.
 * @return  paNoError if no errors occur, paUnanticipatedHostError if OboeEngine::startStream fails.
 */
static PaError StartStream(PaStream *paStream) {
    auto *paOboeStream = (PaOboeStream *) paStream;

    PaUtil_ResetBufferProcessor(&paOboeStream->bufferProcessor);

    //Checking if the stream(s) are already active.
    //TODO: check if it's working as expected (extensive testing needed, no problem spotted with situational tests)
    if (paOboeStream->isActive) {
        LOGW("[PaOboe - StartStream]\t Stream was already active, stopping...");
        StopStream(paStream);
        LOGW("[PaOboe - StartStream]\t Restarting...");
        StartStream(paStream);
    }

    paOboeStream->currentOutputBuffer = 0;
    paOboeStream->currentInputBuffer = 0;

    auto numBuffers = OboeEngine::getInstance().getNumberOfBuffers();
    /* Initialize buffers */
    for (int i = 0; i < numBuffers; ++i) {
        if (paOboeStream->hasOutput()) {
            memset(paOboeStream->outputBuffers[paOboeStream->currentOutputBuffer], 0,
                   paOboeStream->framesPerHostCallback * paOboeStream->bytesPerFrame *
                           paOboeStream->bufferProcessor.outputChannelCount);
            paOboeStream->currentOutputBuffer = (paOboeStream->currentOutputBuffer + 1) % numBuffers;
        }
        if (paOboeStream->hasInput()) {
            memset(paOboeStream->inputBuffers[paOboeStream->currentInputBuffer], 0,
                   paOboeStream->framesPerHostCallback * paOboeStream->bytesPerFrame *
                           paOboeStream->bufferProcessor.inputChannelCount);
            paOboeStream->currentInputBuffer = (paOboeStream->currentInputBuffer + 1) % numBuffers;
        }
    }

    if (!paOboeStream->isBlocking) {
        paOboeStream->callbackResult = paContinue;
        paOboeStream->oboeCallbackResult = DataCallbackResult::Continue;
    }

    paOboeStream->isStopped = false;
    paOboeStream->isActive = true;
    paOboeStream->doStop = false;
    paOboeStream->doAbort = false;

    if (!(OboeEngine::getInstance().startStream(paOboeStream)))
        return paUnanticipatedHostError;
    else
        return paNoError;
}


/**
 * \brief   Ends the stream callback, if the stream is not blocking, and calls
 *          OboeEngine::stopStream.
 * @param   paStream points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our PaOboeStream.
 * @return  paNoError if no errors occur, paUnanticipatedHostError if OboeEngine::stopStream fails.
 */
static PaError StopStream(PaStream *paStream) {
    PaError error = paNoError;
    auto *paOboeStream = (PaOboeStream *) paStream;

    if (paOboeStream->isStopped) {
        LOGW("[PaOboe - StopStream]\t Stream was already stopped.");
    } else {
        if (!(paOboeStream->isBlocking)) {
            paOboeStream->doStop = true;
        }
        if (!(OboeEngine::getInstance().stopStream(paOboeStream))) {
            LOGE("[PaOboe - StopStream]\t Couldn't stop the stream(s) correctly - see OboeEngine::StopStream logs.");
            error = paUnanticipatedHostError;
        }

        paOboeStream->isActive = false;
        paOboeStream->isStopped = true;
        if (paOboeStream->streamRepresentation.streamFinishedCallback != nullptr)
            paOboeStream->streamRepresentation.streamFinishedCallback(
                    paOboeStream->streamRepresentation.userData);
    }

    return error;
}


/**
 * \brief   Aborts the stream callback, if the stream is not blocking, and calls
 *          OboeEngine::abortStream.
 * @param   paStream points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our PaOboeStream.
 * @return  paNoError if no errors occur, paUnanticipatedHostError if OboeEngine::abortStream fails.
 */
static PaError AbortStream(PaStream *paStream) {
    PaError error = paNoError;
    auto *paOboeStream = (PaOboeStream *) paStream;
    LOGI("[PaOboe - AbortStream]\t Aborting stream.");

    if (!paOboeStream->isBlocking) {
        paOboeStream->doAbort = true;
    }

    /* stop immediately so enqueue has no effect */
    if (!(OboeEngine::getInstance().abortStream(paOboeStream))) {
        LOGE("[PaOboe - AbortStream]\t Couldn't abort the stream - see OboeEngine::abortStream logs.");
        error = paUnanticipatedHostError;
    }

    paOboeStream->isActive = false;
    paOboeStream->isStopped = true;
    if (paOboeStream->streamRepresentation.streamFinishedCallback != nullptr)
        paOboeStream->streamRepresentation.streamFinishedCallback(
                paOboeStream->streamRepresentation.userData);

    return error;
}


/**
 * \brief   Copies an input stream buffer by buffer, and calls OboeEngine::readStream.
 * @param   paStream points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our PaOboeStream;
 * @param   buffer is the address of the first sample of the buffer;
 * @param   frames is the total number of frames to read.
 * @return  paInternalError if OboeEngine::readStream fails, paNoError otherwise.
 */
static PaError ReadStream(PaStream *paStream, void *buffer, unsigned long frames) {
    auto *paOboeStream = (PaOboeStream *) paStream;
    void *userBuffer = buffer;
    unsigned framesToRead;
    PaError error = paNoError;

    while (frames > 0) {
        framesToRead = PA_MIN(paOboeStream->framesPerHostCallback, frames);

        if (!(OboeEngine::getInstance().readStream(paOboeStream, userBuffer, framesToRead *
                paOboeStream->bufferProcessor.inputChannelCount)))
            error = paInternalError;

        paOboeStream->currentInputBuffer = (paOboeStream->currentInputBuffer + 1) % paOboeStream->numInputBuffers;
        frames -= framesToRead;
    }

    return error;
}


/**
 * \brief   Copies an output stream buffer by buffer, and calls OboeEngine::writeStream.
 * @param   paStream points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our PaOboeStream;
 * @param   buffer is the address of the first sample of the buffer;
 * @param   frames is the total number of frames to write.
 * @return  paInternalError if OboeEngine::writeStream fails, paNoError otherwise.
 */
static PaError WriteStream(PaStream *paStream, const void *buffer, unsigned long frames) {
    auto *paOboeStream = (PaOboeStream *) paStream;
    const void *userBuffer = buffer;
    unsigned framesToWrite;
    PaError error = paNoError;

    while (frames > 0) {
        framesToWrite = PA_MIN(paOboeStream->framesPerHostCallback, frames);

        if (!(OboeEngine::getInstance().writeStream(paOboeStream, userBuffer, framesToWrite *
                paOboeStream->bufferProcessor.outputChannelCount)))
            error = paInternalError;

        paOboeStream->currentOutputBuffer = (paOboeStream->currentOutputBuffer + 1) % paOboeStream->numOutputBuffers;
        frames -= framesToWrite;
    }

    return error;
}


/*-------------------------------- PaSkeleton Secondary Functions --------------------------------*/

/**
 * \brief   Function needed by portaudio to understand how many frames can be read without waiting.
 * @param   paStream points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our PaOboeStream.
 * @return  the minimum number of frames that can be read without waiting.
 */
static signed long GetStreamReadAvailable(PaStream *paStream) {
    auto *paOboeStream = (PaOboeStream *) paStream;
    return paOboeStream->framesPerHostCallback * (paOboeStream->numInputBuffers - paOboeStream->currentInputBuffer);
}


/**
 * \brief   Function needed by portaudio to understand how many frames can be written without waiting.
 * @param   paStream points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our PaOboeStream.
 * @return  the minimum number of frames that can be written without waiting.
 */
static signed long GetStreamWriteAvailable(PaStream *paStream) {
    auto *paOboeStream = (PaOboeStream *) paStream;
    return paOboeStream->framesPerHostCallback * (paOboeStream->numOutputBuffers - paOboeStream->currentOutputBuffer);
}


/**
 * \brief   Function needed by portaudio to understand if the stream is stopped.
 * @param   paStream points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our PaOboeStream.
 * @return  one (1) when the stream is stopped, or zero (0) when the stream is running.
 */
static PaError IsStreamStopped(PaStream *paStream) {
    auto *paOboeStream = (PaOboeStream *) paStream;
    return paOboeStream->isStopped;
}


/**
 * \brief   Function needed by portaudio to understand if the stream is active.
 * @param   paStream points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our PaOboeStream.
 * @return  one (1) when the stream is active (ie playing or recording audio), or zero (0) otherwise.
 */
static PaError IsStreamActive(PaStream *paStream) {
    auto *paOboeStream = (PaOboeStream *) paStream;
    return paOboeStream->isActive;
}


/**
 * \brief   Function needed by portaudio to get the stream time in seconds.
 * @param   paStream points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our PaOboeStream.
 * @return  The stream's current time in seconds, or 0 if an error occurred.
 */
static PaTime GetStreamTime(PaStream *paStream) {
    return PaUtil_GetTime();
}


/**
 * \brief   Function needed by portaudio to retrieve CPU usage information for the specified stream.
 * @param   paStream points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our PaOboeStream.
 * @return  A floating point value, typically between 0.0 and 1.0, where 1.0 indicates that the
 *          stream callback is consuming the maximum number of CPU cycles possible to maintain
 *          real-time operation. A value of 0.5 would imply that PortAudio and the stream callback
 *          was consuming roughly 50% of the available CPU time. The return value may exceed 1.0.
 *          A value of 0.0 will always be returned for a blocking read/write stream, or if an error
 *          occurs.
 */
static double GetStreamCpuLoad(PaStream *paStream) {
    auto *paOboeStream = (PaOboeStream *) paStream;
    return PaUtil_GetCpuLoad(&paOboeStream->cpuLoadMeasurer);
}

/*----------------------------- Implementation of PaOboe.h functions -----------------------------*/

PaErrorCode PaOboe_RegisterDevice(const char* name, int32_t id, PaOboe_Direction direction, int channelCount, int sampleRate) {
    return OboeEngine::getInstance().registerDevice(name, id, direction, channelCount, sampleRate);
}

PaErrorCode PaOboe_SetNativeBufferSize(unsigned long bufferSize) {
    return OboeEngine::getInstance().setNativeBufferSize(bufferSize);
}

PaErrorCode PaOboe_SetNumberOfBuffers(unsigned numberOfBuffers) {
    return OboeEngine::getInstance().setNumberOfBuffers(numberOfBuffers);
}

void PaOboe_InitializeStreamInfo( PaOboeStreamInfo *info )
{
    info->size = sizeof (PaOboeStreamInfo);
    info->hostApiType = paOboe;
    info->version = 1;
}
