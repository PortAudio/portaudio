/** @file
 * @brief Single-stream ALSA fault fixture for the callback recovery test.
 *
 * Linux executable symbol-interposition test code only. Controls are called on the callback thread,
 * or after the stream has closed. This fixture does not support concurrent streams.
 * Distributed under the same MIT license as PortAudio; see LICENSE.txt.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>

static snd_pcm_t *capture_;
static int armed_, pending_, statusXrun_, failStart_, failRecover_;

void PaTest_Reset( void )
{
    armed_ = pending_ = statusXrun_ = failStart_ = failRecover_ = 0;
    capture_ = NULL;
}

void PaTest_Arm( int mode )
{
    armed_ = 1;
    failStart_ = mode == 3 ? 0 : mode;
    failRecover_ = mode == 3;
}

int PaTest_State( void )
{
    return capture_ ? (int)snd_pcm_state( capture_ ) : -1;
}

int snd_pcm_hw_params_set_access( snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_access_t access )
{
    int (*real)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_access_t) =
            dlsym( RTLD_NEXT, "snd_pcm_hw_params_set_access" );
    if( access == SND_PCM_ACCESS_MMAP_INTERLEAVED || access == SND_PCM_ACCESS_MMAP_NONINTERLEAVED )
        return -EINVAL;
    return real( pcm, params, access );
}

int snd_pcm_hw_params_test_access( snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_access_t access )
{
    int (*real)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_access_t) =
            dlsym( RTLD_NEXT, "snd_pcm_hw_params_test_access" );
    if( access == SND_PCM_ACCESS_MMAP_INTERLEAVED || access == SND_PCM_ACCESS_MMAP_NONINTERLEAVED )
        return -EINVAL;
    return real( pcm, params, access );
}

snd_pcm_sframes_t snd_pcm_avail_update( snd_pcm_t *pcm )
{
    snd_pcm_sframes_t (*real)(snd_pcm_t *) = dlsym( RTLD_NEXT, "snd_pcm_avail_update" );
    if( armed_ && snd_pcm_stream( pcm ) == SND_PCM_STREAM_CAPTURE )
    {
        capture_ = pcm;
        armed_ = 0;
        pending_ = 1;
        return -EPIPE;
    }
    return real( pcm );
}

int snd_pcm_status( snd_pcm_t *pcm, snd_pcm_status_t *status )
{
    int (*real)(snd_pcm_t *, snd_pcm_status_t *) = dlsym( RTLD_NEXT, "snd_pcm_status" );
    int result = real( pcm, status );
    statusXrun_ = result == 0 && pending_ && pcm == capture_;
    return result;
}

snd_pcm_state_t snd_pcm_status_get_state( const snd_pcm_status_t *status )
{
    snd_pcm_state_t (*real)(const snd_pcm_status_t *) = dlsym( RTLD_NEXT, "snd_pcm_status_get_state" );
    if( statusXrun_ )
    {
        statusXrun_ = pending_ = 0;
        return SND_PCM_STATE_XRUN;
    }
    return real( status );
}

int snd_pcm_start( snd_pcm_t *pcm )
{
    int (*real)(snd_pcm_t *) = dlsym( RTLD_NEXT, "snd_pcm_start" );
    if( pcm == capture_ && failStart_ )
    {
        if( failStart_ == 1 )
            failStart_ = 0;
        return -EIO;
    }
    return real( pcm );
}

int snd_pcm_recover( snd_pcm_t *pcm, int error, int silent )
{
    int (*real)(snd_pcm_t *, int, int) = dlsym( RTLD_NEXT, "snd_pcm_recover" );
    if( pcm == capture_ && failRecover_ )
    {
        failRecover_ = 0;
        return -EIO;
    }
    return real( pcm, error, silent );
}

/** @file
 * @brief Linux ALSA non-MMAP capture recovery regression test.
 * Distributed under the same MIT license as PortAudio; see LICENSE.txt.
 */
#include <portaudio.h>
#include <pa_linux_alsa.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    pthread_mutex_t mutex;
    unsigned int calls, overflows;
    unsigned long after;
    int finished, mode, state;
    void (*arm)(int);
    int (*pcmState)(void);
} TestData;

static void Deadline( int signalNumber )
{
    static const char message[] = "FAIL: deadline exceeded (capture or teardown stalled)\n";
    (void)signalNumber;
    (void)write( STDERR_FILENO, message, sizeof(message) - 1 );
    _exit( 2 );
}

