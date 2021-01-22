ADD_EXECUTABLE(paloopback
    src/audio_analyzer.c
    src/biquad_filter.c
    src/paqa.c
    src/paqa_tools.c
    src/test_audio_analyzer.c
    src/write_wav.c
)
TARGET_INCLUDE_DIRECTORIES(paloopback PRIVATE ..)
TARGET_LINK_LIBRARIES(paloopback portaudio)
