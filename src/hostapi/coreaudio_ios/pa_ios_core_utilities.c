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

#include "pa_ios_core_utilities.h"
#include "pa_ios_core_internal.h"

#include <libkern/OSAtomic.h>

#include <strings.h>
#include <pthread.h>

#include <sys/time.h>

PaError
PaIosCore_SetUnixError(int err, int line)
{
	PaError ret;
	const char *errorText;

	if (err == 0)
		return paNoError;

	ret = paNoError;

	errorText = strerror(err);

	if (err == ENOMEM)
		ret = paInsufficientMemory;
	else
		ret = paInternalError;

	PaUtil_SetLastHostErrorInfo(paCoreAudio, err, errorText);

	return (ret);
}

PaError
PaIosCore_SetError(OSStatus error, int line, int isError)
{
	PaError result;
	const char *errorType;
	const char *errorText;

	switch (error) {
	case kAudioServicesNoError:
		return paNoError;
	case kAudioFormatUnspecifiedError:
		errorText = "Unspecified Audio Format Error";
		result = paInternalError;
		break;
	case kAudioFormatUnknownFormatError:
		errorText = "Audio Format: Unknown Format Error";
		result = paInternalError;
		break;
	case kAudioFormatBadPropertySizeError:
		errorText = "Audio Format: Bad Property Size";
		result = paInternalError;
		break;
	case kAudioFormatUnsupportedPropertyError:
		errorText = "Audio Format: Unsupported Property Error";
		result = paInternalError;
		break;
	case kAudioUnitErr_InvalidProperty:
		errorText = "Audio Unit: Invalid Property";
		result = paInternalError;
		break;
	case kAudioUnitErr_InvalidParameter:
		errorText = "Audio Unit: Invalid Parameter";
		result = paInternalError;
		break;
	case kAudioUnitErr_NoConnection:
		errorText = "Audio Unit: No Connection";
		result = paInternalError;
		break;
	case kAudioUnitErr_FailedInitialization:
		errorText = "Audio Unit: Initialization Failed";
		result = paInternalError;
		break;
	case kAudioUnitErr_TooManyFramesToProcess:
		errorText = "Audio Unit: Too Many Frames";
		result = paInternalError;
		break;
	case kAudioUnitErr_IllegalInstrument:
		errorText = "Audio Unit: Illegal Instrument";
		result = paInternalError;
		break;
	case kAudioUnitErr_InstrumentTypeNotFound:
		errorText = "Audio Unit: Instrument Type Not Found";
		result = paInternalError;
		break;
	case kAudioUnitErr_InvalidFile:
		errorText = "Audio Unit: Invalid File";
		result = paInternalError;
		break;
	case kAudioUnitErr_UnknownFileType:
		errorText = "Audio Unit: Unknown File Type";
		result = paInternalError;
		break;
	case kAudioUnitErr_FileNotSpecified:
		errorText = "Audio Unit: File Not Specified";
		result = paInternalError;
		break;
	case kAudioUnitErr_FormatNotSupported:
		errorText = "Audio Unit: Format Not Supported";
		result = paInternalError;
		break;
	case kAudioUnitErr_Uninitialized:
		errorText = "Audio Unit: Unitialized";
		result = paInternalError;
		break;
	case kAudioUnitErr_InvalidScope:
		errorText = "Audio Unit: Invalid Scope";
		result = paInternalError;
		break;
	case kAudioUnitErr_PropertyNotWritable:
		errorText = "Audio Unit: PropertyNotWritable";
		result = paInternalError;
		break;
	case kAudioUnitErr_InvalidPropertyValue:
		errorText = "Audio Unit: Invalid Property Value";
		result = paInternalError;
		break;
	case kAudioUnitErr_PropertyNotInUse:
		errorText = "Audio Unit: Property Not In Use";
		result = paInternalError;
		break;
	case kAudioUnitErr_Initialized:
		errorText = "Audio Unit: Initialized";
		result = paInternalError;
		break;
	case kAudioUnitErr_InvalidOfflineRender:
		errorText = "Audio Unit: Invalid Offline Render";
		result = paInternalError;
		break;
	case kAudioUnitErr_Unauthorized:
		errorText = "Audio Unit: Unauthorized";
		result = paInternalError;
		break;
	case kAudioUnitErr_CannotDoInCurrentContext:
		errorText = "Audio Unit: cannot do in current context";
		result = paInternalError;
		break;
	default:
		errorText = "Unknown Error";
		result = paInternalError;
		break;
	}

	if (isError)
		errorType = "Error";
	else
		errorType = "Warning";

	char str[20];

	/* see if it appears to be a 4-char-code */
	*(UInt32 *) (str + 1) = CFSwapInt32HostToBig(error);

	if (isprint(str[1]) && isprint(str[2]) && isprint(str[3]) && isprint(str[4])) {
		str[0] = str[5] = '\'';
		str[6] = '\0';
	} else {
		/* no, format it as an integer */
		snprintf(str, sizeof(str), "%d", (int)error);
	}

	PaUtil_SetLastHostErrorInfo(paCoreAudio, error, errorText);

	return (result);
}
