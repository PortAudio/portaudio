
/*
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 *
 * Copyright (c) 1999-2010 Phil Burk and Ross Bencina
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

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <math.h>
#include "portaudio.h"

#include "qa_tools.h"

#include "paqa_tools.h"
#include "audio_analyzer.h"
#include "test_audio_analyzer.h"

/** Accumulate counts for how many tests pass or fail. */
int g_testsPassed = 0;
int g_testsFailed = 0;

#define MAX_NUM_GENERATORS  (8)
#define MAX_NUM_RECORDINGS  (8)
#define LOOPBACK_DETECTION_DURATION_SECONDS  (0.5)

// Use two separate streams instead of one full duplex stream.
#define PAQA_FLAG_TWO_STREAMS       (1<<0)
// Use bloching read/write for loopback.
#define PAQA_FLAG_USE_BLOCKING_IO   (1<<1)

const char * s_FlagOnNames[] =
{
	"Two Streams (Half Duplex)",
	"Blocking Read/Write"
};

const char * s_FlagOffNames[] =
{
	"One Stream (Full Duplex)",
	"Callback"
};

#define DEFAULT_FRAMES_PER_BUFFER   (256)

/** Parameters that describe a single test run. */
typedef struct TestParameters_s
{
	PaStreamParameters inputParameters;
	PaStreamParameters outputParameters;
	double             sampleRate;
	int                samplesPerFrame;
	int                framesPerBuffer;
	int                maxFrames;
	double             baseFrequency;
	double             amplitude;
	int                flags;
} TestParameters;

typedef struct LoopbackContext_s
{
	// Generate a unique signal on each channel.
	PaQaSineGenerator  generators[MAX_NUM_GENERATORS];
	// Record each channel individually.
	PaQaRecording      recordings[MAX_NUM_RECORDINGS];
	int                callbackCount;
	
	TestParameters    *test;
} LoopbackContext;

typedef struct UserOptions_s
{
	int           sampleRate;
	int           framesPerBuffer;
	int           latency;
	int           saveBadWaves;
	int           verbose;
	int           waveFileCount;
	const char   *waveFilePath;
	PaDeviceIndex inputDevice;
	PaDeviceIndex outputDevice;
} UserOptions;

#define BIG_BUFFER_SIZE  (sizeof(float) * 2 * 2048)
static unsigned char g_BigBuffer[BIG_BUFFER_SIZE];

/*******************************************************************/
static int RecordAndPlaySinesCallback( const void *inputBuffer, void *outputBuffer,
						unsigned long framesPerBuffer,
						const PaStreamCallbackTimeInfo* timeInfo,
						PaStreamCallbackFlags statusFlags,
						void *userData )
{
	int i;
    float *in = (float *)inputBuffer;
    float *out = (float *)outputBuffer;
    int done = paContinue;
	
	LoopbackContext *loopbackContext = (LoopbackContext *) userData;	
	loopbackContext->callbackCount += 1;
	
    /* This may get called with NULL inputBuffer during initial setup.
	 * We may also use the same callback with output only streams.
	 */
    if( in != NULL)
	{
		// Read each channel from the buffer.
		for( i=0; i<loopbackContext->test->inputParameters.channelCount; i++ )
		{
			done |= PaQa_WriteRecording( &loopbackContext->recordings[i],
										in + i, 
										framesPerBuffer,
										loopbackContext->test->inputParameters.channelCount );
		}
	}
	
	if( out != NULL )
	{
		PaQa_EraseBuffer( out, framesPerBuffer, loopbackContext->test->outputParameters.channelCount );
		
		
		for( i=0; i<loopbackContext->test->outputParameters.channelCount; i++ )
		{
			PaQa_MixSine( &loopbackContext->generators[i],
						 out + i,
						 framesPerBuffer,
						 loopbackContext->test->outputParameters.channelCount );
		}
	}
    return done ? paComplete : paContinue;
}

/*******************************************************************/
/** 
 * Open a full duplex audio stream.
 * Generate sine waves on the output channels and record the input channels.
 * Then close the stream.
 * @return 0 if OK or negative error.
 */
