Portaudio implementation for android using opensles.

Building
----
To build portaudio with opensles an android NDK is needed to crosscompile it, version r9b has been used during development.

TODOs
----
Testing, loads of it.

Misc
----
Audio fast track:
Opensles on android has an audio_output_fast_flag which will be automatically set when you open a device with the right parameters. When this flag is set latency will be reduced dramatically between opensles and the android HAL. The samplerate needed for the fast track flag can be queried using AudioManager's getProperty(PROPERTY_OUTPUT_SAMPLE_RATE).


Latency:
Default latencies and precise stream latencies are hard to calculate since these will depend on device, android version, and whether or not the audio fast track flag has been set. At the moment this implementation only takes into account the user buffers and the opensles buffers.


Buffer sizes:
When using portaudio opensles in an android application it is recommended to query the AudioManager's getProperty(PROPERTY_OUTPUT_FRAMES_PER_BUFFER). Using this buffer size or a multiple of this buffer size is recommended. The implementation does not have any lower bound on the framesPerBuffer argument in Pa_Openstream. This way newer devices can take advantage of improving audio latency on android.


Querying audio capabilities:
OpenSL ES on android does not support the OpenSL ES SLAudioIODeviceCapabilitiesItf. As a result PaOpenSLES_Initialize populates the devices with some amount of heuristics. We simply use one output/input device. And starting API 14 use the SLAndroidConfigurationItf to configure the stream type of said devices. This also means that testing for the support of a certain samplerate or amount of channels quikcly opens and closes the device and checks for errors. This is a pretty dirty way of doing this, suggestions welcomed.


Prefetch underflow assert that fails:
There is an assert(SL_PREFETCHSTATUS_UNDERFLOW == ap->mPrefetchStatus.mStatus) that fails (Src https://android.googlesource.com/platform/frameworks/wilhelm/+/master/src/android/AudioPlayer_to_android.cpp) sometimes with a non-blocking audio output.

This is an assert which tries to make sure that the prefetchitf was in SL_PREFETCHSTATUS_UNDERFLOW, when trying to re-enqueue/restart the buffers because the user didn't enqueue buffers for a while (and the buffers underflowed). This doesn't happen when you can supply the buffer fast enough of course. But the specification states that if you stop enqueueing buffers, it should just stop playing, if you enqueue buffers again, it should start up again, and it shouldn't abort the entire application.

As a result I disabled the prefetchstatus callback for now, so there is no formation available to the user whether the buffers underflowed or overflowed, but at least portaudio just keeps running.


Number of buffers:
As mentioned in the android opensles documentation under performance, the number of buffer sizes can be 1 on api >17
