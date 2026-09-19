# Portaudio implementation for android using Oboe.

In order to use this implementation correctly, be sure to include the "portaudio.h" and "pa_oboe.h"  
headers in your project.

Building:
----  
To build portaudio with Oboe, there are some necessary steps:
1) Install android SDK and NDK to crosscompile it. If you are using `sdkmanager`
   ```sh
   sdkmanager "platforms;android-${ANDROID_API}" "platform-tools" "build-tools;${ANDROID_VERSION}" "ndk;${ANDROID_NDK}"
   ```
2) [Install](https://github.com/google/oboe/blob/main/docs/GettingStarted.md#adding-oboe-to-your-project) the oboe header and library in your prefix. You may use [build_all_android.sh](https://github.com/google/oboe/blob/main/build_all_android.sh) from the Oboe project to easily build the Oboe library
3) Build PortAudio. Oboe support confirmation should display during the CMake init.
   ```plain
   ...
   -- Found Oboe: /workspaces/portaudio/build/oboe/lib/x86/liboboe.so;/opt/android-sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/i686-linux-android/21/liblog.so
   --   Oboe structure found
   ...
   ```

TODOs:
----  
- Testing. This implementation is being tested for VoIP calls that use blocking streams - for
  everything else, lots of tests are needed.

Misc
----  
### Device detection

Devices detection is not possible in NDK, so you will need to register devices using `PaOboe_RegisterDevice` (see [include/pa_oboe.h](include/pa_oboe.h) for reference). 
Here is an example on how to do with pure C++ and the JNI interface using the Qt framework.

```cpp
QJniObject context = QNativeInterface::QAndroidApplication::context();
QJniObject AUDIO_SERVICE =
         QJniObject::getStaticObjectField(
                  "android/content/Context",
                  "AUDIO_SERVICE",
                  "Ljava/lang/String;");
auto audioManager = context.callObjectMethod("getSystemService",
         "(Ljava/lang/String;)Ljava/lang/Object;",
         AUDIO_SERVICE.object());
if (!audioManager.isValid()) {
      qDebug() << "audioManager invalid";
      return;
}
qDebug() << "audioManager valid:" << audioManager.toString();

jint GET_DEVICES_INPUTS =
         QJniObject::getStaticField<jint>(
                  "android/media/AudioManager",
                  "GET_DEVICES_INPUTS");
jint GET_DEVICES_OUTPUTS =
         QJniObject::getStaticField<jint>(
                  "android/media/AudioManager",
                  "GET_DEVICES_OUTPUTS");

auto const isSupported = [](int type) {
      switch (type) {
      case 1:  // AudioDeviceInfo.TYPE_BUILTIN_EARPIECE
      case 2:  // AudioDeviceInfo.TYPE_BUILTIN_SPEAKER
      case 3:  // AudioDeviceInfo.TYPE_WIRED_HEADSET
      case 8:  // AudioDeviceInfo.TYPE_BLUETOOTH_A2DP
      case 11: // AudioDeviceInfo.TYPE_USB_DEVICE
      case 22: // AudioDeviceInfo.TYPE_USB_HEADSET
      case 9:  // AudioDeviceInfo.TYPE_HDMI
      case 10: // AudioDeviceInfo.TYPE_HDMI_ARC
      case 13: // AudioDeviceInfo.TYPE_DOCK
      case 15: // AudioDeviceInfo.TYPE_BUILTIN_MIC
      case 12: // AudioDeviceInfo.TYPE_USB_ACCESSORY
      case 26: // AudioDeviceInfo.TYPE_BLE_HEADSET
      case 27: // AudioDeviceInfo.TYPE_BLE_SPEAKER
      case 23: // AudioDeviceInfo.TYPE_HEARING_AID
      case 25: // AudioDeviceInfo.TYPE_REMOTE_SUBMIX:
         // supported
         return true;
      default:
         // unsupported
         break;
      }
      return false;
};
auto const parse = [isSupported](PaOboe_Direction direction,
                           QJniArray<QJniObject>& devices) {
      for (const auto& device : devices) {
         jint type = device->callMethod<jint>("getType");
         if (!isSupported(type)) {
            continue;
         }
         QString name = device->callObjectMethod("getProductName",
                                       "()Ljava/lang/CharSequence;")
                                 .toString();
         auto channelCounts = device->callMethod<QJniArray<jint>>("getChannelCounts");
         int channelCount = *std::max_element(
                  channelCounts.begin(), channelCounts.end());
         auto sampleRates = device->callMethod<QJniArray<jint>>("getSampleRates");
         if (!sampleRates.isEmpty()) {
            int sampleRate = *sampleRates.cbegin();
            PaOboe_RegisterDevice(name.toStdString().c_str(),
                     direction,
                     channelCount,
                     sampleRate);
         }
      }
};

auto inputDevices =
         audioManager.callMethod<QJniArray<QJniObject>>("getDevices",
                  "(I)[Landroid/media/AudioDeviceInfo;",
                  GET_DEVICES_INPUTS);
parse(PaOboe_Direction::Input, inputDevices);

auto outputDevices =
         audioManager.callMethod<QJniArray<QJniObject>>("getDevices",
                  "(I)[Landroid/media/AudioDeviceInfo;",
                  GET_DEVICES_OUTPUTS);
parse(PaOboe_Direction::Output, outputDevices);

QJniObject PROPERTY_OUTPUT_FRAMES_PER_BUFFER =
         QJniObject::getStaticField<jstring>(
                  "android/media/AudioManager",
                  "PROPERTY_OUTPUT_FRAMES_PER_BUFFER");
auto outputFramePerBuffer =
         audioManager
                  .callMethod<jstring>("getProperty",
                        "(Ljava/lang/String;)Ljava/lang/String;",
                        PROPERTY_OUTPUT_FRAMES_PER_BUFFER)
                  .toString()
                  .toUInt();

// Use PortAudio
Pa_Initialize();
// ...
```

### Buffer sizes:

Portaudio often tries to get approximately low buffer sizes, and if you need specific sizes for your
buffer you should manually modify it (or make a simple function that can set it). For your convenience,
there is a *FIXME* as a bookmark.