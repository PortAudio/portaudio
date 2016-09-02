#include <stdio.h>
#include <assert.h>

#include "portaudio.h"

void printDevices()
{
    int deviceCount = Pa_GetDeviceCount();
    int i;

    for( i=0; i < deviceCount; ++i ){
        const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(i);
        const PaHostApiInfo *hostApiInfo = Pa_GetHostApiInfo( deviceInfo->hostApi );

        assert( deviceInfo != 0 );
        assert( deviceInfo->structVersion >= 3 ); /* should be the case if all APIs have implemented connectionId */

        printf( "%d (conn id: %d) %s (%s)\n", i, deviceInfo->connectionId, deviceInfo->name, hostApiInfo->name );
    }
}

static void devicesChangedCallback(void* p)
{
    (void)p;

    printf( "Portaudio device list have changed!\n" );
}

int main(int argc, char* argv[])
{
    Pa_Initialize();

    Pa_SetDevicesChangedCallback(NULL, devicesChangedCallback);

    for(;;){
        printDevices();

        printf( "press [enter] to update the device list. or q + [enter] to quit.\n" );
        if( getchar() == 'q' )
            break;

        Pa_RefreshDeviceList();
    }

    Pa_Terminate();

	return 0;
}

