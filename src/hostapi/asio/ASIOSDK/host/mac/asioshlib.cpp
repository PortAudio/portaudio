// asio code fragment 'linker'
// mac specific
// loads the symbols via the fragment manager, and
// translates them into function pointers. this
// means no asio lib must be linked, as the
// 'real' asio functions are here

#include "ginclude.h"
#include <macheaders.c>
#include <string.h>
#include <CodeFragments.h>
#include "asio.h"

// Macro below has to 0 if external drivers will be used.
// Can be set to 1 if a specific ASIO driver should be linked into the program
#define ASIOINCLUDED 0



// export
bool resolveASIO(unsigned long aconnID);

//------------------------------------------------------------------------------------------------------
// private

#if ASIOINCLUDED
bool resolveASIO(unsigned long aconnID) {aconnID = aconnID; return true;}
#else

enum
{
	kASIOInit = 0,
	kASIOExit,
	kASIOStart,
	kASIOStop,
	kASIOGetChannels,
	kASIOGetLatencies,
	kASIOGetBufferSize,
	kASIOCanSampleRate,
	kASIOGetSampleRate,
	kASIOSetSampleRate,
	kASIOGetClockSources,
	kASIOSetClockSource,
	kASIOGetSamplePosition,
	kASIOGetChannelInfo,
	kASIOCreateBuffers,
	kASIODisposeBuffers,
	kASIOControlPanel,
	kASIOFuture,

	kASIOOutputReady,

	kNumSymbols,
	kRequiredSymbols = kNumSymbols - 1
}; 

static char *asioTable[kNumSymbols] = 
{
	"ASIOInit",
	"ASIOExit",
	"ASIOStart",
	"ASIOStop",
	"ASIOGetChannels",
	"ASIOGetLatencies",
	"ASIOGetBufferSize",
	"ASIOCanSampleRate",
	"ASIOGetSampleRate",
	"ASIOSetSampleRate",
	"ASIOGetClockSources",
	"ASIOSetClockSource",
	"ASIOGetSamplePosition",
	"ASIOGetChannelInfo",
	"ASIOCreateBuffers",
	"ASIODisposeBuffers",
	"ASIOControlPanel",
	"ASIOFuture",
	"ASIOOutputReady"
};

typedef ASIOError (*fasioInit) (ASIODriverInfo *info);
typedef ASIOError (*fasioExit) (void);
typedef ASIOError (*fasioStart) (void);
typedef ASIOError (*fasioStop) (void);
typedef ASIOError (*asioGetChannels) (long *numInputChannels, long *numOutputChannels);
typedef ASIOError (*asioGetLatencies) (long *inputLatency, long *outputLatency);
typedef ASIOError (*asioGetBufferSize) (long *minSize, long *maxSize, long *preferredSize, long *granularity);
typedef ASIOError (*asioCanSampleRate) (ASIOSampleRate sampleRate);
typedef ASIOError (*asioGetSampleRate) (ASIOSampleRate *currentRate);
typedef ASIOError (*asioSetSampleRate) (ASIOSampleRate sampleRate);
typedef ASIOError (*asioGetClockSources) (ASIOClockSource *clocks, long *numSources);
typedef ASIOError (*asioSetClockSource) (long reference);
typedef ASIOError (*asioGetSamplePosition) (ASIOSamples *sPos, ASIOTimeStamp *tStamp);
typedef ASIOError (*asioGetChannelInfo) (ASIOChannelInfo *info);
typedef ASIOError (*asioCreateBuffers) (ASIOBufferInfo *channelInfos, long numChannels,
	long bufferSize, ASIOCallbacks *callbacks);
typedef ASIOError (*asioDisposeBuffers) (void);
typedef ASIOError (*asioControlPanel) (void);
typedef ASIOError (*asioFuture)(long selector, void *opt);
typedef ASIOError (*asioOutputReady) (void);

static void *functionTable[kNumSymbols] = {0};
static bool inited = false;

//---------------------------------------------------------------------------------------------------------

#include <CodeFragments.h>

bool resolveASIO(unsigned long aconnID)
{
	OSErr err;
	CFragConnectionID connID = (CFragConnectionID)aconnID;
	CFragSymbolClass symClass;
	Ptr symAddr = nil;
	Str255 myName;
	long myCount = 0;

	err = CountSymbols(connID, &myCount);
	if(err != noErr || myCount < (kRequiredSymbols + 1))	// there must be a main()
		return false;

	long n, c = 0;
	for(n = kRequiredSymbols; n < kNumSymbols; n++)
		functionTable[n] = 0;
	for(n = 0; n < myCount; n++)
	{
		// we can't use FindSymbol(), as the compiler adds mangling
		// (such as ASIOInit__Fv). also, the symbols don't appear
		// in the order they are declared or implemented.
		// so we use strncmp()
 		err = GetIndSymbol(connID, n, myName, &symAddr, &symClass);
		if(err != noErr)
			break;
		PtoCstr(myName);
		if(!strncmp((char *)myName, "main", 4L))
			c++;
		else
		{
			for(long i = 0; i < kNumSymbols; i++)
			{
				long sc = strlen(asioTable[i]);
				if(!strncmp((char *)myName, asioTable[i], sc))
				{
					functionTable[i] = symAddr;
					if(i < kRequiredSymbols)
						c++;
					break;
				}
			}
		}
		// if(c >= kNumSymbols + 1)
		//	break;
	}
	if(c >= kRequiredSymbols + 1)
	{
		inited = true;
		return true;
	}
	return false;
}

