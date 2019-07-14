/*
 * Helper and utility functions for pa_ios_core.c (Apple AUHAL implementation)
 *
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 *
 * Written by Bjorn Roche of XO Audio LLC, from PA skeleton code.
 * Portions copied from code by Dominic Mazzoni (who wrote a HAL implementation)
 *
 * Dominic's code was based on code by Phil Burk, Darren Gibbs,
 * Gord Peters, Stephane Letz, and Greg Pfiel.
 *
 * The following people also deserve acknowledgements:
 *
 * Olivier Tristan for feedback and testing
 * Glenn Zelniker and Z-Systems engineering for sponsoring the Blocking I/O
 * interface.
 * 
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2002 Ross Bencina, Phil Burk
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

/**
 @file
 @ingroup hostapi_src
*/

#ifndef PA_IOS_CORE_UTILITIES_H__
#define PA_IOS_CORE_UTILITIES_H__

#include <pthread.h>

#include "portaudio.h"

#include "pa_util.h"

#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>

#ifndef MIN
#define MIN(a, b)  (((a)<(b))?(a):(b))
#endif

#ifndef MAX
#define MAX(a, b)  (((a)<(b))?(b):(a))
#endif

#define ERR(ios_error) PaIosCore_SetError(ios_error, __LINE__, 1 ) 
#define WARNING(ios_error) PaIosCore_SetError(ios_error, __LINE__, 0 )


/* Help keep track of AUHAL element numbers */
#define INPUT_ELEMENT  (1)
#define OUTPUT_ELEMENT (0)

/* Normal level of debugging: fine for most apps that don't mind the occational warning being printf'ed */
/*
 */
#define IOS_CORE_DEBUG
#ifdef IOS_CORE_DEBUG
# define DBUG(MSG) do { printf("||PaIosCore (AUHAL)|| "); printf MSG ; fflush(stdout); } while(0)
#else
# define DBUG(MSG)
#endif

/* Verbose Debugging: useful for developement */
/*
#define IOS_CORE_VERBOSE_DEBUG
*/
#ifdef IOS_CORE_VERBOSE_DEBUG
# define VDBUG(MSG) do { printf("||PaIosCore (v )|| "); printf MSG ; fflush(stdout); } while(0)
#else
# define VDBUG(MSG)
#endif

/* Very Verbose Debugging: Traces every call. */
/*
#define IOS_CORE_VERY_VERBOSE_DEBUG
 */
#ifdef IOS_CORE_VERY_VERBOSE_DEBUG
# define VVDBUG(MSG) do { printf("||PaIosCore (vv)|| "); printf MSG ; fflush(stdout); } while(0)
#else
# define VVDBUG(MSG)
#endif

#define	UNIX_ERR(err) \
	PaIosCore_SetUnixError(err, __LINE__)

PaError PaIosCore_SetUnixError(int err, int line);
PaError PaIosCore_SetError(OSStatus error, int line, int isError);

#endif /* PA_IOS_CORE_UTILITIES_H__*/
