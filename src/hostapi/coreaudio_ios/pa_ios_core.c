/*
 * Implementation of the PortAudio API for Apple AUHAL
 *
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 *
 * Written by Bjorn Roche of XO Audio LLC, from PA skeleton code.
 * Portions copied from code by Dominic Mazzoni (who wrote a HAL implementation)
 *
 * Dominic's code was based on code by Phil Burk, Darren Gibbs,
 * Gord Peters, Stephane Letz, and Greg Pfiel.
 *
 * The following people also deserve acknowledgements:
 *
 * Olivier Tristan for feedback and testing
 * Glenn Zelniker and Z-Systems engineering for sponsoring the Blocking I/O
 * interface.
 *
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

/**
 @file pa_ios_core
 @ingroup hostapi_src
 @author Bjorn Roche
 @brief AUHAL implementation of PortAudio
*/

#include "pa_ios_core_internal.h"

#include <string.h>

#include <libkern/OSAtomic.h>

#include <mach/mach_time.h>

#include "pa_ios_core.h"
#include "pa_ios_core_utilities.h"
#include "pa_ios_core_blocking.h"

/* prototypes for functions declared in this file */
PaError	PaIosCore_Initialize(PaUtilHostApiRepresentation ** hostApi, PaHostApiIndex index);

static void Terminate(struct PaUtilHostApiRepresentation *hostApi);
static	PaError IsFormatSupported(struct PaUtilHostApiRepresentation *hostApi,
    const PaStreamParameters * inputParameters,
    const PaStreamParameters * outputParameters,
    double sampleRate);
static	PaError OpenStream(struct PaUtilHostApiRepresentation *hostApi,
    PaStream ** s,
    const PaStreamParameters * inputParameters,
    const PaStreamParameters * outputParameters,
    double sampleRate,
    unsigned long framesPerBuffer,
    PaStreamFlags streamFlags,
    PaStreamCallback * streamCallback,
    void *userData);
static PaError CloseStream(PaStream * stream);
static PaError StartStream(PaStream * stream);
static PaError StopStream(PaStream * stream);
static PaError AbortStream(PaStream * stream);
static PaError IsStreamStopped(PaStream * s);
static PaError IsStreamActive(PaStream * stream);
static PaTime GetStreamTime(PaStream * stream);
static OSStatus AudioIOProc(void *inRefCon,
    AudioUnitRenderActionFlags * ioActionFlags,
    const AudioTimeStamp * inTimeStamp,
    UInt32 inBusNumber,
    UInt32 inNumberFrames,
    AudioBufferList * ioData);
static double GetStreamCpuLoad(PaStream * stream);

/*
 * Callback called when starting or stopping a stream.
 */
static void
startStopCallback(
    void *inRefCon,
    AudioUnit ci,
    AudioUnitPropertyID inID,
    AudioUnitScope inScope,
    AudioUnitElement inElement)
{
	PaIosCoreStream *stream = (PaIosCoreStream *) inRefCon;
	UInt32 isRunning;
	UInt32 size = sizeof(isRunning);
	OSStatus err;

	err = AudioUnitGetProperty(ci, kAudioOutputUnitProperty_IsRunning, inScope, inElement, &isRunning, &size);
	assert(!err);
	if (err)
		isRunning = false;
	if (isRunning)
		return;
	if (stream->inputUnit && stream->outputUnit && stream->inputUnit != stream->outputUnit && ci == stream->inputUnit)
		return;
	PaStreamFinishedCallback *sfc = stream->streamRepresentation.streamFinishedCallback;

	if (stream->state == STOPPING)
		stream->state = STOPPED;
	if (sfc)
		sfc(stream->streamRepresentation.userData);
}

static void
FillDeviceInfo(PaIosAUHAL * auhalHostApi,
    PaDeviceInfo * deviceInfo,
    PaHostApiIndex hostApiIndex)
{
	memset(deviceInfo, 0, sizeof(PaDeviceInfo));

	deviceInfo->structVersion = 2;
	deviceInfo->hostApi = hostApiIndex;
	deviceInfo->name = "Default";
	deviceInfo->defaultSampleRate = 48000;
	deviceInfo->maxInputChannels = 1;
	deviceInfo->maxOutputChannels = 2;

	deviceInfo->defaultLowInputLatency = 0.008;
	deviceInfo->defaultHighInputLatency = 0.080;
	deviceInfo->defaultLowOutputLatency = 0.008;
	deviceInfo->defaultHighOutputLatency = 0.080;
}

PaError
PaIosCore_Initialize(PaUtilHostApiRepresentation ** hostApi, PaHostApiIndex hostApiIndex)
{
	PaError result = paNoError;
	PaIosAUHAL *auhalHostApi = NULL;
	PaDeviceInfo *deviceInfoArray;

	auhalHostApi = (PaIosAUHAL *) PaUtil_AllocateMemory(sizeof(PaIosAUHAL));
	if (auhalHostApi == NULL) {
		result = paInsufficientMemory;
		goto error;
	}
	auhalHostApi->allocations = PaUtil_CreateAllocationGroup();
	if (auhalHostApi->allocations == NULL) {
		result = paInsufficientMemory;
		goto error;
	}
	*hostApi = &auhalHostApi->inheritedHostApiRep;

	(*hostApi)->info.structVersion = 1;
	(*hostApi)->info.type = paCoreAudio;
	(*hostApi)->info.name = "iOS Audio";
	(*hostApi)->info.defaultInputDevice = 0;
	(*hostApi)->info.defaultOutputDevice = 0;
	(*hostApi)->info.deviceCount = 1;

	(*hostApi)->deviceInfos = (PaDeviceInfo **) PaUtil_GroupAllocateMemory(
	    auhalHostApi->allocations, sizeof(PaDeviceInfo *) * 1);

	if ((*hostApi)->deviceInfos == NULL) {
		result = paInsufficientMemory;
		goto error;
	}
	deviceInfoArray = (PaDeviceInfo *) PaUtil_GroupAllocateMemory(
	    auhalHostApi->allocations, sizeof(PaDeviceInfo) * 1);
	if (deviceInfoArray == NULL) {
		result = paInsufficientMemory;
		goto error;
	}
	FillDeviceInfo(auhalHostApi, &deviceInfoArray[0], hostApiIndex);

	(*hostApi)->deviceInfos[0] = &deviceInfoArray[0];
	(*hostApi)->Terminate = Terminate;
	(*hostApi)->OpenStream = OpenStream;
	(*hostApi)->IsFormatSupported = IsFormatSupported;

	PaUtil_InitializeStreamInterface(
	    &auhalHostApi->callbackStreamInterface,
	    CloseStream, StartStream,
	    StopStream, AbortStream, IsStreamStopped,
	    IsStreamActive,
	    GetStreamTime, GetStreamCpuLoad,
	    PaUtil_DummyRead, PaUtil_DummyWrite,
	    PaUtil_DummyGetReadAvailable,
	    PaUtil_DummyGetWriteAvailable);

	PaUtil_InitializeStreamInterface(
	    &auhalHostApi->blockingStreamInterface,
	    CloseStream, StartStream,
	    StopStream, AbortStream, IsStreamStopped,
	    IsStreamActive,
	    GetStreamTime, PaUtil_DummyGetCpuLoad,
	    ReadStream, WriteStream,
	    GetStreamReadAvailable,
	    GetStreamWriteAvailable);

	return (result);

error:
	if (auhalHostApi != NULL) {
		if (auhalHostApi->allocations != NULL) {
			PaUtil_FreeAllAllocations(auhalHostApi->allocations);
			PaUtil_DestroyAllocationGroup(auhalHostApi->allocations);
		}
		PaUtil_FreeMemory(auhalHostApi);
	}
	return (result);
}

