#ifndef PA_LIB_H
#define PA_LIB_H

/*
 * Portable Audio I/O Library
 * integer type definitions
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2006 Ross Bencina, Phil Burk
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

 @brief Definition of PA_LIB_API and PA_LIB_LOCAL for selective export of
        PortAudio functions. Prefix must be placed in front of the
        function declaration to have it exported in the shared library.

        PA_BUILD_SHARED must be defined if shared library is built.

        PA_LIB_EXPORTS must be defined to export symbols (required for
        Windows platform).

        PA_LIB_API and PA_LIB_LOCAL do not affect static library.
*/

#ifdef PA_LIB_SHARED
    #if (defined(WIN32) || defined(_WIN32))
        #ifdef PA_LIB_EXPORTS
            #define PA_LIB_API __declspec(dllexport)
        #else
            #define PA_LIB_API __declspec(dllimport)
        #endif
        #define PA_LIB_LOCAL
    #else
        #if ((__GNUC__ >= 4) || defined(HAVE_GCC_VISIBILITY))
            #define PA_LIB_API __attribute__ ((visibility("default")))
            #define PA_LIB_LOCAL __attribute__ ((visibility("hidden")))
        #else
            #define PA_LIB_API
            #define PA_LIB_LOCAL
        #endif
    #endif
#else
    #define PA_LIB_API
    #define PA_LIB_LOCAL
#endif

#endif /* PA_LIB_H */