int PaQa_RunLoopbackFullDuplex( LoopbackContext *loopbackContext )
{
	PaStream *stream = NULL;
	PaError err = 0;
	TestParameters *test = loopbackContext->test;
	
	// Use one full duplex stream.
	err = Pa_OpenStream(
					&stream,
					&test->inputParameters,
					&test->outputParameters,
					test->sampleRate,
					test->framesPerBuffer,
					paClipOff, /* we won't output out of range samples so don't bother clipping them */
					RecordAndPlaySinesCallback,
					loopbackContext );
	if( err != paNoError ) goto error;
	
	err = Pa_StartStream( stream );
	if( err != paNoError ) goto error;
		
	// Wait for stream to finish.
	while( Pa_IsStreamActive( stream ) )
	{
		Pa_Sleep(50);
	}
	
	err = Pa_StopStream( stream );
	if( err != paNoError ) goto error;

	err = Pa_CloseStream( stream );
	if( err != paNoError ) goto error;
		
	return 0;
	
error:
	return err;	
}

/*******************************************************************/
/** 
 * Open two audio streams, one for input and one for output.
 * Generate sine waves on the output channels and record the input channels.
 * Then close the stream.
 * @return 0 if OK or negative error.
 */
int PaQa_RunLoopbackHalfDuplex( LoopbackContext *loopbackContext )
{
	PaStream *inStream = NULL;
	PaStream *outStream = NULL;
	PaError err = 0;
	TestParameters *test = loopbackContext->test;
	
	// Use two half duplex streams.
	err = Pa_OpenStream(
						&inStream,
						&test->inputParameters,
						NULL,
						test->sampleRate,
						test->framesPerBuffer,
						paClipOff, /* we won't output out of range samples so don't bother clipping them */
						RecordAndPlaySinesCallback,
						loopbackContext );
	if( err != paNoError ) goto error;
	err = Pa_OpenStream(
						&outStream,
						NULL,
						&test->outputParameters,
						test->sampleRate,
						test->framesPerBuffer,
						paClipOff, /* we won't output out of range samples so don't bother clipping them */
						RecordAndPlaySinesCallback,
						loopbackContext );
	if( err != paNoError ) goto error;
			
	err = Pa_StartStream( inStream );
	if( err != paNoError ) goto error;
	
	// Start output later so we catch the beginning of the waveform.
	err = Pa_StartStream( outStream );
	if( err != paNoError ) goto error;
	
	// Wait for stream to finish.
	while( Pa_IsStreamActive( inStream ) )
	{
		Pa_Sleep(50);
	}
	
	err = Pa_StopStream( inStream );
	if( err != paNoError ) goto error;
		
	err = Pa_StopStream( outStream );
	if( err != paNoError ) goto error;
	
	err = Pa_CloseStream( inStream );
	if( err != paNoError ) goto error;
	
	err = Pa_CloseStream( outStream );
	if( err != paNoError ) goto error;
	
	return 0;
	
error:
	return err;	
}



/*******************************************************************/
static int RecordAndPlayBlockingIO( PaStream *inStream,
									  PaStream *outStream,
									  LoopbackContext *loopbackContext
									  )
{	
	int i;
	float *in = (float *)g_BigBuffer;
	float *out = (float *)g_BigBuffer;
	PaError err;
	int done = 0;
	long available;
	const long maxPerBuffer = 64;
	TestParameters *test = loopbackContext->test;
	long framesPerBuffer = test->framesPerBuffer;
	if( framesPerBuffer <= 0 )
	{
		framesPerBuffer = maxPerBuffer; // bigger values might run past end of recording
	}
	
	// Read in audio.
	err = Pa_ReadStream( inStream, in, framesPerBuffer );
	// Ignore an overflow on the first read.
	//if( !((loopbackContext->callbackCount == 0) && (err == paInputOverflowed)) )
	if( err != paInputOverflowed )
	{
		QA_ASSERT_EQUALS( "Pa_ReadStream failed", paNoError, err );
	}
	
	// Save in a recording.
	for( i=0; i<loopbackContext->test->inputParameters.channelCount; i++ )
	{
		done |= PaQa_WriteRecording( &loopbackContext->recordings[i],
		         in + i,
		         framesPerBuffer,
		         loopbackContext->test->inputParameters.channelCount );
	}
	
	// Synthesize audio.
	available = Pa_GetStreamWriteAvailable( outStream );
	if( available > (2*framesPerBuffer) ) available = (2*framesPerBuffer);
	PaQa_EraseBuffer( out, available, loopbackContext->test->outputParameters.channelCount );
	for( i=0; i<loopbackContext->test->outputParameters.channelCount; i++ )
	{
		PaQa_MixSine( &loopbackContext->generators[i],
		          out + i,
		          available,
		          loopbackContext->test->outputParameters.channelCount );
	}
	
	// Write out audio.
	err = Pa_WriteStream( outStream, out, available );
	// Ignore an underflow on the first write.
	//if( !((loopbackContext->callbackCount == 0) && (err == paOutputUnderflowed)) )
	if( err != paOutputUnderflowed )
	{
		QA_ASSERT_EQUALS( "Pa_WriteStream failed", paNoError, err );
	}
		
	loopbackContext->callbackCount += 1;
	
	return done;
error:
	return err;
}