static int Capture( const void *input, void *output, unsigned long frames,
        const PaStreamCallbackTimeInfo *timing, PaStreamCallbackFlags flags, void *user )
{
    TestData *data = (TestData *)user;
    unsigned int calls;
    (void)input;
    (void)output;
    (void)timing;
    /* This diagnostic deliberately delays capture. The short mutex protects
     * observations shared with the main thread; it is not application advice.
     */
    pthread_mutex_lock( &data->mutex );
    calls = ++data->calls;
    if( flags & paInputOverflow )
        ++data->overflows;
    if( calls > 5 )
    {
        data->after += frames;
        if( data->mode >= 0 && data->mode <= 3 )
            data->state = data->pcmState();
    }
    pthread_mutex_unlock( &data->mutex );
    if( calls == 5 )
    {
        if( data->mode == 4 )
            Pa_Sleep( 1000 );
        else if( data->mode >= 0 )
            data->arm( data->mode );
    }
    return calls >= 100 ? paComplete : paContinue;
}

static void Finished( void *user )
{
    TestData *data = (TestData *)user;
    pthread_mutex_lock( &data->mutex );
    data->finished = 1;
    pthread_mutex_unlock( &data->mutex );
}

int main( int argc, char **argv )
{
    TestData data;
    PaAlsaStreamInfo info;
    PaStreamParameters input;
    PaStream *stream = NULL;
    PaError error;
    void (*reset)(void);
    int trial, finished, ticks, active, passed;
    char *end;
    long mode;
    if( argc != 3 )
    {
        fprintf( stderr, "Usage: %s ALSA_PCM MODE(-1..4)\n", argv[0] );
        return 1;
    }
    mode = strtol( argv[2], &end, 10 );
    if( *end || end == argv[2] || mode < -1 || mode > 4 )
        return 1;
    memset( &data, 0, sizeof(data) );
    data.mode = (int)mode;
    data.arm = PaTest_Arm;
    data.pcmState = PaTest_State;
    reset = PaTest_Reset;
    if( pthread_mutex_init( &data.mutex, NULL ) != 0 )
        return 1;
    signal( SIGALRM, Deadline );
    alarm( 20 );
    error = Pa_Initialize();
    if( error != paNoError )
        goto error;
    printf( "PortAudio: %s\n", Pa_GetVersionText() );
    PaAlsa_InitializeStreamInfo( &info );
    info.deviceString = argv[1];
    memset( &input, 0, sizeof(input) );
    input.device = paUseHostApiSpecificDeviceSpecification;
    input.channelCount = 1;
    input.sampleFormat = paInt16;
    input.suggestedLatency = 0.02;
    input.hostApiSpecificStreamInfo = &info;
    for( trial = 0; trial < 3; ++trial )
    {
        alarm( 20 );
        data.calls = data.overflows = 0;
        data.after = 0;
        data.finished = 0;
        data.state = -1;
        reset();
        error = Pa_OpenStream( &stream, &input, NULL, 48000, 4096, paNoFlag, Capture, &data );
        if( error != paNoError )
            goto error;
        error = Pa_SetStreamFinishedCallback( stream, Finished );
        if( error != paNoError )
            goto error;
        error = Pa_StartStream( stream );
        if( error != paNoError )
            goto error;
        finished = 0;
        for( ticks = 0; ticks < 1200 && !finished; ++ticks )
        {
            Pa_Sleep( 10 );
            pthread_mutex_lock( &data.mutex );
            finished = data.finished;
            pthread_mutex_unlock( &data.mutex );
        }
        active = Pa_IsStreamActive( stream );
        pthread_mutex_lock( &data.mutex );
        passed = finished && active == 0;
        if( mode == 2 )
            passed = passed && data.calls == 5 && data.after == 0;
        else
        {
            passed = passed && data.calls == 100 && data.after == 389120;
            if( mode >= 0 && mode <= 3 )
                passed = passed && data.state == 3; /* ALSA RUNNING */
            if( mode == 4 )
                passed = passed && data.overflows > 0;
        }
        printf( "trial=%d mode=%ld calls=%u after=%lu overflows=%u state=%d active=%d pass=%d\n",
                trial, mode, data.calls, data.after, data.overflows, data.state, active, passed );
        fflush( stdout );
        pthread_mutex_unlock( &data.mutex );
        error = Pa_AbortStream( stream );
        if( error != paNoError && error != paStreamIsStopped )
            goto error;
        error = Pa_CloseStream( stream );
        stream = NULL;
        if( error != paNoError )
            goto error;
        if( !passed )
        {
            Pa_Terminate();
            return 2;
        }
    }
    error = Pa_Terminate();
    if( error != paNoError )
        goto error;
    alarm( 0 );
    pthread_mutex_destroy( &data.mutex );
    puts( "PASS: capture recovery and three reopen cycles" );
    return 0;
error:
    fprintf( stderr, "FAIL: PortAudio error %d: %s\n", error, Pa_GetErrorText( error ) );
    if( stream )
        Pa_CloseStream( stream );
    Pa_Terminate();
    pthread_mutex_destroy( &data.mutex );
    return 1;
}
