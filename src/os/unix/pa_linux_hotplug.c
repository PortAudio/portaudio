
#include "pa_util.h"
#include "pa_debugprint.h"
#include "pa_allocation.h"
#include "pa_linux_alsa.h"
#include "pa_hostapi.h"

#include <alsa/asoundlib.h>

#include <stdio.h>
#include <signal.h>
#include <pthread.h>

/* Implemented in pa_front.c
   @param first  0 = unknown, 1 = insertion, 2 = removal
   @param second Host specific device change info (in windows it is the (unicode) device path)
*/
extern void PaUtil_DevicesChanged(unsigned, void*);

static pthread_t g_thread_id;
static pthread_mutex_t g_mutex;
static volatile sig_atomic_t g_run = 0;

static int device_list_size(void)
{
  snd_ctl_t *handle;
  int card, err, dev, idx;
  int nb = 0;
  snd_ctl_card_info_t *info;
  snd_pcm_info_t *pcminfo;
  snd_ctl_card_info_alloca(&info);
  snd_pcm_info_alloca(&pcminfo);

  card = -1;
  if (snd_card_next(&card) < 0 || card < 0)
  {
    return nb;
  }

  while (card >= 0)
  {
    char name[32];

    sprintf(name, "hw:%d", card);
    if ((err = snd_ctl_open(&handle, name, 0)) < 0)
    {
      goto next_card;
    }
    if ((err = snd_ctl_card_info(handle, info)) < 0)
    {
      snd_ctl_close(handle);
      goto next_card;
    }
    dev = -1;
    while (1)
    {
      unsigned int count;
      int hasPlayback = 0;
      int hasCapture = 0;

      snd_ctl_pcm_next_device(handle, &dev);

      if (dev < 0)
        break;
      snd_pcm_info_set_device(pcminfo, dev);
      snd_pcm_info_set_subdevice(pcminfo, 0);
      snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_CAPTURE);
      if ((err = snd_ctl_pcm_info(handle, pcminfo)) >= 0)
      {
        hasCapture = 1;
      }

      snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_PLAYBACK);
      if ((err = snd_ctl_pcm_info(handle, pcminfo)) >= 0)
      {
        hasPlayback = 1;

        count = snd_pcm_info_get_subdevices_count(pcminfo);
      }

      if(hasPlayback == 0 && hasCapture == 0)
        continue;

      nb++;
    }
    snd_ctl_close(handle);
next_card:
    if (snd_card_next(&card) < 0)
    {
      break;
    }
  }
  return nb;
}

static void* thread_fcn(void* data)
{
  int currentDevices = 0;

  currentDevices = device_list_size();

  while(g_run)
  {
    int count = 0;
    
    sleep(1);
    count = device_list_size();
    if(count != currentDevices)
    {
      /* 1 = add device, 2 = remove device */
      int add = (count > currentDevices) ? 1 : 2;

      currentDevices = count;

      PaUtil_DevicesChanged(add, NULL);
    }     
  }

  return NULL;
}

void PaUtil_InitializeHotPlug()
{
  pthread_mutex_init(&g_mutex, NULL);
  g_run = 1;
  pthread_create(&g_thread_id, NULL, thread_fcn, NULL);
}

void PaUtil_TerminateHotPlug()
{
  void* ret = NULL;

  g_run = 0;
  pthread_join(g_thread_id, &ret); 
  pthread_mutex_destroy(&g_mutex);
}

void PaUtil_LockHotPlug()
{
  pthread_mutex_lock(&g_mutex);
}

void PaUtil_UnlockHotPlug()
{
  pthread_mutex_unlock(&g_mutex);
}