/*******************************************************************/
/** 
 * Open two audio streams with non-blocking IO.
 * Generate sine waves on the output channels and record the input channels.
 * Then close the stream.
 * @return 0 if OK or negative error.
 */
int PaQa_RunLoopbackHalfDuplexBlockingIO( LoopbackContext *loopbackContext )
{
	PaStream *inStream = NULL;
	PaStream *outStream = NULL;
	PaError err = 0;
	TestParameters *test = loopbackContext->test;
	
	// Use two half duplex streams.
	err = Pa_OpenStream(
						&inStream,
						&test->inputParameters,
						NULL,
						test->sampleRate,
						test->framesPerBuffer,
						paClipOff, /* we won't output out of range samples so don't bother clipping them */
						NULL, // causes non-blocking IO
						NULL );
	if( err != paNoError ) goto error1;
	err = Pa_OpenStream(
						&outStream,
						NULL,
						&test->outputParameters,
						test->sampleRate,
						test->framesPerBuffer,
						paClipOff, /* we won't output out of range samples so don't bother clipping them */
						NULL, // causes non-blocking IO
						NULL );
	if( err != paNoError ) goto error2;
	
	err = Pa_StartStream( outStream );
	if( err != paNoError ) goto error3;
	
	err = Pa_StartStream( inStream );
	if( err != paNoError ) goto error3;
	
	while( err == 0 )
	{
		err = RecordAndPlayBlockingIO( inStream, outStream, loopbackContext );
		if( err < 0 ) goto error3;
	}
	
	err = Pa_StopStream( inStream );
	if( err != paNoError ) goto error3;
	
	err = Pa_StopStream( outStream );
	if( err != paNoError ) goto error3;
	
	err = Pa_CloseStream( outStream );
	if( err != paNoError ) goto error2;
	
	err = Pa_CloseStream( inStream );
	if( err != paNoError ) goto error1;
	
	
	return 0;
	
error3:
	Pa_CloseStream( outStream );
error2:
	Pa_CloseStream( inStream );
error1:
	return err;	
}


/*******************************************************************/
/** 
 * Open one audio stream with non-blocking IO.
 * Generate sine waves on the output channels and record the input channels.
 * Then close the stream.
 * @return 0 if OK or negative error.
 */
int PaQa_RunLoopbackFullDuplexBlockingIO( LoopbackContext *loopbackContext )
{
	PaStream *stream = NULL;
	PaError err = 0;
	TestParameters *test = loopbackContext->test;
	
	// Use one full duplex stream.
	err = Pa_OpenStream(
						&stream,
						&test->inputParameters,
						&test->outputParameters,
						test->sampleRate,
						test->framesPerBuffer,
						paClipOff, /* we won't output out of range samples so don't bother clipping them */
						NULL, // causes non-blocking IO
						NULL );
	if( err != paNoError ) goto error1;
		
	err = Pa_StartStream( stream );
	if( err != paNoError ) goto error2;
	
	while( err == 0 )
	{
		err = RecordAndPlayBlockingIO( stream, stream, loopbackContext );
		if( err < 0 ) goto error2;
	}
	
	err = Pa_StopStream( stream );
	if( err != paNoError ) goto error2;
	
	
	err = Pa_CloseStream( stream );
	if( err != paNoError ) goto error1;
	
	
	return 0;
	
error2:
	Pa_CloseStream( stream );
error1:
	return err;	
}