static void
Terminate(struct PaUtilHostApiRepresentation *hostApi)
{
	PaIosAUHAL *auhalHostApi = (PaIosAUHAL *) hostApi;

	if (auhalHostApi->allocations) {
		PaUtil_FreeAllAllocations(auhalHostApi->allocations);
		PaUtil_DestroyAllocationGroup(auhalHostApi->allocations);
	}
	PaUtil_FreeMemory(auhalHostApi);
}

static	PaError
IsFormatSupported(struct PaUtilHostApiRepresentation *hostApi,
    const PaStreamParameters * inputParameters,
    const PaStreamParameters * outputParameters,
    double sampleRate)
{
	PaSampleFormat inputSampleFormat;
	PaSampleFormat outputSampleFormat;
	int inputChannelCount;
	int outputChannelCount;
	PaError err;
	PaStream *s;

	if (inputParameters) {
		inputChannelCount = inputParameters->channelCount;
		inputSampleFormat = inputParameters->sampleFormat;

		if (inputSampleFormat & paCustomFormat)
			return (paSampleFormatNotSupported);
		if (inputParameters->device == paUseHostApiSpecificDeviceSpecification)
			return (paInvalidDevice);
		if (inputChannelCount > hostApi->deviceInfos[inputParameters->device]->maxInputChannels)
			return (paInvalidChannelCount);
	} else {
		inputChannelCount = 0;
	}

	if (outputParameters) {
		outputChannelCount = outputParameters->channelCount;
		outputSampleFormat = outputParameters->sampleFormat;

		if (outputSampleFormat & paCustomFormat)
			return (paSampleFormatNotSupported);
		if (outputParameters->device == paUseHostApiSpecificDeviceSpecification)
			return (paInvalidDevice);
		if (outputChannelCount > hostApi->deviceInfos[outputParameters->device]->maxOutputChannels)
			return (paInvalidChannelCount);
	} else {
		outputChannelCount = 0;
	}

	err = OpenStream(hostApi, &s, inputParameters, outputParameters,
	    sampleRate, 1024, 0, (PaStreamCallback *) 1, NULL);
	if (err)
		return (err);

	(void)CloseStream(s);

	return paFormatIsSupported;
}

/* ================================================================================= */
static void
InitializeDeviceProperties(PaIosCoreDeviceProperties * deviceProperties)
{
	memset(deviceProperties, 0, sizeof(PaIosCoreDeviceProperties));
	deviceProperties->sampleRate = 1.0;	/* Better than random.
						 * Overwritten by actual
						 * values later on. */
	deviceProperties->samplePeriod = 1.0 / deviceProperties->sampleRate;
}

static	Float64
CalculateSoftwareLatencyFromProperties(PaIosCoreStream * stream, PaIosCoreDeviceProperties * deviceProperties)
{
	UInt32 latencyFrames = deviceProperties->bufferFrameSize + deviceProperties->deviceLatency + deviceProperties->safetyOffset;

	return latencyFrames * deviceProperties->samplePeriod;	/* same as dividing by
								 * sampleRate but faster */
}

static	Float64
CalculateHardwareLatencyFromProperties(PaIosCoreStream * stream, PaIosCoreDeviceProperties * deviceProperties)
{
	return deviceProperties->deviceLatency * deviceProperties->samplePeriod;	/* same as dividing by
											 * sampleRate but faster */
}

/* Calculate values used to convert Apple timestamps into PA timestamps
 * from the device properties. The final results of this calculation
 * will be used in the audio callback function.
 */
static void
UpdateTimeStampOffsets(PaIosCoreStream * stream)
{
	Float64 inputSoftwareLatency = 0.0;
	Float64 inputHardwareLatency = 0.0;
	Float64 outputSoftwareLatency = 0.0;
	Float64 outputHardwareLatency = 0.0;

	if (stream->inputUnit != NULL) {
		inputSoftwareLatency = CalculateSoftwareLatencyFromProperties(stream, &stream->inputProperties);
		inputHardwareLatency = CalculateHardwareLatencyFromProperties(stream, &stream->inputProperties);
	}
	if (stream->outputUnit != NULL) {
		outputSoftwareLatency = CalculateSoftwareLatencyFromProperties(stream, &stream->outputProperties);
		outputHardwareLatency = CalculateHardwareLatencyFromProperties(stream, &stream->outputProperties);
	}
	/* We only need a mutex around setting these variables as a group. */
	pthread_mutex_lock(&stream->timingInformationMutex);
	stream->timestampOffsetCombined = inputSoftwareLatency + outputSoftwareLatency;
	stream->timestampOffsetInputDevice = inputHardwareLatency;
	stream->timestampOffsetOutputDevice = outputHardwareLatency;
	pthread_mutex_unlock(&stream->timingInformationMutex);
}

