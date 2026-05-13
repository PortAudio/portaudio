#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cmath>
#include <atomic>
#include <string>
#include "portaudio.h"
#include "pa_linux_alsa.h"

static std::atomic<bool> running(true);

void signal_handler(int) { running = false; }

struct LoopbackData {
    int channels;
    int frameCount;
};

static int loopbackCallback(const void *input, void *output,
                            unsigned long frameCount,
                            const PaStreamCallbackTimeInfo *timeInfo,
                            PaStreamCallbackFlags statusFlags,
                            void *userData)
{
    (void)timeInfo;
    auto *data = static_cast<LoopbackData *>(userData);
    const int32_t *in = static_cast<const int32_t *>(input);
    int32_t *out = static_cast<int32_t *>(output);

    if (statusFlags)
        fprintf(stderr, "STATUS: 0x%lx\n", statusFlags);

    int32_t maxVal = 0;
    for (unsigned long i = 0; i < frameCount * data->channels; ++i) {
        int32_t v = in[i] < 0 ? -in[i] : in[i];
        if (v > maxVal) maxVal = v;
        out[i] = in[i];
    }

    static int printCount = 0;
    if (++printCount <= 50 || printCount % 100 == 0)
        fprintf(stderr, "cb: frames=%lu max=%d\n", frameCount, maxVal);

    data->frameCount++;
    return running ? paContinue : paComplete;
}

void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -d <device>    Device name substring (default: hw:0,0)\n"
        "  -r <rate>      Sample rate (default: 48000)\n"
        "  -c <channels>  Channels (default: 2)\n"
        "  -b <blocksize> Frames per buffer (default: 512)\n"
        "  -D <string>    Use ALSA device string directly via PaAlsaStreamInfo\n"
        "  -h             Help\n", prog);
}

static PaDeviceIndex findDevice(const char *substr) {
    int n = Pa_GetDeviceCount();
    for (int i = 0; i < n; i++) {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
        if (info && info->maxInputChannels > 0 && info->maxOutputChannels > 0) {
            if (strstr(info->name, substr))
                return i;
        }
    }
    return paNoDevice;
}

int main(int argc, char *argv[]) {
    const char *deviceSubstr = "hw:0,0";
    const char *alsaDeviceString = nullptr;
    double sampleRate = 48000;
    int channels = 2;
    unsigned long blockSize = 512;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-d" && i + 1 < argc) deviceSubstr = argv[++i];
        else if (arg == "-r" && i + 1 < argc) sampleRate = std::stod(argv[++i]);
        else if (arg == "-c" && i + 1 < argc) channels = std::stoi(argv[++i]);
        else if (arg == "-b" && i + 1 < argc) blockSize = std::stoul(argv[++i]);
        else if (arg == "-D" && i + 1 < argc) alsaDeviceString = argv[++i];
        else if (arg == "-h") { print_usage(argv[0]); return 0; }
        else { fprintf(stderr, "Unknown: %s\n", argv[i]); print_usage(argv[0]); return 1; }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "Pa_Initialize failed: %s\n", Pa_GetErrorText(err));
        return 1;
    }

    fprintf(stderr, "PortAudio version: %s\n", Pa_GetVersionText());

    PaStreamParameters inputParams, outputParams;
    PaAlsaStreamInfo alsaInputInfo, alsaOutputInfo;

    if (alsaDeviceString) {
        PaAlsa_InitializeStreamInfo(&alsaInputInfo);
        alsaInputInfo.deviceString = alsaDeviceString;
        PaAlsa_InitializeStreamInfo(&alsaOutputInfo);
        alsaOutputInfo.deviceString = alsaDeviceString;

        inputParams.device = Pa_GetDefaultInputDevice();
        inputParams.hostApiSpecificStreamInfo = &alsaInputInfo;
        outputParams.device = Pa_GetDefaultOutputDevice();
        outputParams.hostApiSpecificStreamInfo = &alsaOutputInfo;
        fprintf(stderr, "Using ALSA device string: %s\n", alsaDeviceString);
    } else {
        PaDeviceIndex dev = findDevice(deviceSubstr);
        if (dev == paNoDevice) {
            fprintf(stderr, "No device matching '%s' with both input and output\n", deviceSubstr);
            fprintf(stderr, "Available devices:\n");
            int n = Pa_GetDeviceCount();
            for (int i = 0; i < n; i++) {
                const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
                if (info)
                    fprintf(stderr, "  [%d] %s (in=%d out=%d)\n", i, info->name,
                            info->maxInputChannels, info->maxOutputChannels);
            }
            Pa_Terminate();
            return 1;
        }
        const PaDeviceInfo *devInfo = Pa_GetDeviceInfo(dev);
        fprintf(stderr, "Using device [%d]: %s\n", dev, devInfo->name);

        inputParams.device = dev;
        inputParams.hostApiSpecificStreamInfo = nullptr;
        outputParams.device = dev;
        outputParams.hostApiSpecificStreamInfo = nullptr;
    }

    inputParams.channelCount = channels;
    inputParams.sampleFormat = paInt32;
    inputParams.suggestedLatency = 0.0;

    outputParams.channelCount = channels;
    outputParams.sampleFormat = paInt32;
    outputParams.suggestedLatency = 0.0;

    LoopbackData data = { channels, 0 };

    PaStream *stream = nullptr;
    err = Pa_OpenStream(&stream,
                        &inputParams, &outputParams,
                        sampleRate, blockSize,
                        paNoFlag,
                        loopbackCallback, &data);
    if (err != paNoError) {
        fprintf(stderr, "Pa_OpenStream failed: %s\n", Pa_GetErrorText(err));
        Pa_Terminate();
        return 1;
    }

    const PaStreamInfo *streamInfo = Pa_GetStreamInfo(stream);
    if (streamInfo) {
        fprintf(stderr, "Input latency:  %.3f ms\n", streamInfo->inputLatency * 1000);
        fprintf(stderr, "Output latency: %.3f ms\n", streamInfo->outputLatency * 1000);
        fprintf(stderr, "Sample rate:    %.0f\n", streamInfo->sampleRate);
    }

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        fprintf(stderr, "Pa_StartStream failed: %s\n", Pa_GetErrorText(err));
        Pa_CloseStream(stream);
        Pa_Terminate();
        return 1;
    }

    fprintf(stderr, "Loopback running — Ctrl+C to stop\n");

    while (running && Pa_IsStreamActive(stream))
        Pa_Sleep(100);

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();

    fprintf(stderr, "Done. %d callbacks processed.\n", data.frameCount);
    return 0;
}