/*******************************************************************/
/** 
 * Run some kind of loopback test.
 * @return 0 if OK or negative error.
 */
int PaQa_RunLoopback( LoopbackContext *loopbackContext )
{
	PaError err = 0;
	TestParameters *test = loopbackContext->test;
	
	
	if( test->flags & PAQA_FLAG_TWO_STREAMS )
	{
		if( test->flags & PAQA_FLAG_USE_BLOCKING_IO )
		{
			err = PaQa_RunLoopbackHalfDuplexBlockingIO( loopbackContext );
		}
		else
		{
			err = PaQa_RunLoopbackHalfDuplex( loopbackContext );
		}
	}
	else
	{
		if( test->flags & PAQA_FLAG_USE_BLOCKING_IO )
		{
			err = PaQa_RunLoopbackFullDuplexBlockingIO( loopbackContext );
		}
		else
		{
			err = PaQa_RunLoopbackFullDuplex( loopbackContext );
		}
	}
	
	if( err != paNoError )
	{
		printf("PortAudio error = %s\n", Pa_GetErrorText( err ) );
	}
	return err;	
}

/*******************************************************************/
static int PaQa_SaveTestResultToWaveFile( UserOptions *userOptions, PaQaRecording *recording )
{
	if( userOptions->saveBadWaves )
	{
		char filename[256];
		snprintf( filename, sizeof(filename), "%s/test_%d.wav", userOptions->waveFilePath, userOptions->waveFileCount++ );
		printf( "\"%s\", ", filename );
		return PaQa_SaveRecordingToWaveFile( recording, filename );
	}
	return 0;
}

/*******************************************************************/
static int PaQa_SetupLoopbackContext( LoopbackContext *loopbackContextPtr, TestParameters *testParams )
{
	int i;
	// Setup loopback context.
	memset( loopbackContextPtr, 0, sizeof(LoopbackContext) );	
	loopbackContextPtr->test = testParams;
	for( i=0; i<testParams->samplesPerFrame; i++ )
	{
		int err = PaQa_InitializeRecording( &loopbackContextPtr->recordings[i], testParams->maxFrames, testParams->sampleRate );
		QA_ASSERT_EQUALS( "PaQa_InitializeRecording failed", paNoError, err );
	}
	for( i=0; i<testParams->samplesPerFrame; i++ )
	{
		PaQa_SetupSineGenerator( &loopbackContextPtr->generators[i], PaQa_GetNthFrequency( testParams->baseFrequency, i ),
								testParams->amplitude, testParams->sampleRate );
	}
	return 0;
error:
	return -1;
}

/*******************************************************************/
static void PaQa_TeardownLoopbackContext( LoopbackContext *loopbackContextPtr )
{
	int i;
	for( i=0; i<loopbackContextPtr->test->samplesPerFrame; i++ )
	{
		PaQa_TerminateRecording( &loopbackContextPtr->recordings[i] );
	}	
}

/*******************************************************************/
static void PaQa_PrintShortErrorReport( PaQaAnalysisResult *analysisResultPtr, int channel )
{
	printf("channel %d ", channel);
	if( analysisResultPtr->popPosition > 0 )
	{
		printf("POP %0.3f at %d, ", (double)analysisResultPtr->popAmplitude, (int)analysisResultPtr->popPosition );	
	}
	else
	{
		if( analysisResultPtr->addedFramesPosition > 0 )
		{
			printf("ADD %d at %d ", (int)analysisResultPtr->numAddedFrames, (int)analysisResultPtr->addedFramesPosition );	
		}
		
		if( analysisResultPtr->droppedFramesPosition > 0 )
		{
			printf("DROP %d at %d ", (int)analysisResultPtr->numDroppedFrames, (int)analysisResultPtr->droppedFramesPosition );	
		}
	}
}

/*******************************************************************/
static void PaQa_PrintFullErrorReport( PaQaAnalysisResult *analysisResultPtr, int channel )
{
	printf("\n=== Loopback Analysis ===================\n");
	printf("             channel: %d\n", channel );
	printf("             latency: %10.3f\n", analysisResultPtr->latency );
	printf("      amplitudeRatio: %10.3f\n", (double)analysisResultPtr->amplitudeRatio );	
	printf("         popPosition: %10.3f\n", (double)analysisResultPtr->popPosition );	
	printf("        popAmplitude: %10.3f\n", (double)analysisResultPtr->popAmplitude );
	printf("    num added frames: %10.3f\n", analysisResultPtr->numAddedFrames );
	printf("     added frames at: %10.3f\n", analysisResultPtr->addedFramesPosition );
	printf("  num dropped frames: %10.3f\n", analysisResultPtr->numDroppedFrames );
	printf("   dropped frames at: %10.3f\n", analysisResultPtr->droppedFramesPosition );
}

