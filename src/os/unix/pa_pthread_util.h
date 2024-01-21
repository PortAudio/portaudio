/*
 * $Id$
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2024 Ross Bencina, Phil Burk
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
 @ingroup unix_src
*/

#ifndef PA_PTHREAD_UTIL_H
#define PA_PTHREAD_UTIL_H

#include <time.h> /* timespec, clock_gettime */
#include <sys/types.h> /* clockid_t */
#if !(defined(WIN32) || defined(_WIN32))
#include <sys/time.h> /* gettimeofday on macos */
#endif
#include <pthread.h>

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

/*
  Use the presence of CLOCK_REALTIME as a proxy for the availability of
  pthread_condattr_setclock, pthread_condattr_getclock and clock_gettime.

  Otherewise use a fallback path.

  On Apple, stick with default unix time using gettimeofday,
  since CLOCK_MONOTONIC is known to be buggy:
    https://discussions.apple.com/thread/253778121?sortBy=best

  And clock_gettime is not available pre-Sierra:
    https://stackoverflow.com/questions/5167269/clock-gettime-alternative-in-mac-os-x
    https://stackoverflow.com/a/21352348
*/
#if defined(CLOCK_REALTIME) && !defined(__APPLE__)
#define PAUTIL_USE_POSIX_ADVANCED_REALTIME  1
#else
#define PAUTIL_USE_POSIX_ADVANCED_REALTIME  0
#endif

#if PAUTIL_USE_POSIX_ADVANCED_REALTIME

#define PaUtilClockId clockid_t

#else

#define PaUtilClockId int /* dummy type */

#endif

/** Negotiate the most suitable clock for condvar timeouts, set the clock
 * on cattr and return the clock's id.
 */
PaUtilClockId PaPthreadUtil_NegotiateCondAttrClock( pthread_condattr_t *cattr );

/** Get the current time according to the clock referred to by clockId, as
 * previously returned by PaPthreadUtil_NegotiateCondAttrTimeoutClock().
 *
 * Returns 0 upon success, -1 otherwise.
*/
int PaPthreadUtil_GetTime( PaUtilClockId clockId, struct timespec *ts );

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif
