/** @file pa_start_stop_deadlock.c
    @brief Reproduce a CoreAudio AB-BA deadlock between Pa_StopStream and the
    kAudioOutputUnitProperty_IsRunning property listener (startStopCallback).

    Mechanism: pa_mac_core.c registers startStopCallback as an AudioUnit
    property listener for kAudioOutputUnitProperty_IsRunning. CoreAudio
    delivers that notification synchronously on the HAL IO thread from inside
    HALC_ProxyIOContext::IOWorkLoop, i.e. while the IO thread holds the HAL
    IOContext mutex. The listener calls AudioUnitGetProperty, which needs the
    AudioUnit instance's recursive mutex.

    Meanwhile a thread calling Pa_StopStream is inside AudioOutputUnitStop,
    which takes the AudioUnit instance mutex at the API boundary and then
    blocks waiting for the IOContext mutex in HALC_ProxyIOContext::StopIOProc.

    Each thread holds the lock the other needs -> deadlock.

    Every cycle opens and closes the stream (Pa_OpenStream + Pa_StartStream,
    then Pa_StopStream + Pa_CloseStream), creating and destroying fresh AUHAL
    units each time (fresh listener registration, IO-thread spin-up,
    teardown). Crucially the stream is opened DUPLEX with distinct input and
    output devices, giving two AUHAL units and two IO threads: the deadlock
    fires most readily while stopping the INPUT unit (the separate-units
    branch of FinishStoppingStream). Output-only single-unit cycles reproduce
    orders of magnitude more slowly. The race window is sub-millisecond and
    fixed timing can miss it indefinitely, so each iteration sweeps the phase
    between start and stop across 0-40ms. In duplex mode on an unpatched
    PortAudio expect the deadlock within a few hundred cycles (tens of
    seconds); output-only can take thousands of cycles.

    Usage: pa_start_stop_deadlock [iterations]   (default 100000)
    Requires a real CoreAudio output device; not suitable for headless CI.

    Exit codes: 0 = completed all iterations without deadlock
                1 = PortAudio error
                2 = deadlock detected (main loop stalled >10s)
    Set PA_DEADLOCK_PARK=1 to leave the wedged process running for debugger
    attach / sample instead of exiting.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "portaudio.h"

#define SAMPLE_RATE 44100
#define FRAMES_PER_BUFFER 64
#define NUM_CHANNELS 2

static volatile long g_iteration = 0;
static volatile int g_done = 0;

static int g_outChannels = 0;

#define INDEX_MODULUS  41

/* the stream is opened paFloat32 | paNonInterleaved: output is an
   array of per-channel float buffers */
static int silenceCallback(const void* input, void* output,
                           unsigned long frameCount,
                           const PaStreamCallbackTimeInfo* timeInfo,
                           PaStreamCallbackFlags statusFlags,
                           void* userData)
{
    float** out = (float**)output;
    (void)input;
    (void)timeInfo;
    (void)statusFlags;
    (void)userData;
    for (int ch = 0; ch < g_outChannels; ch++) {
        memset(out[ch], 0, frameCount * sizeof(float));
    }
    return paContinue;
}

static void* watchdog(void* arg)
{
    (void)arg;
    long last = -1;
    int stalledSeconds = 0;
    while (!g_done) {
        sleep(1);
        long now = g_iteration;
        if (now == last) {
            if (++stalledSeconds >= 10) {
                fprintf(stderr, "\nDEADLOCK: no progress for %d seconds at iteration %ld, rem %d, (pid %d)\n",
                        stalledSeconds, now, (int)(now % INDEX_MODULUS), (int)getpid());
                if (getenv("PA_DEADLOCK_PARK")) {
                    fprintf(stderr, "PA_DEADLOCK_PARK set: process left running for debugger attach / sample.\n");
                    fflush(stderr);
                    for (;;) {
                        sleep(60);
                    }
                }
                fflush(stderr);
                _exit(2);
            }
        }
        else {
            stalledSeconds = 0;
            last = now;
        }
    }
    return NULL;
}

int main(int argc, char** argv)
{
    PaStreamParameters inputParameters;
    PaStreamParameters outputParameters;
    const PaDeviceInfo* inputInfo = NULL;
    PaError err;
    pthread_t wd;
    long iterations = (argc > 1) ? atol(argv[1]) : 100000;

    err = Pa_Initialize();
    if (err != paNoError) {
        goto error;
    }

    outputParameters.device = Pa_GetDefaultOutputDevice();
    if (outputParameters.device == paNoDevice) {
        fprintf(stderr, "no default output device\n");
        Pa_Terminate();
        return 1;
    }
    outputParameters.channelCount = NUM_CHANNELS;
    outputParameters.sampleFormat = paFloat32 | paNonInterleaved;
    outputParameters.suggestedLatency = Pa_GetDeviceInfo(outputParameters.device)->defaultLowOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = NULL;
    g_outChannels = outputParameters.channelCount;

    /* duplex with a distinct input device -> two AUHAL units (fastest
       reproduction); fall back to output-only if no input device exists */
    inputParameters.device = Pa_GetDefaultInputDevice();
    if (inputParameters.device != paNoDevice) {
        inputInfo = Pa_GetDeviceInfo(inputParameters.device);
    }
    if (inputInfo && inputInfo->maxInputChannels > 0) {
        inputParameters.channelCount = inputInfo->maxInputChannels < NUM_CHANNELS
                                           ? inputInfo->maxInputChannels
                                           : NUM_CHANNELS;
        inputParameters.sampleFormat = paFloat32 | paNonInterleaved;
        inputParameters.suggestedLatency = inputInfo->defaultLowInputLatency;
        inputParameters.hostApiSpecificStreamInfo = NULL;
    }
    else {
        inputInfo = NULL;
        fprintf(stderr, "warning: no input device, running output-only -- "
                        "reproduces the single-unit path only, much more slowly\n");
    }

    printf("pid %d: hammering open/start/stop/close cycles, %s (%ld iterations max)...\n",
           (int)getpid(), inputInfo ? "duplex" : "output-only", iterations);
    fflush(stdout);

    pthread_create(&wd, NULL, watchdog, NULL);

    for (long i = 0; i < iterations; i++) {
        PaStream* stream = NULL;
        g_iteration = i;

        err = Pa_OpenStream(&stream, inputInfo ? &inputParameters : NULL,
                            &outputParameters, SAMPLE_RATE,
                            FRAMES_PER_BUFFER, 0, silenceCallback, NULL);
        if (err != paNoError) {
            goto error;
        }
        err = Pa_StartStream(stream);
        if (err != paNoError) {
            goto error;
        }

        /* sweep the phase between our stop call and CoreAudio's
           IsRunning notification delivery on the IO thread */
        Pa_Sleep(i % INDEX_MODULUS);

        err = Pa_StopStream(stream);
        if (err != paNoError) {
            goto error;
        }
        err = Pa_CloseStream(stream);
        if (err != paNoError) {
            goto error;
        }

        /* brief stopped-gap sweep, decorrelated from the on-time sweep */
        Pa_Sleep(i % 7);

        if (i % 10 == 0) {
            printf("iteration %ld\n", i);
            fflush(stdout);
        }
    }

    g_done = 1;
    pthread_join(wd, NULL);
    Pa_Terminate();
    printf("completed %ld iterations without deadlock\n", iterations);
    return 0;

error:
    fprintf(stderr, "PortAudio error at iteration %ld: %s\n", g_iteration, Pa_GetErrorText(err));
    Pa_Terminate();
    return 1;
}
