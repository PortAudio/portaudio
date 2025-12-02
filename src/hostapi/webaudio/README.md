# Web Audio API

To build PortAudio for the web, make sure to have [the Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) installed and on your `PATH`, then run from the repository's top-level directory:

```sh
emcmake cmake -B build
cmake --build build
```

To build the examples, use

```sh
emcmake cmake -B build -DPA_BUILD_EXAMPLES=ON
cmake --build build
```

> [!TIP]
> By default PortAudio will be built with `-sASYNCIFY`. This makes it safe to be called from the main thread, but also introduces overhead and potentially other issues when the application already uses other asynchronous operations. PortAudio can also be built without Asyncify (i.e. using synchronous/blocking operations) by setting `-DPA_WEBAUDIO_ASYNCIFY=OFF`, in that case it is recommended to call PortAudio from a worker thread to avoid blocking the main thread (which may result in locking up the UI or in worse cases deadlock the application, e.g. if PortAudio waits for user interaction, as it does during `Pa_StartStream`). This can be done automatically using `-sPROXY_TO_PTHREAD`.

> [!TIP]
> For debug logging, set `-DPA_ENABLE_DEBUG_OUTPUT=ON`

You can now run the examples in a local browser using `emrun`, for example

```sh
emrun build/examples/paex_sine.html
```

> [!IMPORTANT]
> Due to browser policies you have to interact with the site at least once (e.g. by clicking anywhere) before audio contexts can be started. `Pa_StartStream` will (from the C/C++ perspective) block until this happens. Under the hood this is handled asynchronously using Asyncify.

> [!TIP]
> You can customize the browser e.g. by setting `--browser=firefox` and also pass arguments to the browser with `--browser-args`
