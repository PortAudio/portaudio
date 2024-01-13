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
            PaUtil_SetLastHostErrorInfo(paInDevelopment, err, errorText); \
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

static unsigned long GetApproximateLowBufferSize();

// Commonly used parameters initialized.
static unsigned long paOboe_nativeBufferSize = 0;
static unsigned paOboe_numberOfBuffers = 2;

using namespace oboe;

//Useful global variables
int32_t paOboe_inputDeviceId = kUnspecified;
int32_t paOboe_outputDeviceId = kUnspecified;

PerformanceMode paOboe_inputPerformanceMode = PerformanceMode::LowLatency;
PerformanceMode paOboe_outputPerformanceMode = PerformanceMode::LowLatency;

class OboeEngine;
class OboeMediator;

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

    unsigned long framesPerHostCallback;
    unsigned bytesPerFrame;

    OboeMediator* oboeMediator;
} PaOboeStream;


class OboeMediator: public AudioStreamCallback{
public:
    OboeMediator(PaOboeStream* paOboeStream) {
        mOboeCallbackStream = paOboeStream;
    }

    //Callback function for non-blocking streams
    DataCallbackResult onAudioReady(AudioStream *oboeStream, void *audioData,
                                    int32_t numFrames) override;
    //Callback utils
    void onErrorAfterClose(AudioStream *oboeStream, oboe::Result error) override;
    void resetCallbackCounters();
    void setOutputCallback() { mOutputBuilder.setDataCallback(this)->setErrorCallback(this); }
    void setInputCallback() { mInputBuilder.setDataCallback(this)->setErrorCallback(this); }

    //getter and setter of mOboeEngine and mOboeCallbackStream
    OboeEngine *getEngine() { return mOboeEngine; }
    void setEngine(OboeEngine *oboeEngine) { mOboeEngine = oboeEngine; }

    PaOboeStream *getStreamAddress() { return mOboeCallbackStream; }
    void setCallbackStream(PaOboeStream *paOboeStream) { mOboeCallbackStream = paOboeStream; }

    //The only instances of output and input streams that will be used, and their builders
    std::shared_ptr <AudioStream> mOutputStream;
    AudioStreamBuilder mOutputBuilder;
    std::shared_ptr <AudioStream> mInputStream;
    AudioStreamBuilder mInputBuilder;

private:
    OboeEngine *mOboeEngine;

    //callback utils
    PaOboeStream *mOboeCallbackStream;
    unsigned long mFramesProcessed{};
    PaStreamCallbackTimeInfo mTimeInfo{};
    struct timespec mTimeSpec{};
};


/**
 * Stream engine of the host API - Oboe. We allocate only one instance of the engine per PaOboeStream, and
 * we call its functions when we want to operate directly on Oboe. More information on each function is
 * provided right before its implementation.
 */
class OboeEngine {
public:
    OboeEngine();

    //Stream-managing functions
    bool tryStream(Direction direction, int32_t sampleRate, int32_t channelCount);