/*******************************************************************/
/** 
 * Test loopback connection using the given parameters.
 * @return number of channels with glitches, or negative error.
 */
static int PaQa_SingleLoopBackTest( UserOptions *userOptions, TestParameters *testParams, double expectedAmplitude )
{
	int i;
	LoopbackContext loopbackContext;
	PaError err = paNoError;
	PaQaTestTone testTone;
	PaQaAnalysisResult analysisResult;
	int numBadChannels = 0;
	
	printf("| %5d | %6d | ", ((int)(testParams->sampleRate+0.5)), testParams->framesPerBuffer );
	fflush(stdout);
	
	testTone.samplesPerFrame = testParams->samplesPerFrame;
	testTone.sampleRate = testParams->sampleRate;
	testTone.amplitude = testParams->amplitude;
	testTone.startDelay = 0;
	
	err = PaQa_SetupLoopbackContext( &loopbackContext, testParams );
	if( err ) return err;
	
	err = PaQa_RunLoopback( &loopbackContext );
	QA_ASSERT_TRUE("loopback did not run", (loopbackContext.callbackCount > 1) );
	
	// Analyse recording to to detect glitches.
	for( i=0; i<testParams->samplesPerFrame; i++ )
	{
		double freq = PaQa_GetNthFrequency( testParams->baseFrequency, i );
		testTone.frequency = freq;
		
		PaQa_AnalyseRecording(  &loopbackContext.recordings[i], &testTone, &analysisResult );
		
		if( i==0 )
		{
			printf("%7.1f | ", analysisResult.latency );
		}
		
		if( analysisResult.valid )
		{
			int badChannel = ( (analysisResult.popPosition > 0)
					   || (analysisResult.addedFramesPosition > 0)
					   || (analysisResult.droppedFramesPosition > 0) );
			
			if( badChannel )
			{	
				if( userOptions->verbose )
				{
					PaQa_PrintFullErrorReport( &analysisResult, i );
				}
				else
				{
					PaQa_PrintShortErrorReport( &analysisResult, i );
				}
				PaQa_SaveTestResultToWaveFile( userOptions, &loopbackContext.recordings[i] );
			}
			numBadChannels += badChannel;
		}
		else
		{
			printf( "[%d] NO SIGNAL, ", i );
			numBadChannels += 1;
		}

	}
	if( numBadChannels == 0 )
	{
		printf( "OK" );
	}
	printf( "\n" );
	
			
	PaQa_TeardownLoopbackContext( &loopbackContext );
	if( numBadChannels > 0 )
	{
		g_testsFailed += 1;
	}
	return numBadChannels;	
	
error:
	PaQa_TeardownLoopbackContext( &loopbackContext );
	printf( "\n" );
	return err;	
}

/*******************************************************************/
static void PaQa_SetDefaultTestParameters( TestParameters *testParamsPtr, PaDeviceIndex inputDevice, PaDeviceIndex outputDevice )
{
	memset( testParamsPtr, 0, sizeof(TestParameters) );
	testParamsPtr->inputParameters.device = inputDevice;
	testParamsPtr->outputParameters.device = outputDevice;
	testParamsPtr->inputParameters.sampleFormat = paFloat32;
	testParamsPtr->outputParameters.sampleFormat = paFloat32;
	testParamsPtr->samplesPerFrame = 2;
	testParamsPtr->inputParameters.channelCount = testParamsPtr->samplesPerFrame;
	testParamsPtr->outputParameters.channelCount = testParamsPtr->samplesPerFrame;
	testParamsPtr->amplitude = 0.5;
	testParamsPtr->sampleRate = 44100;
	testParamsPtr->maxFrames = (int) (1.0 * testParamsPtr->sampleRate);
	testParamsPtr->framesPerBuffer = DEFAULT_FRAMES_PER_BUFFER;
	testParamsPtr->baseFrequency = 200.0;
	testParamsPtr->flags = PAQA_FLAG_TWO_STREAMS;
}