static	PaError
OpenAndSetupOneAudioUnit(
    const PaIosCoreStream * stream,
    const PaStreamParameters * inStreamParams,
    const PaStreamParameters * outStreamParams,
    const UInt32 requestedFramesPerBuffer,
    UInt32 * actualInputFramesPerBuffer,
    UInt32 * actualOutputFramesPerBuffer,
    const PaIosAUHAL * auhalHostApi,
    AudioUnit * audioUnit,
    const double sampleRate,
    void *refCon)
{
	AudioComponentDescription desc;
	AudioComponent comp;
	AudioStreamBasicDescription desiredFormat;
	OSStatus result = noErr;
	PaError paResult = paNoError;
	int line = 0;
	UInt32 callbackKey;
	AURenderCallbackStruct rcbs;

	if (!inStreamParams && !outStreamParams) {
		*audioUnit = NULL;
		return paNoError;
	}
	desc.componentType = kAudioUnitType_Output;
	desc.componentSubType = kAudioUnitSubType_RemoteIO;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;
	desc.componentFlags = 0;
	desc.componentFlagsMask = 0;

	comp = AudioComponentFindNext(NULL, &desc);
	if (comp == NULL) {
		*audioUnit = NULL;
		return (paUnanticipatedHostError);
	}
	result = AudioComponentInstanceNew(comp, audioUnit);
	if (result) {
		*audioUnit = NULL;
		return (ERR(result));
	}
#define	ERR_WRAP(ios_err) do {			\
      result = ios_err;				\
      line = __LINE__ ;				\
      if (result != noErr)			\
		goto error;			\
} while(0)

	if (inStreamParams) {
		UInt32 enableIO = 1;

		ERR_WRAP(AudioUnitSetProperty(*audioUnit,
		    kAudioOutputUnitProperty_EnableIO,
		    kAudioUnitScope_Input,
		    INPUT_ELEMENT,
		    &enableIO,
		    sizeof(enableIO)));
	}
	if (!outStreamParams) {
		UInt32 enableIO = 0;

		ERR_WRAP(AudioUnitSetProperty(*audioUnit,
		    kAudioOutputUnitProperty_EnableIO,
		    kAudioUnitScope_Output,
		    OUTPUT_ELEMENT,
		    &enableIO,
		    sizeof(enableIO)));
	}
	if (inStreamParams && outStreamParams) {
		assert(outStreamParams->device == inStreamParams->device);
	}
	ERR_WRAP(AudioUnitAddPropertyListener(*audioUnit,
	    kAudioOutputUnitProperty_IsRunning,
	    startStopCallback,
	    (void *)stream));

	memset(&desiredFormat, 0, sizeof(desiredFormat));
	desiredFormat.mFormatID = kAudioFormatLinearPCM;
	desiredFormat.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
	desiredFormat.mFramesPerPacket = 1;
	desiredFormat.mBitsPerChannel = sizeof(float) * 8;

	result = 0;

	if (outStreamParams) {
		UInt32 value = kAudioConverterQuality_High;

		ERR_WRAP(AudioUnitSetProperty(*audioUnit,
		    kAudioUnitProperty_RenderQuality,
		    kAudioUnitScope_Global,
		    OUTPUT_ELEMENT,
		    &value,
		    sizeof(value)));
	}
	/* now set the format on the Audio Units. */
	if (outStreamParams) {
		desiredFormat.mSampleRate = sampleRate;
		desiredFormat.mBytesPerPacket = sizeof(float) * outStreamParams->channelCount;
		desiredFormat.mBytesPerFrame = sizeof(float) * outStreamParams->channelCount;
		desiredFormat.mChannelsPerFrame = outStreamParams->channelCount;
		ERR_WRAP(AudioUnitSetProperty(*audioUnit,
		    kAudioUnitProperty_StreamFormat,
		    kAudioUnitScope_Input,
		    OUTPUT_ELEMENT,
		    &desiredFormat,
		    sizeof(AudioStreamBasicDescription)));
	}
	if (inStreamParams) {
		AudioStreamBasicDescription sourceFormat;
		UInt32 size = sizeof(AudioStreamBasicDescription);

		/* keep the sample rate of the device, or we confuse AUHAL */
		ERR_WRAP(AudioUnitGetProperty(*audioUnit,
		    kAudioUnitProperty_StreamFormat,
		    kAudioUnitScope_Input,
		    INPUT_ELEMENT,
		    &sourceFormat,
		    &size));
		desiredFormat.mSampleRate = sampleRate;
		desiredFormat.mBytesPerPacket = sizeof(float) * inStreamParams->channelCount;
		desiredFormat.mBytesPerFrame = sizeof(float) * inStreamParams->channelCount;
		desiredFormat.mChannelsPerFrame = inStreamParams->channelCount;
		ERR_WRAP(AudioUnitSetProperty(*audioUnit,
		    kAudioUnitProperty_StreamFormat,
		    kAudioUnitScope_Output,
		    INPUT_ELEMENT,
		    &desiredFormat,
		    sizeof(AudioStreamBasicDescription)));
	}
	if (outStreamParams) {
		UInt32 size = sizeof(*actualOutputFramesPerBuffer);

		ERR_WRAP(AudioUnitSetProperty(*audioUnit,
		    kAudioUnitProperty_MaximumFramesPerSlice,
		    kAudioUnitScope_Input,
		    OUTPUT_ELEMENT,
		    &requestedFramesPerBuffer,
		    sizeof(requestedFramesPerBuffer)));
		ERR_WRAP(AudioUnitGetProperty(*audioUnit,
		    kAudioUnitProperty_MaximumFramesPerSlice,
		    kAudioUnitScope_Global,
		    OUTPUT_ELEMENT,
		    actualOutputFramesPerBuffer,
		    &size));
	}
	if (inStreamParams) {
		ERR_WRAP(AudioUnitSetProperty(*audioUnit,
		    kAudioUnitProperty_MaximumFramesPerSlice,
		    kAudioUnitScope_Output,
		    INPUT_ELEMENT,
		    &requestedFramesPerBuffer,
		    sizeof(requestedFramesPerBuffer)));

		*actualInputFramesPerBuffer = requestedFramesPerBuffer;
	}
	callbackKey = outStreamParams ? kAudioUnitProperty_SetRenderCallback :
	    kAudioOutputUnitProperty_SetInputCallback;
	rcbs.inputProc = AudioIOProc;
	rcbs.inputProcRefCon = refCon;
	ERR_WRAP(AudioUnitSetProperty(
	    *audioUnit,
	    callbackKey,
	    kAudioUnitScope_Output,
	    outStreamParams ? OUTPUT_ELEMENT : INPUT_ELEMENT,
	    &rcbs,
	    sizeof(rcbs)));

	/* initialize the audio unit */
	ERR_WRAP(AudioUnitInitialize(*audioUnit));

	return (paNoError);
#undef ERR_WRAP

error:
	AudioComponentInstanceDispose(*audioUnit);
	*audioUnit = NULL;
	if (result)
		return PaIosCore_SetError(result, line, 1);
	return (paResult);
}