    PaError openStream(PaOboeStream *paOboeStream, Direction direction, int32_t sampleRate,
                       Usage outputUsage, InputPreset inputPreset);

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

private:
    OboeMediator* mTerminableMediator;

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
 * \brief   Tries to open a stream with the direction direction, sample rate sampleRate and/or
 *          channel count channelCount. It then closes the stream. It's used to see if the requested
 *          parameters are supported by the devices that are going to be used.
 * @param   direction the Direction of the stream;
 * @param   sampleRate the sample rate we want to try;
 * @param   channelCount the channel count we want to try;
 * @return  true if the stream was opened with Result::OK, false otherwise.
 */
bool OboeEngine::tryStream(Direction direction, int32_t sampleRate, int32_t channelCount) {
    Result result;

    std::shared_ptr <AudioStream> oboeStream;
    AudioStreamBuilder builder;

    builder.setDeviceId(getSelectedDevice(direction))
            // Arbitrary format usually broadly supported. Later, we'll open streams with correct formats.
            // FIXME: if needed, modify this format to whatever you require
            ->setFormat(AudioFormat::Float)
            ->setDirection(direction)
            ->setSampleRate(sampleRate)
            ->setChannelCount(channelCount)
            ->openStream(oboeStream);

    if (result != Result::OK) {
        LOGE("[OboeEngine::TryStream]\t Couldn't open the stream in TryStream. Error: %s",
             convertToText(result));
        return false;
    }

    oboeStream->close();

    return true;
}


/**
 * \brief   Opens an audio stream with a specific direction, sample rate and,
 *          depending on the direction of the stream, sets its usage (if
 *          direction == Direction::Output) or its preset (if direction == Direction::Input).
 *          Moreover, this function checks if the stream is blocking, and sets its callback
 *          function if not.
 * @param   paOboeStream The stream we want to open
 * @param   direction The Oboe::Direction of the stream we want to open;
 * @param   sampleRate The sample rate of the stream we want to open;
 * @param   androidOutputUsage The Oboe::Usage of the output stream we want to open
 *              (only matters with Android Api level >= 28);
 * @param   androidInputPreset The Preset of the input stream we want to open
 *              (only matters with Android Api level >= 28).
 * @return  paNoError if everything goes as expected, paUnanticipatedHostError if Oboe fails to open
 *          a stream, and paInsufficientMemory if the memory allocation of the buffers fails.
 */
PaError OboeEngine::openStream(PaOboeStream *paOboeStream, Direction direction, int32_t sampleRate,
                               Usage androidOutputUsage, InputPreset androidInputPreset) {
    PaError error = paNoError;
    Result result;

    if (paOboeStream == nullptr) {
        LOGE("[OboeEngine::openStream]\t paOboeStream is a nullptr.");
        return paInternalError;
    }

    OboeMediator* mediator = paOboeStream->oboeMediator;

    if(!(paOboeStream->isBlocking)){
        mediator->resetCallbackCounters();
    }

    if (direction == Direction::Input) {
        mediator->mInputBuilder.setChannelCount(paOboeStream->bufferProcessor.inputChannelCount)
                ->setFormat(PaToOboeFormat(paOboeStream->inputFormat))
                ->setSampleRate(sampleRate)
                ->setDirection(Direction::Input)
                ->setDeviceId(getSelectedDevice(Direction::Input))
                ->setPerformanceMode(paOboe_inputPerformanceMode)
                ->setInputPreset(androidInputPreset)
                ->setFramesPerCallback(paOboeStream->framesPerHostCallback);

        if (!(paOboeStream->isBlocking)) {
            mediator->setInputCallback();
        }

        result = mediator->mInputBuilder.openStream(mediator->mInputStream);

        if (result != Result::OK) {
            LOGE("[OboeEngine::OpenStream]\t Oboe couldn't open the input stream: %s",
                 convertToText(result));
            return paUnanticipatedHostError;
        }

        mediator->mInputStream->setBufferSizeInFrames(mediator->mInputStream->getFramesPerBurst() *
                                                               paOboe_numberOfBuffers);
        paOboeStream->inputBuffers =
                (void **) PaUtil_AllocateZeroInitializedMemory(paOboe_numberOfBuffers * sizeof(int32_t * ));

        for (int i = 0; i < paOboe_numberOfBuffers; ++i) {
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
        mediator->mOutputBuilder.setChannelCount(paOboeStream->bufferProcessor.outputChannelCount)
                ->setFormat(PaToOboeFormat(paOboeStream->outputFormat))
                ->setSampleRate(sampleRate)
                ->setDirection(Direction::Output)
                ->setDeviceId(getSelectedDevice(Direction::Output))
                ->setPerformanceMode(paOboe_outputPerformanceMode)
                ->setUsage(androidOutputUsage)
                ->setFramesPerCallback(paOboeStream->framesPerHostCallback);

        if (!(paOboeStream->isBlocking)) {
            mediator->setOutputCallback();
        }

        result = mediator->mOutputBuilder.openStream(mediator->mOutputStream);
        if (result != Result::OK) {
            LOGE("[OboeEngine::OpenStream]\t Oboe couldn't open the output stream: %s",
                 convertToText(result));
            return paUnanticipatedHostError;
        }

        mediator->mOutputStream->setBufferSizeInFrames(mediator->mOutputStream->getFramesPerBurst() *
                                                                paOboe_numberOfBuffers);
        paOboeStream->outputBuffers =
                (void **) PaUtil_AllocateZeroInitializedMemory(paOboe_numberOfBuffers * sizeof(int32_t * ));

        for (int i = 0; i < paOboe_numberOfBuffers; ++i) {
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
    OboeMediator* mediator = paOboeStream->oboeMediator;

    if (paOboeStream->hasInput) {
        inputResult = mediator->mInputStream->requestStart();
        if (inputResult != Result::OK)
            LOGE("[OboeEngine::startStream]\t Oboe couldn't start the input stream: %s",
                 convertToText(inputResult));
    }
    if (paOboeStream->hasOutput) {
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
    OboeMediator* mediator = paOboeStream->oboeMediator;

    if (paOboeStream->hasInput) {
        inputResult = mediator->mInputStream->requestStop();
        if (inputResult != Result::OK)
            LOGE("[OboeEngine::stopStream]\t Oboe couldn't stop the input stream: %s",
                 convertToText(inputResult));
    }
    if (paOboeStream->hasOutput) {
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
    OboeMediator* mediator = paOboeStream->oboeMediator;

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

    OboeMediator* mediator = paOboeStream->oboeMediator;

    if (paOboeStream->hasOutput) {
        outputResult = mediator->mOutputStream->close();
        if (outputResult == Result::ErrorClosed) {
            outputResult = Result::OK;
            LOGW("[OboeEngine::closeStream]\t Tried to close output stream, but was already closed.");
        }
    }
    if (paOboeStream->hasInput) {
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

    OboeMediator* mediator = paOboeStream->oboeMediator;

    if (paOboeStream->hasInput) {
        inputResult = mediator->mInputStream->stop();
        if (inputResult != Result::OK)
            LOGE("[OboeEngine::abortStream]\t Couldn't force the input stream to stop: %s",
                 convertToText(inputResult));
        inputResult = mediator->mInputStream->close();
        if (inputResult != Result::OK)
            LOGE("[OboeEngine::abortStream]\t Couldn't force the input stream to close: %s",
                 convertToText(inputResult));
    }
    if (paOboeStream->hasOutput) {
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
    OboeMediator* mediator = paOboeStream->oboeMediator;

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
    OboeMediator* mediator = paOboeStream->oboeMediator;

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
    mTerminableMediator = paOboeStream->oboeMediator = new OboeMediator(paOboeStream);
    paOboeStream->oboeMediator->setEngine(this);
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


/**
 * \brief   Function used to implement device selection. Device Ids are kUnspecified by default, but
 *          can be set to something different via JNI using the function PaOboe_SetSelectedDevice.
 * @param   direction the Oboe::Direction for which we want to know the device Id.
 * @return  the device Id of the appropriate direction.
 */
int32_t OboeEngine::getSelectedDevice(Direction direction) {
    if (direction == Direction::Input)
        return paOboe_inputDeviceId;
    else
        return paOboe_outputDeviceId;
}

/*----------------------------- OboeMediator functions implementations -----------------------------*/
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

    if (mOboeCallbackStream->hasOutput) {
        mOboeCallbackStream->outputBuffers[mOboeCallbackStream->currentOutputBuffer] = audioData;
        PaUtil_SetOutputFrameCount(&mOboeCallbackStream->bufferProcessor, numFrames);
        PaUtil_SetInterleavedOutputChannels(&mOboeCallbackStream->bufferProcessor, 0,
                                            (void *) ((PaInt16 **) mOboeCallbackStream->outputBuffers)[
                                                    mOboeCallbackStream->currentOutputBuffer],
                                            0);
    }
    if (mOboeCallbackStream->hasInput) {
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
        if (mOboeCallbackStream->hasOutput) {
            mOboeCallbackStream->currentOutputBuffer =
                    (mOboeCallbackStream->currentOutputBuffer + 1) % paOboe_numberOfBuffers;
        }
        if (mOboeCallbackStream->hasInput) {
            mOboeCallbackStream->currentInputBuffer = (mOboeCallbackStream->currentInputBuffer + 1) % paOboe_numberOfBuffers;
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
        OboeEngine* oboeEngine = getEngine();
        LOGW("[OboeMediator::onErrorAfterClose]\t ErrorDisconnected - Restarting stream(s)");
        int i = 0;
        if(mOboeCallbackStream->hasOutput)
            i++;
        if(mOboeCallbackStream->hasInput)
            i+=2;
        if (!oboeEngine->restartStream(mOboeCallbackStream, i))
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
 * \brief   Checks if the requested sample rate is supported by the output device using
 *          OboeEngine::tryStream.
 *          This function is used by PaOboe_Initialize, IsFormatSupported, and OpenStream.
 * @param   oboeHostApi points towards a OboeHostApiRepresentation (see struct defined at the top of this file);
 * @param   sampleRate is the sample rate we want to check.
 * @return  PaNoError regardless of the outcome of the check, but warns in the Logs if the sample
 *          rate was changed by Oboe.
 */
PaError IsOutputSampleRateSupported(PaOboeHostApiRepresentation *oboeHostApi, double sampleRate) {
    if (!(oboeHostApi->oboeEngine->tryStream(Direction::Output,
                                               sampleRate,
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
PaError IsInputSampleRateSupported(PaOboeHostApiRepresentation *oboeHostApi, double sampleRate) {
    if (!(oboeHostApi->oboeEngine->tryStream(Direction::Input,
                                               sampleRate,
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
static PaError IsOutputChannelCountSupported(PaOboeHostApiRepresentation *oboeHostApi, int32_t numOfChannels) {
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
static PaError IsInputChannelCountSupported(PaOboeHostApiRepresentation *oboeHostApi, int32_t numOfChannels) {
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

    *hostApi = &oboeHostApi->inheritedHostApiRep;
    // Info initialization.
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paInDevelopment;
    (*hostApi)->info.name = "android Oboe";
    (*hostApi)->info.defaultOutputDevice = 0;
    (*hostApi)->info.defaultInputDevice = 0;
    (*hostApi)->info.deviceCount = 0;

    deviceCount = 1;
    (*hostApi)->deviceInfos = (PaDeviceInfo **) PaUtil_GroupAllocateZeroInitializedMemory(
            oboeHostApi->allocations, sizeof(PaDeviceInfo * ) * deviceCount);

    if (!(*hostApi)->deviceInfos) {
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

        /* If the user has set paOboe_nativeBufferSize by querying the optimal buffer size via java,
           use the user-defined value since that will offer the lowest possible latency. */

        if (paOboe_nativeBufferSize != 0) {
            deviceInfo->defaultLowInputLatency =
                    (double) paOboe_nativeBufferSize / deviceInfo->defaultSampleRate;
            deviceInfo->defaultLowOutputLatency =
                    (double) paOboe_nativeBufferSize / deviceInfo->defaultSampleRate;
            deviceInfo->defaultHighInputLatency =
                    (double) paOboe_nativeBufferSize * 4 / deviceInfo->defaultSampleRate;
            deviceInfo->defaultHighOutputLatency =
                    (double) paOboe_nativeBufferSize * 4 / deviceInfo->defaultSampleRate;
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

        (*hostApi)->deviceInfos[i] = deviceInfo;
        ++(*hostApi)->info.deviceCount;
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
            if (androidInputPreset != InputPreset::Generic &&
                androidInputPreset != InputPreset::Camcorder &&
                androidInputPreset != InputPreset::VoiceRecognition &&
                androidInputPreset != InputPreset::VoiceCommunication &&
                androidInputPreset != InputPreset::VoicePerformance) {
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
        if (IsOutputSampleRateSupported(oboeHostApi, sampleRate) != paNoError) {
            return paInvalidSampleRate;
        }
    }
    if (inputChannelCount > 0) {
        if (IsInputSampleRateSupported(oboeHostApi, sampleRate) != paNoError) {
            return paInvalidSampleRate;
        }
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
static PaError InitializeOutputStream(PaOboeStream* paOboeStream, PaOboeHostApiRepresentation *oboeHostApi,
                                      Usage androidOutputUsage, double sampleRate) {

    return oboeHostApi->oboeEngine->openStream(paOboeStream,
                                                 Direction::Output,
                                                 sampleRate,
                                                 androidOutputUsage,
                                                 Generic); //Input preset won't be used, so we put the default value.
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
static PaError InitializeInputStream(PaOboeStream* paOboeStream, PaOboeHostApiRepresentation *oboeHostApi,
                                     InputPreset androidInputPreset, double sampleRate) {

    return oboeHostApi->oboeEngine->openStream(paOboeStream,
                                                 Direction::Input,
                                                 sampleRate,
                                                 Usage::Media,   //Usage won't be used, so we put the default value.
                                                 androidInputPreset);
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
    Usage androidOutputUsage = Usage::VoiceCommunication;
    InputPreset androidInputPreset = InputPreset::Generic;

    PaOboeStream* paOboeStream = (PaOboeStream *) PaUtil_AllocateZeroInitializedMemory(sizeof(PaOboeStream));;
    oboeHostApi->oboeEngine->constructPaOboeStream(paOboeStream);

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
            // Only has an effect on ANDROID_API>=28.
            androidInputPreset =
                    static_cast<InputPreset>(
                                ((PaOboeStreamInfo *) outputParameters->hostApiSpecificStreamInfo)->androidInputPreset
                            );
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
            androidOutputUsage =
                    static_cast<Usage>(
                                ((PaOboeStreamInfo *) outputParameters->hostApiSpecificStreamInfo)->androidOutputUsage
                            );
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

    if (streamCallback) {
        PaUtil_InitializeStreamRepresentation(&(paOboeStream->streamRepresentation),
                                              &oboeHostApi->callbackStreamInterface,
                                              streamCallback, userData);
    } else {
        PaUtil_InitializeStreamRepresentation(&(paOboeStream->streamRepresentation),
                                              &oboeHostApi->blockingStreamInterface,
                                              streamCallback, userData);
    }

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
        paOboeStream->hasInput = true;
        paOboeStream->streamRepresentation.streamInfo.inputLatency =
                ((PaTime) PaUtil_GetBufferProcessorInputLatencyFrames(
                        &(paOboeStream->bufferProcessor)) +
                        paOboeStream->framesPerHostCallback) / sampleRate;
        ENSURE(InitializeInputStream(paOboeStream, oboeHostApi,
                                     androidInputPreset, sampleRate),
               "Initializing input stream failed")
    } else { paOboeStream->hasInput = false; }

    if (outputChannelCount > 0) {
        paOboeStream->hasOutput = true;
        paOboeStream->streamRepresentation.streamInfo.outputLatency =
                ((PaTime) PaUtil_GetBufferProcessorOutputLatencyFrames(
                        &paOboeStream->bufferProcessor)
                 + paOboeStream->framesPerHostCallback) / sampleRate;
        ENSURE(InitializeOutputStream(paOboeStream, oboeHostApi,
                                      androidOutputUsage, sampleRate),
               "Initializing output stream failed");
    } else { paOboeStream->hasOutput = false; }

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
    auto *paOboeStream = (PaOboeStream *) paStream;
    auto *oboeEngine = paOboeStream->oboeMediator->getEngine();

    if (!(oboeEngine->closeStream(paOboeStream)))
        LOGW("[PaOboe - CloseStream]\t Some errors have occurred in closing oboe streams - see OboeEngine::CloseStream logs.");

    PaUtil_TerminateBufferProcessor(&paOboeStream->bufferProcessor);
    PaUtil_TerminateStreamRepresentation(&paOboeStream->streamRepresentation);

    for (int i = 0; i < paOboe_numberOfBuffers; ++i) {
        if (paOboeStream->hasOutput)
            PaUtil_FreeMemory(paOboeStream->outputBuffers[i]);
        if (paOboeStream->hasInput)
            PaUtil_FreeMemory(paOboeStream->inputBuffers[i]);
    }

    if (paOboeStream->hasOutput)
        PaUtil_FreeMemory(paOboeStream->outputBuffers);
    if (paOboeStream->hasInput)
        PaUtil_FreeMemory(paOboeStream->inputBuffers);

    PaUtil_FreeMemory(paOboeStream);
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
    auto *oboeEngine = paOboeStream->oboeMediator->getEngine();

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

    /* Initialize buffers */
    for (int i = 0; i < paOboe_numberOfBuffers; ++i) {
        if (paOboeStream->hasOutput) {
            memset(paOboeStream->outputBuffers[paOboeStream->currentOutputBuffer], 0,
                   paOboeStream->framesPerHostCallback * paOboeStream->bytesPerFrame *
                           paOboeStream->bufferProcessor.outputChannelCount);
            paOboeStream->currentOutputBuffer = (paOboeStream->currentOutputBuffer + 1) % paOboe_numberOfBuffers;
        }
        if (paOboeStream->hasInput) {
            memset(paOboeStream->inputBuffers[paOboeStream->currentInputBuffer], 0,
                   paOboeStream->framesPerHostCallback * paOboeStream->bytesPerFrame *
                           paOboeStream->bufferProcessor.inputChannelCount);
            paOboeStream->currentInputBuffer = (paOboeStream->currentInputBuffer + 1) % paOboe_numberOfBuffers;
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

    if (!(oboeEngine->startStream(paOboeStream)))
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
    auto *oboeEngine = paOboeStream->oboeMediator->getEngine();

    if (paOboeStream->isStopped) {
        LOGW("[PaOboe - StopStream]\t Stream was already stopped.");
    } else {
        if (!(paOboeStream->isBlocking)) {
            paOboeStream->doStop = true;
        }
        if (!(oboeEngine->stopStream(paOboeStream))) {
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
    auto *oboeEngine = paOboeStream->oboeMediator->getEngine();
    LOGI("[PaOboe - AbortStream]\t Aborting stream.");

    if (!paOboeStream->isBlocking) {
        paOboeStream->doAbort = true;
    }

    /* stop immediately so enqueue has no effect */
    if (!(oboeEngine->abortStream(paOboeStream))) {
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
    auto *oboeEngine = paOboeStream->oboeMediator->getEngine();
    void *userBuffer = buffer;
    unsigned framesToRead;
    PaError error = paNoError;

    while (frames > 0) {
        framesToRead = PA_MIN(paOboeStream->framesPerHostCallback, frames);

        if (!(oboeEngine->readStream(paOboeStream, userBuffer, framesToRead *
                paOboeStream->bufferProcessor.inputChannelCount)))
            error = paInternalError;

        paOboeStream->currentInputBuffer = (paOboeStream->currentInputBuffer + 1) % paOboe_numberOfBuffers;
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
    auto *oboeEngine = paOboeStream->oboeMediator->getEngine();
    const void *userBuffer = buffer;
    unsigned framesToWrite;
    PaError error = paNoError;

    while (frames > 0) {
        framesToWrite = PA_MIN(paOboeStream->framesPerHostCallback, frames);

        if (!(oboeEngine->writeStream(paOboeStream, userBuffer, framesToWrite *
                paOboeStream->bufferProcessor.outputChannelCount)))
            error = paInternalError;

        paOboeStream->currentOutputBuffer = (paOboeStream->currentOutputBuffer + 1) % paOboe_numberOfBuffers;
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
    return paOboeStream->framesPerHostCallback * (paOboe_numberOfBuffers - paOboeStream->currentInputBuffer);
}


/**
 * \brief   Function needed by portaudio to understand how many frames can be written without waiting.
 * @param   paStream points to to a PaStream, which is an audio stream structure used and built by
 *              portaudio, which holds the information of our PaOboeStream.
 * @return  the minimum number of frames that can be written without waiting.
 */
static signed long GetStreamWriteAvailable(PaStream *paStream) {
    auto *paOboeStream = (PaOboeStream *) paStream;
    return paOboeStream->framesPerHostCallback * (paOboe_numberOfBuffers - paOboeStream->currentOutputBuffer);
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

void PaOboe_SetSelectedDevice(PaOboe_Direction direction, int32_t deviceID) {
    LOGI("[PaOboe - SetSelectedDevice] Selecting device...");
    if (static_cast<Direction>(direction) == Direction::Input)
        paOboe_inputDeviceId = deviceID;
    else
        paOboe_outputDeviceId = deviceID;
}

void PaOboe_SetPerformanceMode(PaOboe_Direction direction, PaOboe_PerformanceMode performanceMode) {
    if (static_cast<Direction>(direction) == Direction::Input) {
        paOboe_inputPerformanceMode = static_cast<PerformanceMode>(performanceMode);
    } else {
        paOboe_outputPerformanceMode = static_cast<PerformanceMode>(performanceMode);
    }
}

void PaOboe_SetNativeBufferSize(unsigned long bufferSize) {
    paOboe_nativeBufferSize = bufferSize;
}

void PaOboe_SetNumberOfBuffers(unsigned numberOfBuffers) {
    paOboe_numberOfBuffers = numberOfBuffers;
}
