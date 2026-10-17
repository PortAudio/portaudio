/** @file
 * @brief Single-stream ALSA fault fixture for the callback recovery test.
 *
 * Linux LD_PRELOAD test code only. Controls are called on the callback thread,
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