static long
computeRingBufferSize(const PaStreamParameters * inputParameters,
    const PaStreamParameters * outputParameters,
    long inputFramesPerBuffer,
    long outputFramesPerBuffer,
    double sampleRate)
{
	long ringSize;
	int index;
	int i;
	double latency;
	long framesPerBuffer;

	assert(inputParameters || outputParameters);

	if (outputParameters && inputParameters) {
		latency = MAX(inputParameters->suggestedLatency, outputParameters->suggestedLatency);
		framesPerBuffer = MAX(inputFramesPerBuffer, outputFramesPerBuffer);
	} else if (outputParameters) {
		latency = outputParameters->suggestedLatency;
		framesPerBuffer = outputFramesPerBuffer;
	} else {
		latency = inputParameters->suggestedLatency;
		framesPerBuffer = inputFramesPerBuffer;
	}

	ringSize = (long)(latency * sampleRate * 2 + .5);

	if (ringSize < framesPerBuffer * 3)
		ringSize = framesPerBuffer * 3;

	/* make sure it's at least 4 */
	ringSize = MAX(ringSize, 4);

	/* round up to the next power of 2 */
	index = -1;

	for (i = 0; i != (sizeof(long) * 8); ++i)
		if ((ringSize >> i) & 0x01)
			index = i;
	assert(index > 0);

	if (ringSize <= (0x01 << index))
		ringSize = 0x01 << index;
	else
		ringSize = 0x01 << (index + 1);

	return ringSize;
}

