
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

/*******************************************************************/
static int RecordAndPlaySinesCallback( const void *inputBuffer, void *outputBuffer,
						unsigned long framesPerBuffer,
						const PaStreamCallbackTimeInfo* timeInfo,
						PaStreamCallbackFlags statusFlags,
						void *userData )
{
    float *in = (float *)inputBuffer;
    float *out = (float *)outputBuffer;
    int done = 0;
	
	LoopbackContext *loopbackContext = (LoopbackContext *) userData;	
	loopbackContext->callbackCount += 1;
	
    /* This may get called with NULL inputBuffer during initial setup. */
    if( in == NULL) return 0;	
	
	for( int i=0; i<loopbackContext->test->inputParameters.channelCount; i++ )
	{
		done |= PaQa_WriteRecording( &loopbackContext->recordings[i], in + i, framesPerBuffer, loopbackContext->test->inputParameters.channelCount );
	}
	
	PaQa_EraseBuffer( out, framesPerBuffer, loopbackContext->test->outputParameters.channelCount );
	
	for( int i=0; i<loopbackContext->test->outputParameters.channelCount; i++ )
	{
		PaQa_MixSine( &loopbackContext->generators[i], out + i, framesPerBuffer, loopbackContext->test->outputParameters.channelCount );
	}
	
    return done ? paComplete : paContinue;
}

/*******************************************************************/
/** 
 * Open an audio stream.
 * Generate sine waves on the output channels and record the input channels.
 * Then close the stream.
 * @return 0 if OK or negative error.
 */
int PaQa_RunLoopback( LoopbackContext *loopbackContext )
{
	PaStream *stream = NULL;
	TestParameters *test = loopbackContext->test;
	PaError err = Pa_OpenStream(
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
		//printf("loopback count = %d\n", loopbackContext->callbackCount );
		//printf("recording position = %d\n", loopbackContext->recordings[0].numFrames );
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
	// Setup loopback context.
	memset( loopbackContextPtr, 0, sizeof(LoopbackContext) );	
	loopbackContextPtr->test = testParams;
	for( int i=0; i<testParams->samplesPerFrame; i++ )
	{
		int err = PaQa_InitializeRecording( &loopbackContextPtr->recordings[i], testParams->maxFrames, testParams->sampleRate );
		QA_ASSERT_EQUALS( "PaQa_InitializeRecording failed", 0, err );
	}
	for( int i=0; i<testParams->samplesPerFrame; i++ )
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

	for( int i=0; i<loopbackContextPtr->test->samplesPerFrame; i++ )
	{
		PaQa_TerminateRecording( &loopbackContextPtr->recordings[i] );
	}	
}

/*******************************************************************/
static void PaQa_PrintShortErrorReport( PaQaAnalysisResult *analysisResultPtr, int channel )
{
	printf("#%d ", channel);
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
	LoopbackContext loopbackContext;
	PaError err = paNoError;
	PaQaTestTone testTone;
	PaQaAnalysisResult analysisResult;
	int numBadChannels = 0;
	
	testTone.samplesPerFrame = testParams->samplesPerFrame;
	testTone.sampleRate = testParams->sampleRate;
	testTone.amplitude = testParams->amplitude;
	testTone.startDelay = 0;
	
	err = PaQa_SetupLoopbackContext( &loopbackContext, testParams );
	if( err ) return err;
	
	err = PaQa_RunLoopback( &loopbackContext );
	QA_ASSERT_TRUE("loopback did not run", (loopbackContext.callbackCount > 1) );
	
	// Analyse recording to to detect glitches.
	for( int i=0; i<testParams->samplesPerFrame; i++ )
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
			
	PaQa_TeardownLoopbackContext( &loopbackContext );
	if( numBadChannels > 0 )
	{
		g_testsFailed += 1;
	}
	return numBadChannels;	
	
error:
	PaQa_TeardownLoopbackContext( &loopbackContext );
	return 1;	
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
	testParamsPtr->framesPerBuffer = 256;
	testParamsPtr->baseFrequency = 200.0;
}

/*******************************************************************/
/** 
 * Run a series of tests on this loopback connection.
 * @return number of bad channel results
 */
static int PaQa_AnalyzeLoopbackConnection( UserOptions *userOptions, PaDeviceIndex inputDevice, PaDeviceIndex outputDevice, double expectedAmplitude )
{
	int totalBadChannels = 0;
	TestParameters testParams;
    const   PaDeviceInfo *inputDeviceInfo;	
    const   PaDeviceInfo *outputDeviceInfo;		
	inputDeviceInfo = Pa_GetDeviceInfo( inputDevice );
	outputDeviceInfo = Pa_GetDeviceInfo( outputDevice );
	
	printf( "=============== Analysing Loopback %d to %d ====================\n", outputDevice, inputDevice  );
	printf( "    Devices: %s => %s\n", outputDeviceInfo->name, inputDeviceInfo->name);
	
	double sampleRates[] = { 8000.0, 11025.0, 16000.0, 22050.0, 32000.0, 44100.0, 48000.0, 96000.0 };
//	double sampleRates[] = { 16000.0, 44100.0 };
	int numRates = (sizeof(sampleRates)/sizeof(double));
	
	int framesPerBuffers[] = { 0, 16, 32, 40, 64, 100, 128, 512, 1024 };
//	int framesPerBuffers[] = { 16, 64, 512 };
	int numBufferSizes = (sizeof(framesPerBuffers)/sizeof(int));
		
	printf("|-sRate-|-buffer-|-latency-|-channel results--------------------|\n");
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
	for( int iRate=0; iRate<numRates; iRate++ )
	{
		// SAMPLE RATE
		testParams.sampleRate = sampleRates[iRate];
		testParams.maxFrames = (int) (1.2 * testParams.sampleRate);
		for( int iSize=0; iSize<numBufferSizes; iSize++ )
		{	
			// BUFFER SIZE
			testParams.framesPerBuffer = framesPerBuffers[iSize];
			printf("| %5d | %6d | ", ((int)(testParams.sampleRate+0.5)), testParams.framesPerBuffer );
			fflush(stdout);
			
			int numBadChannels = PaQa_SingleLoopBackTest( userOptions, &testParams, expectedAmplitude );
			if( numBadChannels == 0 )
			{
				printf( "OK" );
			}
			totalBadChannels += numBadChannels;
			printf( "\n" );
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
		for( PaDeviceIndex i=0; i<numDevices; i++ )
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
		for( PaDeviceIndex i=0; i<numDevices; i++ )
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
		for( PaDeviceIndex i=0; i<numDevices; i++ )
		{
			for( PaDeviceIndex j=0; j<numDevices; j++ )
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
}

/*******************************************************************/
int main( int argc, char **argv )
{
	UserOptions userOptions;
	
	int result = 0;
	int justMath = 0;
	printf("PortAudio LoopBack Test built " __DATE__ " at " __TIME__ "\n");
	
	// Process arguments. Skip name of executable.
	memset(&userOptions, 0, sizeof(userOptions));
	userOptions.inputDevice = paNoDevice;
	userOptions.outputDevice = paNoDevice;
	userOptions.waveFilePath = ".";
	
	char *name = argv[0];
	for( int i=1; i<argc; i++ )
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