//---------------------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------------------

#pragma export on

static short curRes = 0;
#define saveRes() curRes = CurResFile()
#define restRes() UseResFile(curRes);


ASIOError ASIOInit(ASIODriverInfo *info)
{
	saveRes();
	strcpy(info->errorMessage, "No ASIO Driver could be Loaded!");
	if(!inited)
		return ASE_NotPresent;
	fasioInit f = (fasioInit)functionTable[kASIOInit];
	ASIOError e = (*f)(info);
	restRes();
	return e;
}

ASIOError ASIOExit(void)
{
	if(!inited)
		return ASE_NotPresent;
	saveRes();
	fasioExit f = (fasioExit)functionTable[kASIOExit];
	ASIOError e = (*f)();
	restRes();
	return e;
}

ASIOError ASIOStart(void)
{
	if(!inited)
		return ASE_NotPresent;
	fasioStart f = (fasioStart)functionTable[kASIOStart];
	return (*f)();
}

ASIOError ASIOStop(void)
{
	if(!inited)
		return ASE_NotPresent;
	fasioStop f = (fasioStop)functionTable[kASIOStop];
	return (*f)();
}

ASIOError ASIOGetChannels(long *numInputChannels, long *numOutputChannels)
{
	if(!inited)
		return ASE_NotPresent;
	asioGetChannels f = (asioGetChannels)functionTable[kASIOGetChannels];
	return (*f)(numInputChannels, numOutputChannels);
}

ASIOError ASIOGetLatencies(long *inputLatency, long *outputLatency)
{
	if(!inited)
		return ASE_NotPresent;
	asioGetLatencies f = (asioGetLatencies)functionTable[kASIOGetLatencies];
	return (*f)(inputLatency, outputLatency);
}

ASIOError ASIOGetBufferSize(long *minSize, long *maxSize, long *preferredSize, long *granularity)
{
	if(!inited)
		return ASE_NotPresent;
	asioGetBufferSize f = (asioGetBufferSize)functionTable[kASIOGetBufferSize];
	return (*f)(minSize, maxSize, preferredSize, granularity);
}

ASIOError ASIOCanSampleRate(ASIOSampleRate sampleRate)
{
	if(!inited)
		return ASE_NotPresent;
	asioCanSampleRate f = (asioCanSampleRate)functionTable[kASIOCanSampleRate];
	return (*f)(sampleRate);
}

ASIOError ASIOGetSampleRate(ASIOSampleRate *currentRate)
{
	if(!inited)
		return ASE_NotPresent;
	asioGetSampleRate f = (asioGetSampleRate)functionTable[kASIOGetSampleRate];
	return (*f)(currentRate);
}

ASIOError ASIOSetSampleRate(ASIOSampleRate sampleRate)
{
	if(!inited)
		return ASE_NotPresent;
	asioSetSampleRate f = (asioSetSampleRate)functionTable[kASIOSetSampleRate];
	return (*f)(sampleRate);
}

ASIOError ASIOGetClockSources(ASIOClockSource *clocks, long *numSources)
{
	if(!inited)
		return ASE_NotPresent;
	asioGetClockSources f = (asioGetClockSources)functionTable[kASIOGetClockSources];
	return (*f)(clocks, numSources);
}

ASIOError ASIOSetClockSource(long reference)
{
	if(!inited)
		return ASE_NotPresent;
	asioSetClockSource f = (asioSetClockSource)functionTable[kASIOSetClockSource];
	return (*f)(reference);
}

ASIOError ASIOGetSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp)
{
	if(!inited)
		return ASE_NotPresent;
	asioGetSamplePosition f = (asioGetSamplePosition)functionTable[kASIOGetSamplePosition];
	return (*f)(sPos, tStamp);
}

ASIOError ASIOGetChannelInfo(ASIOChannelInfo *info)
{
	if(!inited)
		return ASE_NotPresent;
	asioGetChannelInfo f = (asioGetChannelInfo)functionTable[kASIOGetChannelInfo];
	return (*f)(info);
}

ASIOError ASIOCreateBuffers(ASIOBufferInfo *channelInfos, long numChannels, long bufferSize, ASIOCallbacks *callbacks)
{
	if(!inited)
		return ASE_NotPresent;
	saveRes();
	asioCreateBuffers f = (asioCreateBuffers)functionTable[kASIOCreateBuffers];
	ASIOError e = (*f)(channelInfos, numChannels, bufferSize, callbacks);
	restRes();
	return e;
}

ASIOError ASIODisposeBuffers(void)
{
	if(!inited)
		return ASE_NotPresent;
	asioDisposeBuffers f = (asioDisposeBuffers)functionTable[kASIODisposeBuffers];
	return (*f)();
}

ASIOError ASIOControlPanel(void)
{
	if(!inited)
		return ASE_NotPresent;
	saveRes();
	asioControlPanel f = (asioControlPanel)functionTable[kASIOControlPanel];
	ASIOError e = (*f)();
	restRes();
	return e;
}

ASIOError ASIOFuture(long selector, void *opt)
{
	if(!inited)
		return 0;
	saveRes();
	asioFuture f = (asioFuture)functionTable[kASIOFuture];
	ASIOError e = (*f)(selector, opt);
	restRes();
	return e;
}

ASIOError ASIOOutputReady(void)
{
	asioOutputReady f = (asioControlPanel)functionTable[kASIOOutputReady];
	if(!inited || !f)
		return ASE_NotPresent;
	return (*f)();
}

#pragma export off

#endif	// ASIOINCLUDED