static	PaError
OpenStream(struct PaUtilHostApiRepresentation *hostApi,
    PaStream ** s,
    const PaStreamParameters * inputParameters,
    const PaStreamParameters * outputParameters,
    double sampleRate,
    unsigned long requestedFramesPerBuffer,
    PaStreamFlags streamFlags,
    PaStreamCallback * streamCallback,
    void *userData)
{
	PaError result = paNoError;
	PaIosAUHAL *auhalHostApi = (PaIosAUHAL *) hostApi;
	PaIosCoreStream *stream = 0;
	int inputChannelCount;
	int outputChannelCount;
	PaSampleFormat inputSampleFormat;
	PaSampleFormat outputSampleFormat;
	PaSampleFormat hostInputSampleFormat;
	PaSampleFormat hostOutputSampleFormat;

	UInt32 inputLatencyFrames = 0;
	UInt32 outputLatencyFrames = 0;

	if (requestedFramesPerBuffer == paFramesPerBufferUnspecified)
		requestedFramesPerBuffer = sampleRate * 0.016;

	if (inputParameters) {
		inputChannelCount = inputParameters->channelCount;
		inputSampleFormat = inputParameters->sampleFormat;

		/* @todo Blocking read/write on Ios is not yet supported. */
		if (!streamCallback && inputSampleFormat & paNonInterleaved) {
			return paSampleFormatNotSupported;
		}
		/*
		 * unless alternate device specification is supported,
		 * reject the use of paUseHostApiSpecificDeviceSpecification
		 */

		if (inputParameters->device == paUseHostApiSpecificDeviceSpecification)
			return paInvalidDevice;

		/* check that input device can support inputChannelCount */
		if (inputChannelCount > hostApi->deviceInfos[inputParameters->device]->maxInputChannels)
			return paInvalidChannelCount;

		/* Host supports interleaved float32 */
		hostInputSampleFormat = paFloat32;
	} else {
		inputChannelCount = 0;
		inputSampleFormat = hostInputSampleFormat = paFloat32;
	}

	if (outputParameters) {
		outputChannelCount = outputParameters->channelCount;
		outputSampleFormat = outputParameters->sampleFormat;

		/* @todo Blocking read/write on Ios is not yet supported. */
		if (!streamCallback && outputSampleFormat & paNonInterleaved) {
			return paSampleFormatNotSupported;
		}
		/*
		 * unless alternate device specification is supported,
		 * reject the use of paUseHostApiSpecificDeviceSpecification
		 */

		if (outputParameters->device == paUseHostApiSpecificDeviceSpecification)
			return paInvalidDevice;

		/* check that output device can support inputChannelCount */
		if (outputChannelCount > hostApi->deviceInfos[outputParameters->device]->maxOutputChannels)
			return paInvalidChannelCount;

		/* Host supports interleaved float32 */
		hostOutputSampleFormat = paFloat32;
	} else {
		outputChannelCount = 0;
		outputSampleFormat = hostOutputSampleFormat = paFloat32;
	}

	/* validate platform specific flags */
	if ((streamFlags & paPlatformSpecificFlags) != 0)
		return paInvalidFlag;	/* unexpected platform specific flag */

	stream = (PaIosCoreStream *) PaUtil_AllocateMemory(sizeof(PaIosCoreStream));
	if (!stream) {
		result = paInsufficientMemory;
		goto error;
	}
	/*
	 * If we fail after this point, we my be left in a bad state, with
	 * some data structures setup and others not. So, first thing we do
	 * is initialize everything so that if we fail, we know what hasn't
	 * been touched.
	 */
	memset(stream, 0, sizeof(PaIosCoreStream));

	if (streamCallback) {
		PaUtil_InitializeStreamRepresentation(&stream->streamRepresentation,
		    &auhalHostApi->callbackStreamInterface,
		    streamCallback, userData);
	} else {
		PaUtil_InitializeStreamRepresentation(&stream->streamRepresentation,
		    &auhalHostApi->blockingStreamInterface,
		    BlioCallback, &stream->blio);
	}

	PaUtil_InitializeCpuLoadMeasurer(&stream->cpuLoadMeasurer, sampleRate);

	if (inputParameters && outputParameters && outputParameters->device == inputParameters->device) {
		/* full duplex. One device. */
		UInt32 inputFramesPerBuffer = (UInt32) stream->inputFramesPerBuffer;
		UInt32 outputFramesPerBuffer = (UInt32) stream->outputFramesPerBuffer;

		result = OpenAndSetupOneAudioUnit(stream,
		    inputParameters,
		    outputParameters,
		    requestedFramesPerBuffer,
		    &inputFramesPerBuffer,
		    &outputFramesPerBuffer,
		    auhalHostApi,
		    &stream->inputUnit,
		    sampleRate,
		    stream);
		stream->inputFramesPerBuffer = inputFramesPerBuffer;
		stream->outputFramesPerBuffer = outputFramesPerBuffer;
		stream->outputUnit = stream->inputUnit;
		if (result != paNoError)
			goto error;
	} else {
		/* full duplex, different devices OR simplex */
		UInt32 outputFramesPerBuffer = (UInt32) stream->outputFramesPerBuffer;
		UInt32 inputFramesPerBuffer = (UInt32) stream->inputFramesPerBuffer;

		result = OpenAndSetupOneAudioUnit(stream,
		    NULL,
		    outputParameters,
		    requestedFramesPerBuffer,
		    NULL,
		    &outputFramesPerBuffer,
		    auhalHostApi,
		    &stream->outputUnit,
		    sampleRate,
		    stream);
		if (result != paNoError)
			goto error;
		result = OpenAndSetupOneAudioUnit(stream,
		    inputParameters,
		    NULL,
		    requestedFramesPerBuffer,
		    &inputFramesPerBuffer,
		    NULL,
		    auhalHostApi,
		    &stream->inputUnit,
		    sampleRate,
		    stream);
		if (result != paNoError)
			goto error;
		stream->inputFramesPerBuffer = inputFramesPerBuffer;
		stream->outputFramesPerBuffer = outputFramesPerBuffer;
	}

	inputLatencyFrames += stream->inputFramesPerBuffer;
	outputLatencyFrames += stream->outputFramesPerBuffer;

	if (stream->inputUnit) {
		const size_t szfl = sizeof(float);

		/* setup the AudioBufferList used for input */
		memset(&stream->inputAudioBufferList, 0, sizeof(AudioBufferList));
		stream->inputAudioBufferList.mNumberBuffers = 1;
		stream->inputAudioBufferList.mBuffers[0].mNumberChannels
		    = inputChannelCount;
		stream->inputAudioBufferList.mBuffers[0].mDataByteSize
		    = stream->inputFramesPerBuffer * inputChannelCount * szfl;
		stream->inputAudioBufferList.mBuffers[0].mData
		    = (float *)calloc(
		    stream->inputFramesPerBuffer * inputChannelCount,
		    szfl);
		if (!stream->inputAudioBufferList.mBuffers[0].mData) {
			result = paInsufficientMemory;
			goto error;
		}

		/*
		 * If input and output devs are different we also need
		 * a ring buffer to store input data while waiting for
		 * output data:
		 */
		if (stream->outputUnit && (stream->inputUnit != stream->outputUnit)) {
			/*
			 * May want the ringSize or initial position in ring
			 * buffer to depend somewhat on sample rate change
			 */
			void *data;
			long ringSize;

			ringSize = computeRingBufferSize(inputParameters,
			    outputParameters,
			    stream->inputFramesPerBuffer,
			    stream->outputFramesPerBuffer,
			    sampleRate);

			/*
			 * now, we need to allocate memory for the ring
			 * buffer
			 */
			data = calloc(ringSize, szfl * inputParameters->channelCount);
			if (!data) {
				result = paInsufficientMemory;
				goto error;
			}
			/* now we can initialize the ring buffer */
			result = PaUtil_InitializeRingBuffer(&stream->inputRingBuffer,
			    szfl * inputParameters->channelCount, ringSize, data);
			if (result != 0) {
				/*
				 * The only reason this should fail is if
				 * ringSize is not a power of 2, which we do
				 * not anticipate happening.
				 */
				result = paUnanticipatedHostError;
				free(data);
				goto error;
			}
			/*
			 * advance the read point a little, so we are
			 * reading from the middle of the buffer
			 */
			if (stream->outputUnit) {
				PaUtil_AdvanceRingBufferWriteIndex(&stream->inputRingBuffer,
				    ringSize / RING_BUFFER_ADVANCE_DENOMINATOR);
			}

			/*
			 * Just adds to input latency between input device
			 * and PA full duplex callback.
			 */
			inputLatencyFrames += ringSize;
		}
	}

	/* -- initialize Blio Buffer Processors -- */
	if (!streamCallback) {
		long ringSize;

		ringSize = computeRingBufferSize(inputParameters,
		    outputParameters,
		    stream->inputFramesPerBuffer,
		    stream->outputFramesPerBuffer,
		    sampleRate);
		result = initializeBlioRingBuffers(&stream->blio,
		    inputParameters ? inputParameters->sampleFormat : 0,
		    outputParameters ? outputParameters->sampleFormat : 0,
		    ringSize,
		    inputParameters ? inputChannelCount : 0,
		    outputParameters ? outputChannelCount : 0);
		if (result != paNoError)
			goto error;

		inputLatencyFrames += ringSize;
		outputLatencyFrames += ringSize;

	}
	/* -- initialize Buffer Processor -- */
	{
		size_t maxHostFrames = stream->inputFramesPerBuffer;

		if (stream->outputFramesPerBuffer > maxHostFrames)
			maxHostFrames = stream->outputFramesPerBuffer;

		result = PaUtil_InitializeBufferProcessor(&stream->bufferProcessor,
		    inputChannelCount, inputSampleFormat,
		    hostInputSampleFormat,
		    outputChannelCount, outputSampleFormat,
		    hostOutputSampleFormat,
		    sampleRate,
		    streamFlags,
		    requestedFramesPerBuffer,
		    maxHostFrames,
		    paUtilBoundedHostBufferSize,
		    streamCallback ? streamCallback : BlioCallback,
		    streamCallback ? userData : &stream->blio);
		if (result != paNoError)
			goto error;
	}
	stream->bufferProcessorIsInitialized = TRUE;

	/* Calculate actual latency from the sum of individual latencies. */
	if (inputParameters) {
		inputLatencyFrames += PaUtil_GetBufferProcessorInputLatencyFrames(&stream->bufferProcessor);
		stream->streamRepresentation.streamInfo.inputLatency = inputLatencyFrames / sampleRate;
	} else {
		stream->streamRepresentation.streamInfo.inputLatency = 0.0;
	}

	if (outputParameters) {
		outputLatencyFrames += PaUtil_GetBufferProcessorOutputLatencyFrames(&stream->bufferProcessor);
		stream->streamRepresentation.streamInfo.outputLatency = outputLatencyFrames / sampleRate;
	} else {
		stream->streamRepresentation.streamInfo.outputLatency = 0.0;
	}

	stream->streamRepresentation.streamInfo.sampleRate = sampleRate;

	stream->sampleRate = sampleRate;

	stream->userInChan = inputChannelCount;
	stream->userOutChan = outputChannelCount;

	/* Setup property listeners for timestamp and latency calculations. */
	pthread_mutex_init(&stream->timingInformationMutex, NULL);
	stream->timingInformationMutexIsInitialized = 1;
	InitializeDeviceProperties(&stream->inputProperties);
	InitializeDeviceProperties(&stream->outputProperties);

	UpdateTimeStampOffsets(stream);
	/* Setup timestamp copies to be used by audio callback */
	stream->timestampOffsetCombined_ioProcCopy = stream->timestampOffsetCombined;
	stream->timestampOffsetInputDevice_ioProcCopy = stream->timestampOffsetInputDevice;
	stream->timestampOffsetOutputDevice_ioProcCopy = stream->timestampOffsetOutputDevice;

	stream->state = STOPPED;
	stream->xrunFlags = 0;

	*s = (PaStream *) stream;

	return result;

error:
	CloseStream(stream);
	return result;
}

