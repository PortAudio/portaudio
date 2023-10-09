# Portaudio implementation for android using Oboe.

In order to use this implementation correctly, be sure to include the "portaudio.h" and "pa_oboe.h"  
headers in your project.

Building:
----  
To build portaudio with Oboe, there are some necessary steps:
1) An android NDK is needed to crosscompile it. I used the version 25.1.8937393, which I found at
   https://developer.android.com/ndk/downloads.
2) Clone the Oboe repository - just follow the steps detailed here:
   https://github.com/google/oboe/blob/main/docs/GettingStarted.md.
   Make sure to correctly link the NDK path in the Oboe build. If you instead prefer to use the
   prebuilt libraries, you can skip this step.
3) Set the CMake variable OBOE_DIRECTORY (used in cmake/modules/FindOboe.cmake) to the path of the
   cloned Oboe repository, and build the Oboe libraries (you can use "build_all_android.sh").

   If you instead used the prebuilt libraries, do the following:
   - set OBOE_DIRECTORY to TRUE;
   - set OBOE_LIBRARY_DIRS path_to_Oboe_libraries_folder/${ANDROID_ABI}), the code will search the
     prebuilt library of the chosen ABI in that folder.

4) Build PaOboe (you can use "build_all_PaOboe.sh").
5) Don't forget to add liboboe.so and libportaudio.so in your jniLibs folder.

TODOs:
----  
- Testing. This implementation is being tested for VoIP calls that use blocking streams - for
  everything else, lots of tests are needed.

Misc
----  
### Audio Format:
If you need to select a specific audio format, you'll have to manually set it in PaOboe_OpenStream 
by modifying the format selection marked with a *FIXME*. I'm positive that automatic format selection 
is possible, but simply using  PaUtil_SelectClosestAvailableFormat got me nowhere.


### Buffer sizes:
Portaudio often tries to get approximately low buffer sizes, and if you need specific sizes for your
buffer you should manually modify it (or make a simple function that can set it). For your convenience,
there is a *FIXME* as a bookmark.