/*******************************************************************/
/** 
 * Run a series of tests on this loopback connection.
 * @return number of bad channel results
 */
static int PaQa_AnalyzeLoopbackConnection( UserOptions *userOptions, PaDeviceIndex inputDevice, PaDeviceIndex outputDevice, double expectedAmplitude )
{
	int iFlags;
	int iRate;
	int iSize;
	int totalBadChannels = 0;
	TestParameters testParams;
    const   PaDeviceInfo *inputDeviceInfo;	
    const   PaDeviceInfo *outputDeviceInfo;		
	inputDeviceInfo = Pa_GetDeviceInfo( inputDevice );
	outputDeviceInfo = Pa_GetDeviceInfo( outputDevice );
	
	printf( "=============== Analysing Loopback %d to %d ====================\n", outputDevice, inputDevice  );
	printf( "    Devices: %s => %s\n", outputDeviceInfo->name, inputDeviceInfo->name);
	
	int flagSettings[] = { 0, 1 };
	int numFlagSettings = (sizeof(flagSettings)/sizeof(int));
	
	double sampleRates[] = { 44100.0, 48000.0, 8000.0, 11025.0, 16000.0, 22050.0, 32000.0, 96000.0 };
	int numRates = (sizeof(sampleRates)/sizeof(double));
	
	int framesPerBuffers[] = { 256, 16, 32, 40, 64, 100, 128, 512, 1024 };
	int numBufferSizes = (sizeof(framesPerBuffers)/sizeof(int));
		
	// Check to see if a specific value was requested.
	if( userOptions->sampleRate > 0 )
	{
		sampleRates[0] = userOptions->sampleRate;
		numRates = 1;
	}
	if( userOptions->framesPerBuffer > 0 )
	{
		framesPerBuffers[0] = userOptions->framesPerBuffer;
		numBufferSizes = 1;
	}
	
	PaQa_SetDefaultTestParameters( &testParams, inputDevice, outputDevice );
	testParams.maxFrames = (int) (0.5 * testParams.sampleRate);	
	
	// Loop though combinations of audio parameters.
	for( iFlags=0; iFlags<numFlagSettings; iFlags++ )
	{
		testParams.flags = flagSettings[iFlags];
		printf( "************ Mode = %s ************\n",
			   (( iFlags & 1 ) ? s_FlagOnNames[0] : s_FlagOffNames[0]) );
		printf("|-sRate-|-buffer-|-latency-|-channel results--------------------|\n");

		// Loop though combinations of audio parameters.
		testParams.framesPerBuffer = framesPerBuffers[0];
		for( iRate=0; iRate<numRates; iRate++ )
		{
			// SAMPLE RATE
			testParams.sampleRate = sampleRates[iRate];
			testParams.maxFrames = (int) (1.2 * testParams.sampleRate);
			
			int numBadChannels = PaQa_SingleLoopBackTest( userOptions, &testParams, expectedAmplitude );
			totalBadChannels += numBadChannels;
		}
		printf( "\n" );
		
		testParams.sampleRate = sampleRates[0];
		testParams.maxFrames = (int) (1.2 * testParams.sampleRate);
		for( iSize=0; iSize<numBufferSizes; iSize++ )
		{	
			// BUFFER SIZE
			testParams.framesPerBuffer = framesPerBuffers[iSize];
			
			int numBadChannels = PaQa_SingleLoopBackTest( userOptions, &testParams, expectedAmplitude );
			totalBadChannels += numBadChannels;			
		}
		printf( "\n" );
		
		
	}
	return totalBadChannels;
}

/*******************************************************************/
/** 
 * Output a sine wave then try to detect it on input.
 *
 * @return 1 if loopback connected, 0 if not, or negative error.
 */