/* Convert to nanoseconds and then to seconds */
#define	HOST_TIME_TO_PA_TIME(x) ({					\
	mach_timebase_info_data_t info;					\
	mach_timebase_info(&info);					\
	((x) * (double)info.numer / (double)info.denom) * 1.0E-09;	\
	})

PaTime
GetStreamTime(PaStream * s)
{
	return (HOST_TIME_TO_PA_TIME(mach_absolute_time()));
}

#define	RING_BUFFER_EMPTY 1000

/*
 * Called by the AudioUnit API to process audio from the sound card.
 * This is where the magic happens.
 */
static OSStatus
AudioIOProc(void *inRefCon,
    AudioUnitRenderActionFlags * ioActionFlags,
    const AudioTimeStamp * inTimeStamp,
    UInt32 inBusNumber,
    UInt32 inNumberFrames,
    AudioBufferList * ioData)
{
	size_t framesProcessed = 0;
	PaStreamCallbackTimeInfo timeInfo = {};
	PaIosCoreStream *stream = (PaIosCoreStream *) inRefCon;
	const bool isRender = (inBusNumber == OUTPUT_ELEMENT);
	int callbackResult = paContinue;
	double hostTimeStampInPaTime = HOST_TIME_TO_PA_TIME(inTimeStamp->mHostTime);

	PaUtil_BeginCpuLoadMeasurement(&stream->cpuLoadMeasurer);

	/* compute PaStreamCallbackTimeInfo */
	if (pthread_mutex_trylock(&stream->timingInformationMutex) == 0) {
		/* snapshot the ioproc copy of timing information */
		stream->timestampOffsetCombined_ioProcCopy = stream->timestampOffsetCombined;
		stream->timestampOffsetInputDevice_ioProcCopy = stream->timestampOffsetInputDevice;
		stream->timestampOffsetOutputDevice_ioProcCopy = stream->timestampOffsetOutputDevice;
		pthread_mutex_unlock(&stream->timingInformationMutex);
	}

	/*
	 * For timeInfo.currentTime we could calculate current time
	 * backwards from the HAL audio output time to give a more accurate
	 * impression of the current timeslice but it doesn't seem worth it
	 * at the moment since other PA host APIs don't do any better.
	 */
	timeInfo.currentTime = HOST_TIME_TO_PA_TIME(mach_absolute_time());

	/*
	 * For an input HAL AU, inTimeStamp is the time the samples
	 * are received from the hardware, for an output HAL AU
	 * inTimeStamp is the time the samples are sent to the
	 * hardware.  PA expresses timestamps in terms of when the
	 * samples enter the ADC or leave the DAC so we add or
	 * subtract kAudioDevicePropertyLatency below.
	 */

	/*
	 * FIXME: not sure what to do below if the host timestamps aren't
	 * valid (kAudioTimeStampHostTimeValid isn't set) Could ask on CA
	 * mailing list if it is possible for it not to be set. If so, could
	 * probably grab a now timestamp at the top and compute from there
	 * (modulo scheduling jitter) or ask on mailing list for other
	 * options.
	 */

	if (isRender) {
		if (stream->inputUnit) {
			/* full duplex */
			timeInfo.inputBufferAdcTime = hostTimeStampInPaTime -
			    (stream->timestampOffsetCombined_ioProcCopy + stream->timestampOffsetInputDevice_ioProcCopy);
			timeInfo.outputBufferDacTime = hostTimeStampInPaTime + stream->timestampOffsetOutputDevice_ioProcCopy;
		} else {
			/* output only */
			timeInfo.inputBufferAdcTime = 0;
			timeInfo.outputBufferDacTime = hostTimeStampInPaTime + stream->timestampOffsetOutputDevice_ioProcCopy;
		}
	} else {
		/* input only */
		timeInfo.inputBufferAdcTime = hostTimeStampInPaTime - stream->timestampOffsetInputDevice_ioProcCopy;
		timeInfo.outputBufferDacTime = 0;
	}

	if (isRender && stream->inputUnit == stream->outputUnit) {
		/*
		 * Full Duplex, One Device
		 *
		 * This is the lowest latency case, and also the simplest.
		 * Input data and output data are available at the same
		 * time. we do not use the input SR converter or the input
		 * ring buffer.
		 */
		OSStatus err;
		size_t bytesPerFrame = sizeof(float) * ioData->mBuffers[0].mNumberChannels;
		size_t frames = ioData->mBuffers[0].mDataByteSize / bytesPerFrame;
		size_t total = 0;

		assert(ioData->mNumberBuffers == 1);
		assert(ioData->mBuffers[0].mNumberChannels == stream->userOutChan);

		while (1) {
			size_t delta = frames - total;
			if (delta > stream->inputFramesPerBuffer)
				delta = stream->inputFramesPerBuffer;
			if (delta > stream->outputFramesPerBuffer)
				delta = stream->outputFramesPerBuffer;
			if (delta == 0)
				break;

			PaUtil_BeginBufferProcessing(&stream->bufferProcessor,
			    &timeInfo, stream->xrunFlags);
			stream->xrunFlags = 0;

			stream->inputAudioBufferList.mBuffers[0].mDataByteSize =
			    delta * bytesPerFrame;

			err = AudioUnitRender(stream->inputUnit,
			    ioActionFlags,
			    inTimeStamp,
			    INPUT_ELEMENT,
			    delta,
			    &stream->inputAudioBufferList);
			if (err)
				goto stop_stream;

			PaUtil_SetInputFrameCount(&stream->bufferProcessor, delta);
			PaUtil_SetInterleavedInputChannels(&stream->bufferProcessor, 0,
			    stream->inputAudioBufferList.mBuffers[0].mData,
			    stream->inputAudioBufferList.mBuffers[0].mNumberChannels);

			PaUtil_SetOutputFrameCount(&stream->bufferProcessor, delta);
			PaUtil_SetInterleavedOutputChannels(&stream->bufferProcessor, 0,
			    (char *)ioData->mBuffers[0].mData + (bytesPerFrame * total),
			    ioData->mBuffers[0].mNumberChannels);

			framesProcessed +=
			    PaUtil_EndBufferProcessing(&stream->bufferProcessor,
			    &callbackResult);
			total += delta;
		}
	} else if (isRender) {
		/*
		 * Output Side of Full Duplex or Simplex Output
		 *
		 * This case handles output data as in the full duplex
		 * case and if there is input data, reads it off the
		 * ring buffer and into the PA buffer processor.
		 */
		size_t bytesPerFrame = sizeof(float) * ioData->mBuffers[0].mNumberChannels;
		size_t frames = ioData->mBuffers[0].mDataByteSize / bytesPerFrame;
		size_t total = 0;
		int xrunFlags = stream->xrunFlags;

		if (stream->state == STOPPING || stream->state == CALLBACK_STOPPED)
			xrunFlags = 0;

		assert(ioData->mNumberBuffers == 1);
		assert(ioData->mBuffers[0].mNumberChannels == stream->userOutChan);

		while (1) {
			size_t delta = frames - total;

			if (stream->inputUnit && delta > stream->inputFramesPerBuffer)
				delta = stream->inputFramesPerBuffer;
			if (delta > stream->outputFramesPerBuffer)
				delta = stream->outputFramesPerBuffer;
			if (delta == 0)
				break;

			PaUtil_BeginBufferProcessing(&stream->bufferProcessor,
			    &timeInfo, xrunFlags);
			stream->xrunFlags = xrunFlags = 0;

			PaUtil_SetOutputFrameCount(&stream->bufferProcessor, delta);
			PaUtil_SetInterleavedOutputChannels(&stream->bufferProcessor, 0,
			    (char *)ioData->mBuffers[0].mData + (total * bytesPerFrame),
			    ioData->mBuffers[0].mNumberChannels);

			if (stream->inputUnit) {
				/* read data out of the ring buffer */
				int inChan = stream->inputAudioBufferList.mBuffers[0].mNumberChannels;
				size_t inBytesPerFrame = sizeof(float) * inChan;
				void *data1;
				void *data2;
				ring_buffer_size_t size1;
				ring_buffer_size_t size2;
				size_t framesReadable = PaUtil_GetRingBufferReadRegions(&stream->inputRingBuffer,
				    delta, &data1, &size1, &data2, &size2);

				if (size1 == delta) {
					/* simplest case: all in first buffer */
					PaUtil_SetInputFrameCount(&stream->bufferProcessor, delta);
					PaUtil_SetInterleavedInputChannels(&stream->bufferProcessor, 0,
					    data1, inChan);
					framesProcessed +=
					    PaUtil_EndBufferProcessing(&stream->bufferProcessor,
					    &callbackResult);
					PaUtil_AdvanceRingBufferReadIndex(&stream->inputRingBuffer, size1);
				} else if (framesReadable < delta) {
					long sizeBytes1 = size1 * inBytesPerFrame;
					long sizeBytes2 = size2 * inBytesPerFrame;

					/*
					 * Underflow: Take what data we can,
					 * zero the rest.
					 */
					unsigned char data[delta * inBytesPerFrame];

					if (size1 > 0)
						memcpy(data, data1, sizeBytes1);
					if (size2 > 0)
						memcpy(data + sizeBytes1, data2, sizeBytes2);
					memset(data + sizeBytes1 + sizeBytes2, 0,
					    (delta * inBytesPerFrame) - sizeBytes1 - sizeBytes2);

					PaUtil_SetInputFrameCount(&stream->bufferProcessor, delta);
					PaUtil_SetInterleavedInputChannels(&stream->bufferProcessor, 0,
					    data, inChan);
					framesProcessed +=
					    PaUtil_EndBufferProcessing(&stream->bufferProcessor,
					    &callbackResult);
					PaUtil_AdvanceRingBufferReadIndex(&stream->inputRingBuffer,
					    framesReadable);
					/* flag underflow */
					stream->xrunFlags |= paInputUnderflow;
				} else {
					/*
					 * We got all the data, but
					 * split between buffers
					 */
					PaUtil_SetInputFrameCount(&stream->bufferProcessor, size1);
					PaUtil_SetInterleavedInputChannels(&stream->bufferProcessor, 0,
					    data1, inChan);
					PaUtil_Set2ndInputFrameCount(&stream->bufferProcessor, size2);
					PaUtil_Set2ndInterleavedInputChannels(&stream->bufferProcessor, 0,
					    data2, inChan);
					framesProcessed +=
					    PaUtil_EndBufferProcessing(&stream->bufferProcessor,
					    &callbackResult);
					PaUtil_AdvanceRingBufferReadIndex(&stream->inputRingBuffer, framesReadable);
				}
			} else {
				framesProcessed +=
				    PaUtil_EndBufferProcessing(&stream->bufferProcessor,
				    &callbackResult);
			}
			total += delta;
		}
	} else {
		/*
		 * Input
		 *
		 * First, we read off the audio data and put it in the ring
		 * buffer. if this is an input-only stream, we need to
		 * process it more, otherwise, we let the output case deal
		 * with it.
		 */
		OSStatus err;
		int inChan = stream->inputAudioBufferList.mBuffers[0].mNumberChannels;
		size_t bytesPerFrame = sizeof(float) * inChan;
		size_t frames = inNumberFrames;
		size_t total = 0;

		while (1) {
			size_t delta = frames - total;

			if (delta > stream->inputFramesPerBuffer)
				delta = stream->inputFramesPerBuffer;
			if (delta == 0)
				break;

			stream->inputAudioBufferList.mBuffers[0].mDataByteSize =
			    frames * bytesPerFrame;

			err = AudioUnitRender(stream->inputUnit,
			    ioActionFlags,
			    inTimeStamp,
			    INPUT_ELEMENT,
			    delta,
			    &stream->inputAudioBufferList);
			if (err)
				goto stop_stream;

			if (stream->outputUnit) {
				/*
				 * If this is duplex put the data into the
				 * ring buffer:
				 */
				size_t framesWritten = PaUtil_WriteRingBuffer(&stream->inputRingBuffer,
				    stream->inputAudioBufferList.mBuffers[0].mData, delta);

				if (framesWritten != delta)
					stream->xrunFlags |= paInputOverflow;
			} else {
				/* Push data into the buffer processor */
				PaUtil_BeginBufferProcessing(&stream->bufferProcessor,
				    &timeInfo, stream->xrunFlags);
				stream->xrunFlags = 0;

				PaUtil_SetInputFrameCount(&stream->bufferProcessor, delta);
				PaUtil_SetInterleavedInputChannels(&stream->bufferProcessor, 0,
				    stream->inputAudioBufferList.mBuffers[0].mData, inChan);
				framesProcessed +=
				    PaUtil_EndBufferProcessing(&stream->bufferProcessor,
				    &callbackResult);
			}
			total += delta;
		}
	}

	/*
	 * Should we return successfully or fall through to stopping
	 * the stream?
	 */
	if (callbackResult == paContinue) {
		PaUtil_EndCpuLoadMeasurement(&stream->cpuLoadMeasurer, framesProcessed);
		return noErr;
	}

stop_stream:
	/* XXX stopping stream from here causes a deadlock */
	PaUtil_EndCpuLoadMeasurement(&stream->cpuLoadMeasurer, framesProcessed);
	return noErr;
}

