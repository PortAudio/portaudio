add_executable(paloopback
    src/audio_analyzer.c
    src/biquad_filter.c
    src/paqa.c
    src/paqa_tools.c
    src/test_audio_analyzer.c
    src/write_wav.c
)
target_include_directories(paloopback PRIVATE ..)
target_link_libraries(paloopback PortAudio)
if(UNIX)
  target_link_libraries(paloopback m)
endif()
