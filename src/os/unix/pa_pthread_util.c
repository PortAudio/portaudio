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

#if !PAUTIL_USE_POSIX_ADVANCED_REALTIME && (defined(WIN32) || defined(_WIN32))
#include <windows.h>
#else
#include <errno.h>
#endif

#include "pa_pthread_util.h"
#include "pa_debugprint.h"

PaUtilClockId PaPthreadUtil_NegotiateCondAttrClock( pthread_condattr_t *cattr )
{
#if PAUTIL_USE_POSIX_ADVANCED_REALTIME
    /* Set most suitable timeout clock on the condattr and return its clock id.
       If a clock can't be set, return the default clock id.
    */
    clockid_t clockId;

/* try each potential clockid in order of preference until one succeeds: */
#if defined(CLOCK_BOOTTIME )
    if( pthread_condattr_setclock( cattr, CLOCK_BOOTTIME ) == 0 )
        return CLOCK_BOOTTIME;
#endif

#if defined(CLOCK_MONOTONIC)
    if( pthread_condattr_setclock( cattr, CLOCK_MONOTONIC ) == 0 )
        return CLOCK_MONOTONIC;
#endif

#if defined(CLOCK_REALTIME)
    if( pthread_condattr_setclock( cattr, CLOCK_REALTIME ) == 0 )
        return CLOCK_REALTIME;
#endif

    /* fall back to returning the current clock id */
    if ( pthread_condattr_getclock( cattr, &clockId) == 0 )
        return clockId;

    /* fall back to returning the default expected clock id */
    PA_DEBUG(( "%s: could not configure condattr clock\n", __FUNCTION__));
    return CLOCK_REALTIME;
#else /* not PAUTIL_USE_POSIX_ADVANCED_REALTIME */
    return 0; /* dummy value */
#endif
}

int PaPthreadUtil_GetTime( PaUtilClockId clockId, struct timespec *ts )
{
#if PAUTIL_USE_POSIX_ADVANCED_REALTIME
    if ( clock_gettime(clockId, ts) == 0 )
    {
        return 0; /* success */
    }
    PA_DEBUG(( "%s: clock_gettime failed with errno %d\n", __FUNCTION__, errno));
    ts->tv_sec = 0;
    ts->tv_nsec = 0;
    return -1; /* failure */

#else /* not PAUTIL_USE_POSIX_ADVANCED_REALTIME */

#if defined(WIN32) || defined(_WIN32)
    /* On Windows, the most likely pthreads implementations are pthreads4w,
       and winpthread via mingw-w64. Both use Unix time derived from Win32 SystemTime as the time base:
         https://sourceforge.net/p/pthreads4w/code/ci/master/tree/ptw32_timespec.c
         https://sourceforge.net/p/mingw-w64/mingw-w64/ci/master/tree/mingw-w64-libraries/winpthreads/src/misc.c
       The conversion from SystemTime to Unix time is based on this code: https://stackoverflow.com/a/26085827
       with reference to the pthreads4w code linked above.
    */
    FILETIME ft;
    UINT64 t1601; /* 100ns units since 00:00:00 UTC January 1, 1601 */
    UINT64 t1970; /* 100ns units since 00:00:00 UTC January 1, 1970 */

    GetSystemTimeAsFileTime( &ft );
    t1601 = ((UINT64)ft.dwHighDateTime << 32) + (UINT64)ft.dwLowDateTime;
/* The following constant is 134774 days in 100ns ticks. i.e. 134774*24*3600*10000000 */
#define SYSTEM_TIME_TO_UNIX_TIME_OFFSET (((UINT64)27111902UL << 32) + (UINT64)3577643008UL)
    t1970 = t1601 - SYSTEM_TIME_TO_UNIX_TIME_OFFSET;

    ts->tv_sec = (time_t) (t1970 / (UINT64)10000000UL);
    ts->tv_nsec = (long) ((t1970 - ((UINT64)ts->tv_sec * (UINT64)10000000UL)) * (UINT64)100UL);
    return 0; /* success */
#else
    /* fall back to gettimeofday for Apple and when clock_gettime is unavailable */
    struct timeval tv;
    if ( gettimeofday(&tv, NULL) == 0 )
    {
        ts->tv_sec = tv.tv_sec;
        ts->tv_nsec = tv.tv_usec * 1000UL;
        return 0; /* success */
    }
    PA_DEBUG(( "%s: gettimeofday failed with errno %d\n", __FUNCTION__, errno));
    ts->tv_sec = 0;
    ts->tv_nsec = 0;
    return -1; /* failure */
#endif

#endif
}