static	PaError
CloseStream(PaStream * s)
{
	PaIosCoreStream *stream = (PaIosCoreStream *) s;
	PaError result = paNoError;

	if (stream == NULL)
		return (result);

	if (stream->outputUnit != NULL && stream->outputUnit != stream->inputUnit)
		AudioComponentInstanceDispose(stream->outputUnit);
	stream->outputUnit = NULL;

	if (stream->inputUnit != NULL)
		AudioComponentInstanceDispose(stream->inputUnit);
	stream->inputUnit = NULL;

	free((void *)stream->inputRingBuffer.buffer);
	stream->inputRingBuffer.buffer = NULL;

	free(stream->inputAudioBufferList.mBuffers[0].mData);
	stream->inputAudioBufferList.mBuffers[0].mData = NULL;

	result = destroyBlioRingBuffers(&stream->blio);
	if (result)
		return (result);

	if (stream->bufferProcessorIsInitialized)
		PaUtil_TerminateBufferProcessor(&stream->bufferProcessor);
	if (stream->timingInformationMutexIsInitialized)
		pthread_mutex_destroy(&stream->timingInformationMutex);

	PaUtil_TerminateStreamRepresentation(&stream->streamRepresentation);
	PaUtil_FreeMemory(stream);

	return (result);
}