int PaQa_CheckForLoopBack( PaDeviceIndex inputDevice, PaDeviceIndex outputDevice )
{
	TestParameters testParams;
	LoopbackContext loopbackContext;
    const   PaDeviceInfo *inputDeviceInfo;	
    const   PaDeviceInfo *outputDeviceInfo;		
	PaError err = paNoError;
	double minAmplitude = 0.3;
	
	inputDeviceInfo = Pa_GetDeviceInfo( inputDevice );
	if( inputDeviceInfo->maxInputChannels < 2 )
	{
		return 0;
	}
	outputDeviceInfo = Pa_GetDeviceInfo( outputDevice );
	if( outputDeviceInfo->maxOutputChannels < 2 )
	{
		return 0;
	}
	
	printf( "Look for loopback cable between \"%s\" => \"%s\"\n", outputDeviceInfo->name, inputDeviceInfo->name);
	
	PaQa_SetDefaultTestParameters( &testParams, inputDevice, outputDevice );
	testParams.maxFrames = (int) (LOOPBACK_DETECTION_DURATION_SECONDS * testParams.sampleRate);	
	
	PaQa_SetupLoopbackContext( &loopbackContext, &testParams );
			
	testParams.flags = PAQA_FLAG_TWO_STREAMS;
	err = PaQa_RunLoopback( &loopbackContext );
	QA_ASSERT_TRUE("loopback detection callback did not run", (loopbackContext.callbackCount > 1) );
	
	// Analyse recording to see if we captured the output.
	// Start in the middle assuming past latency.
	int startFrame = testParams.maxFrames/2;
	int numFrames = testParams.maxFrames/2;
	double magLeft = PaQa_CorrelateSine( &loopbackContext.recordings[0],
										loopbackContext.generators[0].frequency,
										testParams.sampleRate,
										startFrame, numFrames, NULL );
	double magRight = PaQa_CorrelateSine( &loopbackContext.recordings[1],
										 loopbackContext.generators[1].frequency,
										 testParams.sampleRate,
										 startFrame, numFrames, NULL );
	printf("   Amplitudes: left = %f, right = %f\n", magLeft, magRight );
	int loopbackConnected = ((magLeft > minAmplitude) && (magRight > minAmplitude));
	
	// Check for backwards cable.
	if( !loopbackConnected )
	{
		double magLeftReverse = PaQa_CorrelateSine( &loopbackContext.recordings[0],
												   loopbackContext.generators[1].frequency,
												   testParams.sampleRate,
												   startFrame, numFrames, NULL );
		
		double magRightReverse = PaQa_CorrelateSine( &loopbackContext.recordings[1],
		          loopbackContext.generators[0].frequency,
			  testParams.sampleRate,
			  startFrame, numFrames, NULL );
		
		if ((magLeftReverse > 0.1) && (magRightReverse>minAmplitude))
		{
			printf("WARNING - you seem to have the left and right channels swapped on the loopback cable!\n");
		}
	}
	
	
	PaQa_TeardownLoopbackContext( &loopbackContext );
	return loopbackConnected;	
	
error:
	PaQa_TeardownLoopbackContext( &loopbackContext );
	return err;	
}

/*******************************************************************/
/**
 * Scan every combination of output to input device.
 * If a loopback is found the analyse the combination.
 * The scan can be overriden using the -i and -o command line options.
 */
static int ScanForLoopback(UserOptions *userOptions)
{
	PaDeviceIndex i,j;
	int  numLoopbacks = 0;
    int  numDevices;
    numDevices = Pa_GetDeviceCount();    
	
	double expectedAmplitude = 0.4;
	
	// If both devices are specified then just use that combination.
	if ((userOptions->inputDevice >= 0) && (userOptions->outputDevice >= 0))
	{
		PaQa_AnalyzeLoopbackConnection( userOptions, userOptions->inputDevice, userOptions->outputDevice, expectedAmplitude );
		numLoopbacks += 1;
	}
	else if (userOptions->inputDevice >= 0)
	{
		// Just scan for output.
		for( i=0; i<numDevices; i++ )
		{					
			int loopbackConnected = PaQa_CheckForLoopBack( userOptions->inputDevice, i );
			if( loopbackConnected > 0 )
			{
				PaQa_AnalyzeLoopbackConnection( userOptions, userOptions->inputDevice, i, expectedAmplitude );
				numLoopbacks += 1;
			}
		}
	}
	else if (userOptions->outputDevice >= 0)
	{
		// Just scan for input.
		for( i=0; i<numDevices; i++ )
		{					
			int loopbackConnected = PaQa_CheckForLoopBack( i, userOptions->inputDevice );
			if( loopbackConnected > 0 )
			{
				PaQa_AnalyzeLoopbackConnection( userOptions, i, userOptions->inputDevice, expectedAmplitude );
				numLoopbacks += 1;
			}
		}
	}
	else 
	{	
		// Scan both.
		for( i=0; i<numDevices; i++ )
		{
			for( j=0; j<numDevices; j++ )
			{
				int loopbackConnected = PaQa_CheckForLoopBack( i, j );
				if( loopbackConnected > 0 )
				{
					PaQa_AnalyzeLoopbackConnection( userOptions, i, j, expectedAmplitude );
					numLoopbacks += 1;
				}
			}
		}
	}
	QA_ASSERT_TRUE( "No loopback cables found or volumes too low.", (numLoopbacks > 0) );
	return numLoopbacks;
	
error:
	return -1;
}

/*******************************************************************/
void usage( const char *name )
{
	printf("%s [-i# -o# -l# -r# -s# -m -w -dDir]\n", name);
	printf("  -i# Input device ID. Will scan for loopback cable if not specified.\n");
	printf("  -o# Output device ID. Will scan for loopback if not specified.\n");
//	printf("  -l# Latency in milliseconds.\n");
	printf("  -r# Sample Rate in Hz.  Will use multiple common rates if not specified.\n");
	printf("  -s# Size of callback buffer in frames, framesPerBuffer. Will use common values if not specified.\n");
	printf("  -w  Save bad recordings in a WAV file.\n");
	printf("  -dDir  Path for Directory for WAV files. Default is current directory.\n");
	printf("  -m  Just test the DSP Math code and not the audio devices.\n");
	printf("  -v  Verbose reports.\n");
}

/*******************************************************************/
int main( int argc, char **argv )
{
	UserOptions userOptions;
	int i;
	int result = 0;
	int justMath = 0;
	printf("PortAudio LoopBack Test built " __DATE__ " at " __TIME__ "\n");
	
	// Process arguments. Skip name of executable.
	memset(&userOptions, 0, sizeof(userOptions));
	userOptions.inputDevice = paNoDevice;
	userOptions.outputDevice = paNoDevice;
	userOptions.waveFilePath = ".";
	
	char *name = argv[0];
	for( i=1; i<argc; i++ )
	{
		char *arg = argv[i];
		if( arg[0] == '-' )
		{
			switch(arg[1])
			{
				case 'i':
					userOptions.inputDevice = atoi(&arg[2]);
					break;
				case 'o':
					userOptions.outputDevice = atoi(&arg[2]);
					break;
				case 'l':
					userOptions.latency = atoi(&arg[2]);
					break;
				case 'r':
					userOptions.sampleRate = atoi(&arg[2]);
					break;
				case 's':
					userOptions.framesPerBuffer = atoi(&arg[2]);
					break;
					
				case 'm':
					printf("Option -m set so just testing math and not the audio devices.\n");
					justMath = 1;
					break;
					
				case 'w':
					userOptions.saveBadWaves = 1;
					break;
				case 'd':
					userOptions.waveFilePath = &arg[2];
					break;
					
				case 'v':
					userOptions.verbose = 1;
					break;
					
				case 'h':
					usage( name );
					return(0);
					break;
				default:
					printf("Illegal option: %s\n", arg);
					usage( name );
					break;
			}
		}
		else
		{
			printf("Illegal argument: %s\n", arg);
			usage( name );
		}
	}
		
	result = PaQa_TestAnalyzer();
	
	if( (result == 0) && (justMath == 0) )
	{
		Pa_Initialize();
		printf( "PortAudio version number = %d\nPortAudio version text = '%s'\n",
			   Pa_GetVersion(), Pa_GetVersionText() );
		printf( "=============== PortAudio Devices ========================\n" );
		PaQa_ListAudioDevices();
		printf( "=============== Detect Loopback ==========================\n" );
		ScanForLoopback(&userOptions);
		Pa_Terminate();
	}

	if (g_testsFailed == 0)
	{
		printf("PortAudio QA SUCCEEDED! %d tests passed, %d tests failed\n", g_testsPassed, g_testsFailed );
		return 0;

	}
	else
	{
		printf("PortAudio QA FAILED! %d tests passed, %d tests failed\n", g_testsPassed, g_testsFailed );
		return 1;
	}	
}
