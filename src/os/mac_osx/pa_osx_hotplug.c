
#include "pa_util.h"
#include "pa_debugprint.h"
#include "pa_allocation.h"

#include <stdio.h>

#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>

/* Implemented in pa_front.c
   @param first  0 = unknown, 1 = insertion, 2 = removal
   @param second Host specific device change info (in windows it is the (unicode) device path)
*/
extern void PaUtil_DevicesChanged(unsigned, void*);

/* Callback for audio hardware property changes. */
static OSStatus audioPropertyCallback(AudioHardwarePropertyID inPropertyID, 
        void *refCon)
{
    (void)refCon;
    switch (inPropertyID)
    {
        /*
         * These are the other types of notifications we might receive, however, they are beyond
         * the scope of this sample and we ignore them.
         */
        case kAudioHardwarePropertyDefaultInputDevice:
            PA_DEBUG(("audioPropertyCallback: default input device changed\n"));
            break;
        case kAudioHardwarePropertyDefaultOutputDevice:
            PA_DEBUG(("audioPropertyCallback: default output device changed\n"));
            break;
        case kAudioHardwarePropertyDefaultSystemOutputDevice:
            PA_DEBUG(("audioPropertyCallback: default system output device changed\n"));
            break;
        case kAudioHardwarePropertyDevices:
            PA_DEBUG(("audioPropertyCallback: device list changed\n"));
            PaUtil_DevicesChanged(1, NULL);
            break;
        default:
            PA_DEBUG(("audioPropertyCallback: unknown message id=%08lx\n", inPropertyID));
            break;
    }

    return noErr;
}

void PaUtil_InitializeHotPlug()
{
    AudioHardwareAddPropertyListener(kAudioHardwarePropertyDevices,  
            audioPropertyCallback, NULL); 
    AudioHardwareAddPropertyListener(kAudioHardwarePropertyDefaultInputDevice,  
            audioPropertyCallback, NULL); 
    AudioHardwareAddPropertyListener(kAudioHardwarePropertyDefaultOutputDevice,  
            audioPropertyCallback, NULL); 
}

void PaUtil_TerminateHotPlug()
{
    AudioHardwareRemovePropertyListener(kAudioHardwarePropertyDevices,  
            audioPropertyCallback); 
    AudioHardwareRemovePropertyListener(kAudioHardwarePropertyDefaultInputDevice,  
            audioPropertyCallback); 
    AudioHardwareRemovePropertyListener(kAudioHardwarePropertyDefaultOutputDevice,  
            audioPropertyCallback); 
}

void PaUtil_LockHotPlug()
{
}

void PaUtil_UnlockHotPlug()
{
}

