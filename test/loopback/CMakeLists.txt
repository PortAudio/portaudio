ADD_EXECUTABLE(paloopback
    audio_analyzer.c
    biquad_filter.c
    paqa.c
    paqa_tools.c
    test_audio_analyzer.c
    write_wav.c
)
TARGET_INCLUDE_DIRECTORIES(paloopback PRIVATE ..)
TARGET_LINK_LIBRARIES(paloopback portaudio)
