/*
 * Mac spcific flags for PA.
 * portaudio.h should be included before this file.
 */

/*
 * A pointer to a paMacCoreStreamInfo may be passed as
 * the hostApiSpecificStreamInfo in the PaStreamParameters struct
 * when opening a stream. Use NULL, for the defaults. Note that for
 * duplex streams, both infos should be the same or behaviour
 * is undefined.
 */
typedef struct paMacCoreStreamInfo
{
    unsigned long size;         /**size of whole structure including this header */
    PaHostApiTypeId hostApiType;/**host API for which this data is intended */
    unsigned long version;      /**structure version */
    unsigned long flags;        /* flags to modify behaviour */
} paMacCoreStreamInfo;

/* Use this function to initialize a paMacCoreStreamInfo struct
   using the requested flags. */
void paSetupMacCoreStreamInfo( paMacCoreStreamInfo *data, unsigned long flags )
{
   bzero( data, sizeof( paMacCoreStreamInfo ) );
   data->size = sizeof( paMacCoreStreamInfo );
   data->hostApiType = paCoreAudio;
   data->version = 0x01;
   data->flags = flags;
}

/*
 * The following flags alter the behaviour of PA on the mac platform.
 * they can be ORed together. These should work both for opening and
 * checking a device.
 */
/* Allows PortAudio to change things like the device's frame size,
 * which allows for much lower latency, but might disrupt the device
 * if other programs are using it. */
const unsigned long paMacCore_ChangeDeviceParameters      = 0x01;

/* In combination with the above flag,
 * causes the stream opening to fail, unless the exact sample rates
 * are supported by the device. */
const unsigned long paMacCore_FailIfConversionRequired    = 0x02;

/* These flags set the SR conversion quality, if required. The wierd ordering
 * allows Maximum Quality to be the default.*/
const unsigned long paMacCore_ConversionQualityMin    = 0x0100;
const unsigned long paMacCore_ConversionQualityMedium = 0x0200;
const unsigned long paMacCore_ConversionQualityLow    = 0x0300;
const unsigned long paMacCore_ConversionQualityHigh   = 0x0400;
const unsigned long paMacCore_ConversionQualityMax    = 0x0000;

/*
 * Here are some "preset" combinations of flags (above) to get to some
 * common configurations. THIS IS OVERKILL, but if more flags are added
 * it won't be.
 */
/*This is the default setting: do as much sample rate conversion as possible
 * and as little mucking with the device as possible. */
const unsigned long paMacCorePlayNice = 0x00;
/*This setting is tuned for pro audio apps. It allows SR conversion on input
  and output, but it tries to set the appropriate SR on the device.*/
const unsigned long paMacCorePro      = 0x01;
/*This is a setting to minimize CPU usage and still play nice.*/
const unsigned long paMacCoreMinimizeCPUButPlayNice = 0x0100;
/*This is a setting to minimize CPU usage, even if that means interrupting the device. */
const unsigned long paMacCoreMinimizeCPU = 0x0101;
