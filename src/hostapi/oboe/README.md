# Portaudio implementation for android using Oboe.

In order to use this implementation correctly, be sure to include the "portaudio.h" and "pa_oboe.h"  
headers in your project.

Building:
----  
To build portaudio with Oboe, there are some necessary steps:
1) An android NDK is needed to crosscompile it. I used the version 25.1.8937393, which I found at https://developer.android.com/ndk/downloads.
2) Clone the Oboe repository - just follow the steps detailed here: https://github.com/google/oboe/blob/main/docs/GettingStarted.md.
   Make sure to correctly link the NDK path in the Oboe build.
3) Set the CMake variable OBOE_DIR (used in cmake/modules/FindOboe.cmake) to the path of the cloned Oboe repository.
4) Build the Oboe Library (you can use "build_all_android.sh").
5) Build PaOboe (you can use "build_all_PaOboe.sh").
6) Don't forget to add liboboe.so and libportaudio.so in your jniLibs folder.

TODOs:
----  
- Testing. This implementation was tested for VoIP calls that use blocking streams - for everything else, lots of tests are needed.

Misc
----  
### Audio Format:
If you need to select a specific audio format, you'll have to manually set it in PaOboe_OpenStream  by modifying the format selection marked with a *FIXME*.
I'm positive that automatic format selection is possible, but simply using  PaUtil_SelectClosestAvailableFormat got me nowhere.


### Buffer sizes:
Portaudio often tries to get approximately low buffer sizes, and if you need specific sizes for your  buffer you should manually modify it (or make a simple function that can set it). For your convenience,  there is a *FIXME* as a bookmark.