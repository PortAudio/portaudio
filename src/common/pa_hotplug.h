#ifndef PA_HOTPLUG_H
#define PA_HOTPLUG_H
/*
 * $Id$
 * Portable Audio I/O Library
 * hotplug interface and utilities
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2016 Ross Bencina, Phil Burk
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
 @ingroup common_src

 @brief Utilities for implementing hotplug support.
*/

#include "portaudio.h"

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

/** Init/terminate hotplug notification engine.

    At the moment there is on hotplug implementation per platform.
    It is responsible for posting devices changed notifications
    by calling PaUtil_DevicesChanged.

    Once we support multiple notification mechanisms we'll probably
    have the host APIs init and terminate their own notification
    engines (using reference counting?) e.g. wasapi will have its own,
    but other windows APIs will use the global windows notifier.

    Implemented in pa_win_hotplug.c
*/
void PaUtil_InitializeHotPlug();
void PaUtil_TerminateHotPlug();


/** Invoke the client's registered devices changed notification.

  @param first  0 = unknown, 1 = insertion, 2 = removal
  @param second Host specific device change info (in windows it is the (unicode) device path)

  Parameters are currently ignored. TODO REVIEW

  Implemented in pa_front.c
*/
void PaUtil_DevicesChanged(unsigned, void*);


/** Lock/unlock a mutex used to protect the devices changed callback.
  Used by pa_front.c to synchronise notification callbacks and client
  requests to set/clear the device callback.

  Coded like this because we don't have a cross-platform mutex.

  Implemented in pa_win_hotplug.c
*/
void PaUtil_LockHotPlug();
void PaUtil_UnlockHotPlug();

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PA_HOTPLUG_H */