static	PaError
StartStream(PaStream * s)
{
	PaIosCoreStream *stream = (PaIosCoreStream *) s;
	OSStatus result = noErr;

#define	ERR_WRAP(ios_err) do {		\
	result = ios_err;		\
	if (result != noErr)		\
		return ERR(result);	\
} while(0)

	PaUtil_ResetBufferProcessor(&stream->bufferProcessor);

	stream->state = ACTIVE;
	if (stream->inputUnit)
		ERR_WRAP(AudioOutputUnitStart(stream->inputUnit));

	if (stream->outputUnit && stream->outputUnit != stream->inputUnit)
		ERR_WRAP(AudioOutputUnitStart(stream->outputUnit));

	return paNoError;
#undef ERR_WRAP
}

static OSStatus
BlockWhileAudioUnitIsRunning(AudioUnit audioUnit, AudioUnitElement element)
{
	Boolean isRunning;

	while (1) {
		UInt32 s = sizeof(isRunning);
		OSStatus err = AudioUnitGetProperty(audioUnit,
		    kAudioOutputUnitProperty_IsRunning,
		    kAudioUnitScope_Global, element, &isRunning, &s);

		if (err || isRunning == false)
			return (err);
		Pa_Sleep(100);
	}
	return (noErr);
}

static	PaError
FinishStoppingStream(PaIosCoreStream * stream)
{
	OSStatus result = noErr;
	PaError paErr;

#define	ERR_WRAP(ios_err) do {		\
	result = ios_err;		\
	if (result != noErr)		\
		return ERR(result);	\
} while(0)

	if (stream->inputUnit == stream->outputUnit && stream->inputUnit) {
		ERR_WRAP(AudioOutputUnitStop(stream->inputUnit));
		ERR_WRAP(BlockWhileAudioUnitIsRunning(stream->inputUnit, 0));
		ERR_WRAP(BlockWhileAudioUnitIsRunning(stream->inputUnit, 1));
		ERR_WRAP(AudioUnitReset(stream->inputUnit, kAudioUnitScope_Global, 1));
		ERR_WRAP(AudioUnitReset(stream->inputUnit, kAudioUnitScope_Global, 0));
	} else {
		if (stream->inputUnit) {
			ERR_WRAP(AudioOutputUnitStop(stream->inputUnit));
			ERR_WRAP(BlockWhileAudioUnitIsRunning(stream->inputUnit, 1));
			ERR_WRAP(AudioUnitReset(stream->inputUnit, kAudioUnitScope_Global, 1));
		}
		if (stream->outputUnit) {
			ERR_WRAP(AudioOutputUnitStop(stream->outputUnit));
			ERR_WRAP(BlockWhileAudioUnitIsRunning(stream->outputUnit, 0));
			ERR_WRAP(AudioUnitReset(stream->outputUnit, kAudioUnitScope_Global, 0));
		}
	}
	if (stream->inputRingBuffer.buffer) {
		PaUtil_FlushRingBuffer(&stream->inputRingBuffer);
		memset((void *)stream->inputRingBuffer.buffer, 0, stream->inputRingBuffer.bufferSize);
		if (stream->outputUnit) {
			PaUtil_AdvanceRingBufferWriteIndex(
			    &stream->inputRingBuffer, stream->inputRingBuffer.bufferSize /
			    RING_BUFFER_ADVANCE_DENOMINATOR);
		}
	}
	stream->xrunFlags = 0;
	stream->state = STOPPED;

	paErr = resetBlioRingBuffers(&stream->blio);
	if (paErr)
		return (paErr);

	return (paNoError);
#undef ERR_WRAP
}

static	PaError
StopStream(PaStream * s)
{
	PaIosCoreStream *stream = (PaIosCoreStream *) s;
	PaError paErr;

	stream->state = STOPPING;

	if (stream->userOutChan > 0) {
		size_t maxHostFrames = MAX(stream->inputFramesPerBuffer, stream->outputFramesPerBuffer);

		paErr = waitUntilBlioWriteBufferIsEmpty(&stream->blio, stream->sampleRate, maxHostFrames);
	}
	return (FinishStoppingStream(stream));
}

static	PaError
AbortStream(PaStream * s)
{
	PaIosCoreStream *stream = (PaIosCoreStream *) s;

	stream->state = STOPPING;
	return (FinishStoppingStream(stream));
}

static	PaError
IsStreamStopped(PaStream * s)
{
	PaIosCoreStream *stream = (PaIosCoreStream *) s;

	return (stream->state == STOPPED);
}

static	PaError
IsStreamActive(PaStream * s)
{
	PaIosCoreStream *stream = (PaIosCoreStream *) s;

	return (stream->state == ACTIVE || stream->state == STOPPING);
}

static double
GetStreamCpuLoad(PaStream * s)
{
	PaIosCoreStream *stream = (PaIosCoreStream *) s;

	return (PaUtil_GetCpuLoad(&stream->cpuLoadMeasurer));
}
